/* Native update mechanics; shared lifecycle policy runs outside GTK.
 */
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "terminal-build-id.h"
#include "terminal-screen.h"
#include "terminal-update.h"
#include "terminal-window.h"

#define UPDATE_PATH "/org/xfce/Terminal/Update"
#define UPDATE_INTERFACE "org.xfce.Terminal.Update1"
#define PENDING_KEY "terminal-update-in-place"

typedef struct
{
  gatomicrefcount refs;
  GtkWindow *window;
  GtkWidget *dialog;
  TerminalBuildMonitor *monitor;
  GCancellable *cancel;
  GCancellable *helper_exited;
  GDBusConnection *connection;
  GSubprocess *helper;
  gchar *service, *owner, *uuid, *operation, *title;
  gchar *executable, *build_id, *description, *log_path;
  gulong destroy_id;
  GPtrArray *held_screens;
  gboolean preserves_processes, destroyed, handoff, confirmed;
} Update;

typedef struct
{
  TerminalScreen *screen;
  gboolean hold, input, destroyed;
  gulong destroy_id;
} HeldScreen;

static void
held_screen_destroyed (GtkWidget *widget, HeldScreen *held)
{
  held->destroyed = TRUE;
}

static void
release_screen (gpointer data)
{
  HeldScreen *held = data;
  if (!held->destroyed)
    {
      terminal_screen_restart_hold (held->screen, held->hold);
      terminal_screen_set_input_enabled (held->screen, held->input);
    }
  if (g_signal_handler_is_connected (held->screen, held->destroy_id))
    g_signal_handler_disconnect (held->screen, held->destroy_id);
  g_object_unref (held->screen);
  g_free (held);
}

static Update *
update_ref (Update *update)
{
  g_atomic_ref_count_inc (&update->refs);
  return update;
}

static void
cancel_helper (Update *update)
{
  if (update->connection != NULL && update->owner != NULL && !update->handoff)
    g_dbus_connection_call (update->connection, update->owner, UPDATE_PATH,
                            UPDATE_INTERFACE, "Cancel", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL,
                            NULL);
}

static void
update_unref (gpointer data)
{
  Update *update = data;
  if (!g_atomic_ref_count_dec (&update->refs))
    return;
  cancel_helper (update);
  /* An interrupted child may exit after the helper has reported failure. Keep
   * that recovery tab open, but restore input when releasing the pending update. */
  if (update->held_screens != NULL && !update->handoff)
    for (guint i = 0; i < update->held_screens->len; i++)
      ((HeldScreen *) g_ptr_array_index (update->held_screens, i))->hold = TRUE;
  g_clear_pointer (&update->held_screens, g_ptr_array_unref);
  g_object_set_data (G_OBJECT (update->window), PENDING_KEY, NULL);
  if (g_signal_handler_is_connected (update->window, update->destroy_id))
    g_signal_handler_disconnect (update->window, update->destroy_id);
  g_object_unref (update->window);
  g_object_unref (update->monitor);
  g_object_unref (update->cancel);
  g_clear_object (&update->helper_exited);
  g_clear_object (&update->connection);
  g_clear_object (&update->helper);
  g_free (update->service);
  g_free (update->owner);
  g_free (update->uuid);
  g_free (update->operation);
  g_free (update->title);
  g_free (update->executable);
  g_free (update->build_id);
  g_free (update->description);
  g_free (update->log_path);
  g_free (update);
}

static gboolean
fail (GError **error, const gchar *message)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
  return FALSE;
}

static const gchar *
string_member (JsonObject *object, const gchar *name)
{
  JsonNode *node = json_object_get_member (object, name);
  return node != NULL && JSON_NODE_HOLDS_VALUE (node)
             && json_node_get_value_type (node) == G_TYPE_STRING
           ? json_node_get_string (node)
           : NULL;
}

static JsonNode *
call (Update *update, const gchar *method, GVariant *parameters, GError **error)
{
  GVariant *reply = g_dbus_connection_call_sync (
    update->connection, update->owner, UPDATE_PATH, UPDATE_INTERFACE, method,
    parameters, G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
    5000, update->cancel, error);
  if (reply == NULL)
    return NULL;
  const gchar *json;
  g_variant_get (reply, "(&s)", &json);
  JsonParser *parser = json_parser_new ();
  JsonNode *result = NULL;
  if (strlen (json) > 65536)
    fail (error,
          _ ("The terminal update helper returned an oversized reply."));
  else if (json_parser_load_from_data (parser, json, -1, error))
    {
      JsonNode *root = json_parser_get_root (parser);
      JsonNode *pid = root != NULL && JSON_NODE_HOLDS_OBJECT (root)
                        ? json_object_get_member (
                            json_node_get_object (root), "owner_pid")
                        : NULL;
      if (root != NULL && JSON_NODE_HOLDS_OBJECT (root)
          && g_strcmp0 (
               string_member (json_node_get_object (root), "operation_id"),
               update->operation)
               == 0
          && g_strcmp0 (
               string_member (json_node_get_object (root), "window_uuid"),
               update->uuid)
               == 0
          && pid != NULL && JSON_NODE_HOLDS_VALUE (pid)
          && json_node_get_value_type (pid) == G_TYPE_INT64
          && json_node_get_int (pid) == getpid ())
        result = json_node_copy (root);
      else
        fail (
          error,
          _ ("The terminal update helper returned a mismatched operation."));
    }
  g_object_unref (parser);
  g_variant_unref (reply);
  return result;
}

static gchar *
resolve_executable (Update *update)
{
  gchar *path = terminal_build_monitor_resolve_executable (update->monitor);
  gchar *resolved = path != NULL ? realpath (path, NULL) : NULL;
  g_free (path);
  return resolved;
}

static gboolean
wait_for_state (Update *update, gboolean preparing, GError **error)
{
  gint64 deadline = g_get_monotonic_time () + (preparing ? 90 : 300) * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline)
    {
      JsonNode *node = call (update, "GetStatus", NULL, error);
      if (node == NULL)
        return FALSE;
      JsonObject *status = json_node_get_object (node);
      const gchar *phase = string_member (status, "phase");
      gboolean done = g_strcmp0 (phase, preparing ? "ready" : "complete") == 0;
      if (g_strcmp0 (phase, "failed") == 0)
        fail (error, string_member (status, "message") != NULL
                       ? string_member (status, "message")
                       : _ ("Terminal update failed."));
      else if (done && preparing)
        {
          JsonNode *preserves = json_object_get_member (status, "preserves_processes");
          if (preserves == NULL
              || json_node_get_value_type (preserves) != G_TYPE_BOOLEAN)
            fail (error, _ ("Invalid terminal update confirmation plan."));
          else
            {
              update->preserves_processes = json_node_get_boolean (preserves);
              update->description = g_strdup (string_member (status, "message"));
            }
        }
      else if (!done && g_strcmp0 (phase, "preparing") != 0
               && g_strcmp0 (phase, "ready") != 0
               && g_strcmp0 (phase, "running") != 0
               && g_strcmp0 (phase, "closing") != 0)
        fail (error, _ ("Unexpected terminal update phase."));
      json_node_free (node);
      if (*error != NULL || done)
        return *error == NULL;
      g_usleep (100 * 1000);
    }
  return fail (error, _ ("The terminal update helper timed out; no automatic "
                         "retry was performed."));
}

static void
helper_finished (GObject *source, GAsyncResult *result, gpointer data)
{
  g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);
  g_cancellable_cancel (G_CANCELLABLE (data));
  g_object_unref (data);
}

static gboolean
spawn_helper (Update *update, GError **error)
{
  gchar *executable = g_file_read_link ("/proc/self/exe", NULL);
  gchar *parent = executable != NULL ? g_path_get_dirname (executable) : NULL;
  gchar *relative = parent != NULL ? g_build_filename (parent, "..", "..", "terminal", "update", "terminal-update-helper.py", NULL) : NULL;
  const gchar *helper = relative != NULL && g_file_test (relative, G_FILE_TEST_IS_REGULAR) ? relative
                        : g_file_test (TERMINAL_UPDATE_HELPER_INSTALLED, G_FILE_TEST_IS_REGULAR)
                          ? TERMINAL_UPDATE_HELPER_INSTALLED
                          : TERMINAL_UPDATE_HELPER_BUILD;
  g_free (executable);
  g_free (parent);
  if (!g_file_test (helper, G_FILE_TEST_IS_REGULAR))
    {
      g_free (relative);
      return fail (error, _("The terminal's bundled update helper is missing."));
    }
  JsonObject *request = json_object_new ();
  json_object_set_int_member (request, "owner_pid", getpid ());
  json_object_set_string_member (
    request, "owner_bus",
    g_dbus_connection_get_unique_name (update->connection));
  json_object_set_string_member (request, "window_uuid", update->uuid);
  json_object_set_string_member (request, "window_title", update->title);
  json_object_set_string_member (request, "executable", update->executable);
  json_object_set_string_member (
    request, "launch_path",
    terminal_build_monitor_get_launch_path (update->monitor));
  json_object_set_string_member (
    request, "launch_kind",
    terminal_build_monitor_get_launch_kind (update->monitor)
        == TERMINAL_BUILD_LAUNCH_REPO_WRAPPER
      ? "repo-wrapper"
      : "executable");
  json_object_set_string_member (request, "job_id", update->operation);
  json_object_set_string_member (request, "service", update->service);
  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (node, request);
  JsonGenerator *generator = json_generator_new ();
  json_generator_set_root (generator, node);
  gchar *json = json_generator_to_data (generator, NULL);
  gchar *state = g_getenv ("XDG_STATE_HOME") != NULL
                   ? g_strdup (g_getenv ("XDG_STATE_HOME"))
                   : g_build_filename (g_get_home_dir (), ".local", "state", NULL);
  gchar *directory = g_build_filename (state, "xfce4-terminal", "updates", NULL);
  g_free (state);
  gboolean writable = g_mkdir_with_parents (directory, 0700) == 0
                      && g_chmod (directory, 0700) == 0;
  if (writable)
    {
      update->log_path = g_strdup_printf ("%s/%s.log", directory, update->operation);
      GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
      g_subprocess_launcher_set_stderr_file_path (launcher, update->log_path);
      g_subprocess_launcher_unsetenv (launcher, "XDG_ACTIVATION_TOKEN");
      g_subprocess_launcher_unsetenv (launcher, "DESKTOP_STARTUP_ID");
      /* The worker and replacement must not die with the old app service. */
      gchar *scope = g_strconcat ("xfce-terminal-update-", update->operation, NULL);
      update->helper = g_subprocess_launcher_spawn (
        launcher, error, "systemd-run", "--user", "--scope", "--collect",
        "--quiet", "--expand-environment=no", "--unit", scope, "--", TERMINAL_UPDATE_PYTHON, helper,
        "--serve", json, NULL);
      if (update->helper != NULL)
        {
          update->helper_exited = g_cancellable_new ();
          g_subprocess_wait_async (update->helper, NULL, helper_finished,
                                   g_object_ref (update->helper_exited));
        }
      g_free (scope);
      g_object_unref (launcher);
    }
  else
    fail (error, _ ("The terminal update recovery directory is unavailable."));
  g_free (directory);
  g_free (json);
  json_node_free (node);
  g_object_unref (generator);
  g_free (relative);
  return update->helper != NULL;
}

static void
prepare_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  Update *update = data;
  GError *error = NULL;
  update->executable = resolve_executable (update);
  update->build_id = update->executable != NULL
                       ? terminal_build_id_read (update->executable)
                       : NULL;
  if (update->build_id == NULL)
    fail (&error,
          _ ("The latest terminal executable has no usable ELF build ID."));
  else if ((update->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, cancel, &error))
             != NULL
           && spawn_helper (update, &error))
    {
      gint64 deadline = g_get_monotonic_time () + 8 * G_TIME_SPAN_SECOND;
      while (g_get_monotonic_time () < deadline
             && !g_cancellable_is_cancelled (cancel)
             && !g_cancellable_is_cancelled (update->helper_exited))
        {
          GVariant *reply = g_dbus_connection_call_sync (
            update->connection, "org.freedesktop.DBus",
            "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
            g_variant_new ("(s)", update->service), G_VARIANT_TYPE ("(s)"),
            0, 1000, cancel, NULL);
          if (reply != NULL)
            {
              g_variant_get (reply, "(s)", &update->owner);
              g_variant_unref (reply);
              break;
            }
          g_usleep (100 * 1000);
        }
      if (update->owner == NULL)
        {
          gchar bytes[8192];
          FILE *stream = g_fopen (update->log_path, "rb");
          gsize length = stream != NULL ? fread (bytes, 1, sizeof bytes, stream) : 0;
          if (stream != NULL)
            fclose (stream);
          gchar *details = g_utf8_make_valid (bytes, length);
          gchar *message = g_strdup_printf (
            _ ("The terminal update helper could not start. It requires "
                 "Python 3, PyGObject, KWin and a systemd user session.\n%s"),
                                            details != NULL ? details : "");
          fail (&error, message);
          g_free (message);
          g_free (details);
        }
      else
        wait_for_state (update, TRUE, &error);
    }
  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
start_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  Update *update = data;
  GError *error = NULL;
  gchar *path = resolve_executable (update);
  gchar *id = path != NULL ? terminal_build_id_read (path) : NULL;
  if (g_strcmp0 (path, update->executable) != 0
      || g_strcmp0 (id, update->build_id) != 0)
    fail (&error, _ ("The launch target changed after preview; original left "
                     "untouched."));
  else
    {
      JsonNode *node = call (update, "Confirm", g_variant_new ("(b)", TRUE), &error);
      if (node != NULL)
        {
          json_node_free (node);
          wait_for_state (update, FALSE, &error);
        }
    }
  g_free (path);
  g_free (id);
  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
show_error (Update *update, GError *error)
{
  if (!update->destroyed
      && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      GtkWidget *dialog = gtk_message_dialog_new (
        update->window, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
        GTK_BUTTONS_CLOSE, "%s", _ ("Update in Place"));
      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                "%s", error->message);
      g_signal_connect_swapped (dialog, "response",
                                G_CALLBACK (gtk_widget_destroy), dialog);
      gtk_widget_show (dialog);
    }
}

static void
start_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Update *update = g_task_get_task_data (G_TASK (result));
  GError *error = NULL;
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      show_error (update, error);
      g_error_free (error);
    }
}

static void
run_worker (Update *update, GTaskThreadFunc worker, GAsyncReadyCallback callback)
{
  GTask *task = g_task_new (NULL, update->cancel, callback, NULL);
  g_task_set_task_data (task, update_ref (update), update_unref);
  g_task_run_in_thread (task, worker);
  g_object_unref (task);
}

static void
confirmed_dialog_destroyed (GtkWidget *dialog, Update *update)
{
  update->dialog = NULL;
  g_object_unref (dialog);
  update_unref (update);
}

static void
confirm_response (GtkDialog *dialog, gint response, Update *update)
{
  update_ref (update);
  if (response == GTK_RESPONSE_ACCEPT && !update->destroyed)
    run_worker (update, start_worker, start_done);
  else
    cancel_helper (update);
  gtk_widget_destroy (GTK_WIDGET (dialog));
  update_unref (update);
}

static void
prepare_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Update *update = g_task_get_task_data (G_TASK (result));
  GError *error = NULL;
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      show_error (update, error);
      g_error_free (error);
    }
  else if (!update->destroyed)
    {
      if (update->preserves_processes || update->confirmed)
        run_worker (update, start_worker, start_done);
      else
        {
          update->dialog = gtk_message_dialog_new (
            update->window,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
            _ ("Restart the programs in this terminal window?"));
          g_object_ref_sink (update->dialog);
          gtk_window_set_title (GTK_WINDOW (update->dialog),
                                _ ("Update in Place"));
          gtk_message_dialog_format_secondary_text (
            GTK_MESSAGE_DIALOG (update->dialog),
            _ ("%s\n\nAll tabs will be replaced at the same position. The terminal "
                 "and supported applications use the latest installed builds. "
                 "tmux clients reconnect; their servers and pane applications "
                 "stay running. Other programs restart or resume. "
                 "Scrollback, shell variables and unsaved/in-flight non-tmux "
                 "work are lost. "
                 "Non-tmux programs relaunch only after their originals exit. "
                 "Recovery commands are saved under "
                 "~/.local/state/xfce4-terminal/updates/."),
            update->description != NULL ? update->description : "");
          gtk_dialog_add_buttons (GTK_DIALOG (update->dialog), _ ("_Cancel"),
                                  GTK_RESPONSE_CANCEL, _ ("_Update in Place"),
                                  GTK_RESPONSE_ACCEPT, NULL);
          gtk_dialog_set_default_response (GTK_DIALOG (update->dialog),
                                           GTK_RESPONSE_CANCEL);
          g_signal_connect (update->dialog, "response",
                            G_CALLBACK (confirm_response), update);
          g_signal_connect (update->dialog, "destroy",
                            G_CALLBACK (confirmed_dialog_destroyed),
                            update_ref (update));
          gtk_widget_show (update->dialog);
        }
    }
}

static void
window_destroyed (GtkWidget *window, Update *update)
{
  update->destroyed = TRUE;
  cancel_helper (update);
  g_cancellable_cancel (update->cancel);
  if (update->dialog != NULL)
    gtk_widget_destroy (update->dialog);
}

gboolean
terminal_update_is_pending (GtkWindow *window)
{
  return g_object_get_data (G_OBJECT (window), PENDING_KEY) != NULL
         || g_getenv ("XFCE4_TERMINAL_UPDATE_SERVICE") != NULL;
}

const gchar *
terminal_update_get_service (GtkWindow *window)
{
  Update *update = g_object_get_data (G_OBJECT (window), PENDING_KEY);
  return update != NULL ? update->service
         : g_getenv ("XFCE4_TERMINAL_UPDATE_SERVICE") != NULL
           ? g_getenv ("XFCE4_TERMINAL_UPDATE_SERVICE")
           : "";
}

gboolean
terminal_update_hold (GtkWindow *window, const gchar *service, const gchar *sender, gboolean hold)
{
  Update *update = g_object_get_data (G_OBJECT (window), PENDING_KEY);
  if (update == NULL || update->destroyed || update->handoff || update->owner == NULL
      || g_strcmp0 (service, update->service) != 0 || g_strcmp0 (sender, update->owner) != 0)
    return FALSE;
  if (!hold)
    {
      g_clear_pointer (&update->held_screens, g_ptr_array_unref);
      return TRUE;
    }
  if (update->held_screens != NULL)
    return TRUE;
  update->held_screens = g_ptr_array_new_with_free_func (release_screen);
  GList *tabs = gtk_container_get_children (GTK_CONTAINER (terminal_window_get_notebook (TERMINAL_WINDOW (window))));
  for (GList *link = tabs; link != NULL; link = link->next)
    {
      HeldScreen *held = g_new0 (HeldScreen, 1);
      held->screen = g_object_ref (TERMINAL_SCREEN (link->data));
      held->input = terminal_screen_get_input_enabled (held->screen);
      held->hold = terminal_screen_restart_hold (held->screen, TRUE);
      held->destroy_id = g_signal_connect (held->screen, "destroy", G_CALLBACK (held_screen_destroyed), held);
      terminal_screen_set_input_enabled (held->screen, FALSE);
      g_ptr_array_add (update->held_screens, held);
    }
  g_list_free (tabs);
  return TRUE;
}

gboolean
terminal_update_authorize_close (GtkWindow *window, const gchar *service, const gchar *sender)
{
  Update *update = g_object_get_data (G_OBJECT (window), PENDING_KEY);
  if (update == NULL || update->destroyed || update->handoff
      || update->owner == NULL || g_strcmp0 (service, update->service) != 0
      || g_strcmp0 (sender, update->owner) != 0)
    return FALSE;
  update->handoff = TRUE;
  return TRUE;
}

static void
terminal_update_begin (GtkWindow *window, const gchar *window_uuid, TerminalBuildMonitor *monitor, gboolean confirmed)
{
  g_return_if_fail (GTK_IS_WINDOW (window));
  g_return_if_fail (window_uuid != NULL
                    && g_uuid_string_is_valid (window_uuid));
  g_return_if_fail (TERMINAL_IS_BUILD_MONITOR (monitor));
  if (!terminal_build_monitor_has_update (monitor)
      || terminal_update_is_pending (window)
      || gtk_widget_in_destruction (GTK_WIDGET (window)))
    return;
  Update *update = g_new0 (Update, 1);
  g_atomic_ref_count_init (&update->refs);
  update->window = g_object_ref (window);
  update->uuid = g_strdup (window_uuid);
  update->operation = g_uuid_string_random ();
  gchar *compact = g_strdelimit (g_strdup (update->operation), "-", '_');
  update->service = g_strconcat ("org.xfce.Terminal5.Update.i", compact, NULL);
  g_free (compact);
  update->title = g_strdup (gtk_window_get_title (window) != NULL
                              ? gtk_window_get_title (window)
                              : "");
  update->monitor = g_object_ref (monitor);
  update->confirmed = confirmed;
  update->cancel = g_cancellable_new ();
  g_object_set_data (G_OBJECT (window), PENDING_KEY, update);
  update->destroy_id = g_signal_connect (
    window, "destroy", G_CALLBACK (window_destroyed), update);
  run_worker (update, prepare_worker, prepare_done);
  update_unref (update);
}

void
terminal_update_in_place (GtkWindow *window, const gchar *window_uuid, TerminalBuildMonitor *monitor)
{
  terminal_update_begin (window, window_uuid, monitor, FALSE);
}

void
terminal_update_in_place_confirmed (GtkWindow *window, const gchar *window_uuid, TerminalBuildMonitor *monitor)
{
  terminal_update_begin (window, window_uuid, monitor, TRUE);
}
