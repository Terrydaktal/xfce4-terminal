/* A prepared tab keeps its native window alive while its child is replaced.
 * Preparation never sends a signal. Commit is a separate, owner-bound action. */
#include <errno.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "terminal-restart.h"
#include "terminal-update.h"

typedef struct
{
  gchar *owner, *token, *cwd, **argv;
  const gchar *phase, *message;
  GPid child;
  struct stat old_exe, new_exe;
  gboolean hold, input;
  gint64 deadline;
  guint timer;
} Restart;

static void
restart_free (gpointer data)
{
  Restart *restart = data;
  if (restart->timer != 0)
    g_source_remove (restart->timer);
  g_free (restart->owner);
  g_free (restart->token);
  g_free (restart->cwd);
  g_strfreev (restart->argv);
  g_free (restart);
}

static gboolean
same_file (const struct stat *a, const struct stat *b)
{
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino
         && a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec
         && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

gboolean
terminal_restart_is_pending (TerminalScreen *screen)
{
  Restart *restart = g_object_get_data (G_OBJECT (screen), "terminal-tab-restart");
  return restart != NULL && (g_str_equal (restart->phase, "prepared") || g_str_equal (restart->phase, "stopping") || g_str_equal (restart->phase, "spawning"));
}

static gboolean
restart_poll (gpointer data)
{
  TerminalScreen *screen = data;
  Restart *restart = g_object_get_data (G_OBJECT (screen), "terminal-tab-restart");
  GPid child = terminal_screen_get_child_pid (screen);
  if (gtk_widget_in_destruction (GTK_WIDGET (screen)))
    {
      restart->phase = "failed";
      restart->message = "The terminal tab was closed";
    }
  else if (g_get_monotonic_time () >= restart->deadline)
    {
      if (g_str_equal (restart->phase, "prepared") && child > 0)
        terminal_screen_restart_hold (screen, restart->hold);
      restart->phase = "failed";
      restart->message = "Tab replacement expired; no force-kill was attempted";
    }
  else if (g_str_equal (restart->phase, "stopping") && child <= 0)
    {
      restart->phase = "spawning";
      restart->message = "Starting the replacement in the existing tab";
      terminal_screen_replace_child (screen, restart->argv, restart->cwd);
    }
  else if (g_str_equal (restart->phase, "spawning") && child > 0)
    {
      gchar *path = g_strdup_printf ("/proc/%d/exe", child);
      struct stat actual;
      gboolean exact = stat (path, &actual) == 0 && same_file (&actual, &restart->new_exe);
      g_free (path);
      restart->phase = exact ? "complete" : "failed";
      restart->message = exact ? "Program replaced in the same terminal tab"
                               : "The replacement executable did not match the prepared binary";
      if (exact)
        terminal_screen_restart_hold (screen, restart->hold);
    }
  if (!terminal_restart_is_pending (screen))
    {
      terminal_screen_set_input_enabled (screen, restart->input);
      restart->timer = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

gchar *
terminal_restart_prepare (TerminalScreen *screen, const gchar *owner, guint child, gchar **argv, const gchar *cwd, GError **error)
{
  GtkWidget *window = gtk_widget_get_toplevel (GTK_WIDGET (screen));
  guint argc = argv != NULL ? g_strv_length (argv) : 0;
  gsize bytes = 0;
  for (guint i = 0; i < argc; i++)
    bytes += strlen (argv[i]);
  struct stat old_exe, new_exe;
  gchar *path = g_strdup_printf ("/proc/%u/exe", child);
  gboolean valid = child > 1 && terminal_screen_get_child_pid (screen) == (GPid) child
                   && !terminal_restart_is_pending (screen) && !terminal_update_is_pending (GTK_WINDOW (window))
                   && terminal_screen_get_input_enabled (screen) && argc > 0 && argc <= 128 && bytes <= 65536
                   && g_path_is_absolute (argv[0]) && g_path_is_absolute (cwd)
                   && g_file_test (cwd, G_FILE_TEST_IS_DIR) && stat (path, &old_exe) == 0
                   && stat (argv[0], &new_exe) == 0 && S_ISREG (new_exe.st_mode) && access (argv[0], X_OK) == 0;
  g_free (path);
  if (!valid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Tab changed, is busy, or replacement arguments are invalid");
      return NULL;
    }
  Restart *restart = g_new0 (Restart, 1);
  restart->owner = g_strdup (owner);
  restart->token = g_uuid_string_random ();
  restart->cwd = g_strdup (cwd);
  restart->argv = g_strdupv (argv);
  restart->child = child;
  restart->old_exe = old_exe;
  restart->new_exe = new_exe;
  restart->phase = "prepared";
  restart->message = "Prepared; no program has been stopped";
  restart->deadline = g_get_monotonic_time () + 300 * G_TIME_SPAN_SECOND;
  restart->hold = terminal_screen_restart_hold (screen, TRUE);
  restart->input = terminal_screen_get_input_enabled (screen);
  terminal_screen_set_input_enabled (screen, FALSE);
  g_object_set_data_full (G_OBJECT (screen), "terminal-tab-restart", restart, restart_free);
  restart->timer = g_timeout_add_full (G_PRIORITY_DEFAULT, 50, restart_poll, g_object_ref (screen), g_object_unref);
  return g_strdup (restart->token);
}

static Restart *
lookup (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error)
{
  Restart *restart = g_object_get_data (G_OBJECT (screen), "terminal-tab-restart");
  if (restart == NULL || g_strcmp0 (restart->owner, owner) != 0 || g_strcmp0 (restart->token, token) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "Only the preparing connection can use this tab replacement token");
      return NULL;
    }
  return restart;
}

gboolean
terminal_restart_commit (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error)
{
  Restart *restart = lookup (screen, owner, token, error);
  if (restart == NULL)
    return FALSE;
  if (!g_str_equal (restart->phase, "prepared"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "The tab replacement is not prepared");
      return FALSE;
    }
  struct stat actual;
  GPid child = terminal_screen_get_child_pid (screen);
  gchar *path = g_strdup_printf ("/proc/%d/exe", child);
  gboolean exited = child <= 0;
  gboolean same_child = FALSE;
  if (child > 0 && child == restart->child)
    {
      if (stat (path, &actual) == 0)
        same_child = same_file (&actual, &restart->old_exe);
      else if (errno == ENOENT)
        /* The child watch can lag behind an already completed graceful exit. */
        exited = TRUE;
    }
  gboolean valid = stat (restart->argv[0], &actual) == 0 && same_file (&actual, &restart->new_exe)
                   && (exited || same_child);
  g_free (path);
  if (!valid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Program or installed binary changed; replacement refused");
      return FALSE;
    }
  restart->phase = "stopping";
  restart->message = "Waiting for the previous terminal child to exit";
  restart->deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  if (!exited && kill (child, SIGHUP) != 0 && errno != ESRCH)
    {
      restart->phase = "failed";
      restart->message = "Could not request terminal child shutdown";
    }
  return TRUE;
}

gboolean
terminal_restart_cancel (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error)
{
  Restart *restart = lookup (screen, owner, token, error);
  if (restart == NULL)
    return FALSE;
  if (!g_str_equal (restart->phase, "prepared"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "A committed replacement cannot be cancelled");
      return FALSE;
    }
  if (terminal_screen_get_child_pid (screen) > 0)
    terminal_screen_restart_hold (screen, restart->hold);
  restart->phase = "cancelled";
  restart->message = "Cancelled before tab replacement";
  terminal_screen_set_input_enabled (screen, restart->input);
  return TRUE;
}

gchar *
terminal_restart_status (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error)
{
  Restart *restart = lookup (screen, owner, token, error);
  if (restart == NULL)
    return NULL;
  JsonBuilder *builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "phase");
  json_builder_add_string_value (builder, restart->phase);
  json_builder_set_member_name (builder, "message");
  json_builder_add_string_value (builder, restart->message);
  json_builder_set_member_name (builder, "child_pid");
  json_builder_add_int_value (builder, terminal_screen_get_child_pid (screen));
  json_builder_end_object (builder);
  JsonNode *root = json_builder_get_root (builder);
  JsonGenerator *generator = json_generator_new ();
  json_generator_set_root (generator, root);
  gchar *json = json_generator_to_data (generator, NULL);
  g_object_unref (generator);
  json_node_free (root);
  g_object_unref (builder);
  return json;
}
