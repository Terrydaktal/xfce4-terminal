/* Check the preference UI and actual process reuse on the suite's private bus. */
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "terminal/terminal-config.h"
#include "terminal/terminal-options.h"
#include "terminal/terminal-preferences-dialog.h"
#include "test-terminal-launching.h"

#define LAUNCH_PROPERTY "/misc-run-in-separate-process"
#define LAUNCH_LABEL "Start each terminal launch in a separate process"

static void
dispatch_events (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

typedef struct
{
  gboolean done;
  gchar *output;
} QueryResult;

static void
query_finished (GObject *object, GAsyncResult *result, gpointer data)
{
  QueryResult *query = data;
  GError *error = NULL;

  g_assert_true (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (object), result,
                                                    &query->output, NULL, &error));
  g_assert_no_error (error);
  query->done = TRUE;
}

static gchar *
query_setting (const gchar *value)
{
  const gchar *argv[] = {
    "xfconf-query", "-c", "xfce4-terminal", "-p", LAUNCH_PROPERTY,
    value != NULL ? "-n" : NULL, "-t", "bool", "-s", value, NULL
  };
  GError *error = NULL;
  GSubprocess *process;
  QueryResult query = { 0 };

  process = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
  g_assert_no_error (error);
  /* Keep dispatching Xfconf write acknowledgements while the other process reads. */
  g_subprocess_communicate_utf8_async (process, NULL, NULL, query_finished, &query);
  while (!query.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (g_subprocess_get_successful (process));
  g_object_unref (process);
  return g_strstrip (query.output);
}

static void
set_setting (gboolean separate)
{
  g_free (query_setting (separate ? "true" : "false"));
  dispatch_events ();
}

static void
assert_saved_setting (gboolean separate)
{
  const gchar *expected = separate ? "true" : "false";
  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
  gchar *actual;

  do
    {
      dispatch_events ();
      actual = query_setting (NULL);
      if (g_str_equal (actual, expected) || g_get_monotonic_time () >= deadline)
        break;
      g_free (actual);
      g_usleep (1000);
    }
  while (TRUE);
  g_assert_cmpstr (actual, ==, expected);
  g_free (actual);
}

static GtkWidget *
find_widget (GtkWidget *widget, gboolean checkbox, const gchar *text)
{
  GtkWidget *match = NULL;

  if (checkbox && GTK_IS_CHECK_BUTTON (widget))
    {
      GtkWidget *label = gtk_bin_get_child (GTK_BIN (widget));
      if (GTK_IS_LABEL (label) && g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), text) == 0)
        return widget;
    }
  if (!checkbox && GTK_IS_LABEL (widget)
      && g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), text) == 0)
    return widget;
  if (GTK_IS_CONTAINER (widget))
    {
      GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
      for (GList *child = children; child != NULL && match == NULL; child = child->next)
        match = find_widget (child->data, checkbox, text);
      g_list_free (children);
    }
  return match;
}

static void
preference_dialog (void)
{
  TerminalPreferences *preferences = terminal_preferences_get ();
  GParamSpec *spec = g_object_class_find_property (G_OBJECT_GET_CLASS (preferences), LAUNCH_PROPERTY + 1);
  GtkWidget *dialog;
  GtkWidget *button;
  gboolean active;

  g_assert_nonnull (spec);
  g_assert_true (G_IS_PARAM_SPEC_BOOLEAN (spec));
  g_assert_false (G_PARAM_SPEC_BOOLEAN (spec)->default_value);
  g_object_set (preferences, LAUNCH_PROPERTY + 1, FALSE, NULL);
  assert_saved_setting (FALSE);
  dialog = terminal_preferences_dialog_new (TRUE, FALSE);
  button = find_widget (dialog, TRUE, LAUNCH_LABEL);
  g_assert_nonnull (button);
  g_assert_nonnull (find_widget (dialog, FALSE, "Launching"));
  GtkWidget *page = button;
  while (page != NULL && !GTK_IS_NOTEBOOK (gtk_widget_get_parent (page)))
    page = gtk_widget_get_parent (page);
  g_assert_nonnull (page);
  g_assert_cmpstr (gtk_notebook_get_tab_label_text (GTK_NOTEBOOK (gtk_widget_get_parent (page)), page), ==, "Advanced");
  g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)));

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
  g_object_get (preferences, LAUNCH_PROPERTY + 1, &active, NULL);
  g_assert_true (active);
  assert_saved_setting (TRUE);
  gtk_widget_destroy (dialog);

  dialog = terminal_preferences_dialog_new (TRUE, FALSE);
  button = find_widget (dialog, TRUE, LAUNCH_LABEL);
  g_assert_nonnull (button);
  g_assert_true (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)));
  g_object_set (preferences, LAUNCH_PROPERTY + 1, FALSE, NULL);
  g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)));
  assert_saved_setting (FALSE);
  gtk_widget_destroy (dialog);
  g_object_unref (preferences);
}

typedef struct
{
  const gchar *name;
  const gchar *command;
  gboolean explicit_mode;
  gboolean separate;
} OptionCase;

static void
option_parser (gconstpointer data)
{
  const OptionCase *test = data;
  TerminalOptions options = { 0 };
  gchar **argv = g_strsplit (test->command, " ", -1);

  terminal_options_parse (g_strv_length (argv), argv, &options);
  g_assert_cmpint (options.server_mode_set, ==, test->explicit_mode);
  g_assert_cmpint (options.disable_server, ==, test->separate);
  g_strfreev (argv);
}

static void
window_parser (void)
{
  gchar *argv[] = { "xfce4-terminal", "--enable-server", "--disable-server", NULL };
  GError *error = NULL;
  GSList *windows = terminal_window_attr_parse (3, argv, FALSE, &error);

  g_assert_no_error (error);
  g_assert_cmpuint (g_slist_length (windows), ==, 1);
  g_slist_free_full (windows, (GDestroyNotify) terminal_window_attr_free);
}

typedef struct
{
  const gchar *name;
  gboolean saved_separate;
  const gchar *flags[3];
  gboolean separate;
} LaunchCase;

static GSubprocess *
start_terminal (const gchar *const *flags)
{
  const gchar *binary = g_getenv ("TEST_TERMINAL_BINARY");
  GPtrArray *argv = g_ptr_array_new ();
  GError *error = NULL;
  GSubprocess *child;

  g_ptr_array_add (argv, (gpointer) (binary != NULL ? binary : TEST_TERMINAL_BINARY));
  for (guint i = 0; flags != NULL && flags[i] != NULL; i++)
    g_ptr_array_add (argv, (gpointer) flags[i]);
  g_ptr_array_add (argv, "--command");
  g_ptr_array_add (argv, "/bin/sleep 120");
  g_ptr_array_add (argv, NULL);
  child = g_subprocess_newv ((const gchar *const *) argv->pdata, G_SUBPROCESS_FLAGS_NONE, &error);
  g_assert_no_error (error);
  g_ptr_array_unref (argv);
  return child;
}

static guint
terminal_count (GDBusConnection *connection, guint *instances)
{
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync (connection,
    "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "ListNames", NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  gchar **names;
  guint tabs = 0;

  g_assert_no_error (error);
  g_variant_get (reply, "(^as)", &names);
  g_variant_unref (reply);
  *instances = 0;
  for (guint i = 0; names[i] != NULL; i++)
    if (g_str_has_prefix (names[i], TERMINAL_DBUS_INSTANCE_PREFIX))
      {
        reply = g_dbus_connection_call_sync (connection, names[i], TERMINAL_DBUS_PATH,
          TERMINAL_DBUS_INTERFACE, "ListTerminals", NULL, G_VARIANT_TYPE ("(aa{sv})"),
          G_DBUS_CALL_FLAGS_NONE, 250, NULL, NULL);
        if (reply != NULL)
          {
            GVariant *records = g_variant_get_child_value (reply, 0);
            tabs += g_variant_n_children (records);
            (*instances)++;
            g_variant_unref (records);
            g_variant_unref (reply);
          }
      }
  g_strfreev (names);
  return tabs;
}

static gboolean
wait_for_tabs (GDBusConnection *connection, guint expected, guint *instances)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  do
    {
      dispatch_events ();
      if (terminal_count (connection, instances) == expected)
        return TRUE;
      g_usleep (10000);
    }
  while (g_get_monotonic_time () < deadline);
  return FALSE;
}

static void
launch_processes (gconstpointer data)
{
  const LaunchCase *test = data;
  GError *error = NULL;
  GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  guint instances = 0;
  gboolean ready;
  GSubprocess *first;
  GSubprocess *second = NULL;

  g_assert_no_error (error);
  g_assert_true (wait_for_tabs (connection, 0, &instances));
  set_setting (FALSE);
  first = start_terminal (NULL);
  ready = wait_for_tabs (connection, 1, &instances) && instances == 1;
  if (ready)
    {
      set_setting (test->saved_separate);
      second = start_terminal (test->flags);
      ready = wait_for_tabs (connection, 2, &instances);
    }

  /* Clean up our processes before asserting, including on an expected red run. */
  if (second != NULL)
    {
      g_subprocess_force_exit (second);
      g_assert_true (g_subprocess_wait (second, NULL, &error));
      g_assert_no_error (error);
      g_object_unref (second);
    }
  g_subprocess_force_exit (first);
  g_assert_true (g_subprocess_wait (first, NULL, &error));
  g_assert_no_error (error);
  g_object_unref (first);
  guint remaining;
  gboolean cleaned = wait_for_tabs (connection, 0, &remaining);
  g_object_unref (connection);
  set_setting (FALSE);
  g_assert_true (cleaned);
  g_assert_true (ready);
  g_assert_cmpuint (instances, ==, test->separate ? 2 : 1);
}

void
terminal_launching_register_tests (void)
{
  static const OptionCase options[] = {
    { "default", "xfce4-terminal", FALSE, FALSE },
    { "disable", "xfce4-terminal --disable-server", TRUE, TRUE },
    { "enable", "xfce4-terminal --enable-server", TRUE, FALSE },
    { "last-enable", "xfce4-terminal --disable-server --enable-server", TRUE, FALSE },
    { "last-disable", "xfce4-terminal --enable-server --disable-server", TRUE, TRUE },
    { "execute-boundary", "xfce4-terminal --execute command --disable-server", FALSE, FALSE },
    { "short-execute-boundary", "xfce4-terminal -x command --enable-server", FALSE, FALSE },
    { "delimiter-boundary", "xfce4-terminal -- --disable-server", FALSE, FALSE },
  };
  static const LaunchCase launches[] = {
    { "reuse", FALSE, { NULL }, FALSE },
    { "separate", TRUE, { NULL }, TRUE },
    { "force-reuse", TRUE, { "--enable-server", NULL }, FALSE },
    { "force-separate", FALSE, { "--disable-server", NULL }, TRUE },
    { "last-enable", TRUE, { "--disable-server", "--enable-server", NULL }, FALSE },
    { "last-disable", FALSE, { "--enable-server", "--disable-server", NULL }, TRUE },
  };

  g_test_add_func ("/terminal/launching/preference-dialog", preference_dialog);
  g_test_add_func ("/terminal/launching/window-parser", window_parser);
  for (guint i = 0; i < G_N_ELEMENTS (options); i++)
    {
      gchar *name = g_strconcat ("/terminal/launching/options/", options[i].name, NULL);
      g_test_add_data_func (name, &options[i], option_parser);
      g_free (name);
    }
  for (guint i = 0; i < G_N_ELEMENTS (launches); i++)
    {
      gchar *name = g_strconcat ("/terminal/launching/processes/", launches[i].name, NULL);
      g_test_add_data_func (name, &launches[i], launch_processes);
      g_free (name);
    }
}
