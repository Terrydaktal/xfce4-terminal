/* Real GTK client, private bus and a mock of the terminal-owned worker. */
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <unistd.h>

#include "terminal/terminal-build-id.h"
#include "terminal/terminal-update.h"

#include "test-terminal-update.h"

typedef enum
{
  TMUX,
  CONFIRM_CANCEL,
  CONFIRM_ACCEPT,
  WRONG_UUID,
  WRONG_PID,
  WRONG_OPERATION,
  MALFORMED,
  REFUSED,
  RETARGET,
  CLOSE_PREPARING,
  CLOSE_CONFIRMING,
  NO_SERVICE,
  START_FAILED,
  COMPLETED_FAILURE,
  NO_UPDATE,
  UNAVAILABLE_TARGET
} Scenario;

typedef struct
{
  Scenario scenario;
  GtkWindow *window;
  TerminalBuildMonitor *monitor;
  gchar *directory, *alias, *uuid, *old_path;
} Fixture;

static const gchar *candidate;

static void
pump (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_usleep (1000);
}

static guint
count (Fixture *fixture, const gchar *key)
{
  gchar *path = g_build_filename (fixture->directory, "worker.json", NULL);
  JsonParser *parser = json_parser_new ();
  guint result = 0;
  if (json_parser_load_from_file (parser, path, NULL))
    result = json_object_get_int_member (
      json_node_get_object (json_parser_get_root (parser)), key);
  g_object_unref (parser);
  g_free (path);
  return result;
}

static GtkWidget *
find_dialog (Fixture *fixture)
{
  GList *windows = gtk_window_list_toplevels ();
  GtkWidget *result = NULL;
  for (GList *item = windows; item != NULL; item = item->next)
    if (GTK_IS_DIALOG (item->data)
        && gtk_window_get_transient_for (item->data) == fixture->window)
      result = item->data;
  g_list_free (windows);
  return result;
}

static void
setup (Fixture *fixture, gconstpointer data)
{
  GError *error = NULL;
  fixture->scenario = GPOINTER_TO_INT (data);
  fixture->uuid = g_uuid_string_random ();
  fixture->directory = g_dir_make_tmp ("terminal-owned-update.XXXXXX", &error);
  g_assert_no_error (error);
  fixture->alias = g_build_filename (fixture->directory, "xfce4-terminal", NULL);
  g_assert_cmpint (symlink (candidate, fixture->alias), ==, 0);
  fixture->monitor = terminal_build_monitor_new (
    fixture->scenario == NO_UPDATE ? candidate : "/usr/bin/true",
    fixture->alias, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  if (fixture->scenario != NO_UPDATE)
    {
      gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
      while (!terminal_build_monitor_has_update (fixture->monitor)
             && g_get_monotonic_time () < deadline)
        pump ();
      g_assert_true (terminal_build_monitor_has_update (fixture->monitor));
    }
  gchar *mock = NULL;
  gsize length;
  g_assert_true (
    g_file_get_contents (TEST_UPDATE_WORKER, &mock, &length, &error));
  gchar *front = g_build_filename (fixture->directory, "systemd-run", NULL);
  g_assert_true (g_file_set_contents (front, mock, length, &error));
  g_assert_cmpint (g_chmod (front, 0700), ==, 0);
  fixture->old_path = g_strdup (g_getenv ("PATH"));
  gchar *path = g_strconcat (fixture->directory, ":", fixture->old_path, NULL);
  gchar *scenario = g_strdup_printf ("%u", fixture->scenario);
  g_setenv ("PATH", path, TRUE);
  g_setenv ("TERMINAL_UPDATE_TEST_SCENARIO", scenario, TRUE);
  g_setenv ("TERMINAL_UPDATE_TEST_DIRECTORY", fixture->directory, TRUE);
  g_free (front);
  g_free (mock);
  g_free (path);
  g_free (scenario);
  fixture->window = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));
  g_object_ref_sink (fixture->window);
  gtk_widget_show (GTK_WIDGET (fixture->window));
}

static void
teardown (Fixture *fixture, gconstpointer data)
{
  GtkWidget *dialog = find_dialog (fixture);
  if (dialog != NULL)
    gtk_widget_destroy (dialog);
  gtk_widget_destroy (GTK_WIDGET (fixture->window));
  gint64 deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  while (terminal_update_is_pending (fixture->window)
         && g_get_monotonic_time () < deadline)
    pump ();
  g_assert_false (terminal_update_is_pending (fixture->window));
  deadline = g_get_monotonic_time () + 150 * 1000;
  while (g_get_monotonic_time () < deadline)
    pump ();
  g_object_unref (fixture->window);
  g_object_unref (fixture->monitor);
  g_setenv ("PATH", fixture->old_path, TRUE);
  g_unsetenv ("TERMINAL_UPDATE_TEST_SCENARIO");
  g_unsetenv ("TERMINAL_UPDATE_TEST_DIRECTORY");
  GDir *directory = g_dir_open (fixture->directory, 0, NULL);
  const gchar *name;
  while ((name = g_dir_read_name (directory)) != NULL)
    {
      gchar *path = g_build_filename (fixture->directory, name, NULL);
      g_unlink (path);
      g_free (path);
    }
  g_dir_close (directory);
  g_rmdir (fixture->directory);
  g_free (fixture->directory);
  g_free (fixture->alias);
  g_free (fixture->uuid);
  g_free (fixture->old_path);
}

static void
test_update (Fixture *fixture, gconstpointer data)
{
  terminal_update_in_place (fixture->window, fixture->uuid, fixture->monitor);
  terminal_update_in_place (fixture->window, fixture->uuid, fixture->monitor);
  g_assert_true (terminal_update_is_pending (fixture->window));
  g_assert_true (
    g_str_has_prefix (terminal_update_get_service (fixture->window),
                      "org.xfce.Terminal5.Update.i"));
  g_assert_false (terminal_update_authorize_close (
    fixture->window, "wrong-service", ":9999.9"));
  gint64 deadline = g_get_monotonic_time () + 12 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline)
    {
      pump ();
      if (find_dialog (fixture) != NULL
          || !terminal_update_is_pending (fixture->window)
          || (fixture->scenario == CLOSE_PREPARING
              && count (fixture, "reports") > 0))
        break;
    }
  GtkWidget *dialog = find_dialog (fixture);
  if (fixture->scenario == CONFIRM_CANCEL
      || fixture->scenario == CONFIRM_ACCEPT || fixture->scenario == RETARGET
      || fixture->scenario == CLOSE_CONFIRMING)
    {
      g_assert_nonnull (dialog);
      g_assert_cmpuint (count (fixture, "starts"), ==, 0);
      if (fixture->scenario == CLOSE_CONFIRMING)
        gtk_widget_destroy (GTK_WIDGET (fixture->window));
      else
        {
          if (fixture->scenario == RETARGET)
            {
              g_assert_cmpint (g_unlink (fixture->alias), ==, 0);
              g_assert_cmpint (symlink ("/usr/bin/true", fixture->alias), ==,
                               0);
            }
          gtk_dialog_response (GTK_DIALOG (dialog),
                               fixture->scenario == CONFIRM_CANCEL
                                 ? GTK_RESPONSE_CANCEL
                                 : GTK_RESPONSE_ACCEPT);
        }
    }
  else if (fixture->scenario == CLOSE_PREPARING)
    gtk_widget_destroy (GTK_WIDGET (fixture->window));
  else if (fixture->scenario != TMUX)
    g_assert_nonnull (dialog);
  deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  while (terminal_update_is_pending (fixture->window)
         && g_get_monotonic_time () < deadline)
    pump ();
  g_assert_false (terminal_update_is_pending (fixture->window));
  g_assert_cmpuint (count (fixture, "begins"), ==,
                    fixture->scenario == NO_SERVICE ? 0 : 1);
  g_assert_cmpuint (count (fixture, "starts"), ==,
                    fixture->scenario == TMUX
                        || fixture->scenario == CONFIRM_ACCEPT
                        || fixture->scenario == START_FAILED
                        || fixture->scenario == COMPLETED_FAILURE
                      ? 1
                      : 0);
  if (fixture->scenario != CLOSE_PREPARING
      && fixture->scenario != CLOSE_CONFIRMING)
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (fixture->window)));
}

static void
test_no_update (Fixture *fixture, gconstpointer data)
{
  if (fixture->scenario == UNAVAILABLE_TARGET)
    {
      g_assert_cmpint (g_unlink (fixture->alias), ==, 0);
      terminal_build_monitor_refresh (fixture->monitor);
      gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
      while (terminal_build_monitor_has_update (fixture->monitor)
             && g_get_monotonic_time () < deadline)
        pump ();
    }
  g_assert_false (terminal_build_monitor_has_update (fixture->monitor));
  terminal_update_in_place (fixture->window, fixture->uuid, fixture->monitor);
  g_assert_false (terminal_update_is_pending (fixture->window));
  g_assert_cmpuint (count (fixture, "begins"), ==, 0);
  g_assert_null (find_dialog (fixture));
}

void
terminal_update_register_tests (const char *executable)
{
  candidate = executable;
  static const gchar *names[] = { "tmux-preserves-programs",
                                  "cancel-non-tmux",
                                  "confirm-non-tmux",
                                  "wrong-uuid",
                                  "wrong-pid",
                                  "wrong-operation",
                                  "malformed-report",
                                  "refused-window",
                                  "launch-target-changed",
                                  "close-during-preparation",
                                  "close-during-confirmation",
                                  "helper-unavailable",
                                  "start-refused",
                                  "completed-with-failed-window" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
      gchar *path = g_strconcat ("/terminal/update/", names[i], NULL);
      g_test_add (path, Fixture, GUINT_TO_POINTER (i), setup, test_update,
                  teardown);
      g_free (path);
    }
  g_test_add ("/terminal/update/no-new-build", Fixture,
              GUINT_TO_POINTER (NO_UPDATE), setup, test_no_update, teardown);
  g_test_add ("/terminal/update/unavailable-target", Fixture,
              GUINT_TO_POINTER (UNAVAILABLE_TARGET), setup, test_no_update,
              teardown);
}
