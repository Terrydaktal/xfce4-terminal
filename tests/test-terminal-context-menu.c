/* Right-click routing through a real, private tmux client and VTE widget. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "terminal-test-utils.h"

static gchar *self;

static GtkWidget *
menu_requested (GtkWidget *widget G_GNUC_UNUSED,
                guint *count)
{
  (*count)++;
  return NULL;
}

static void
committed (VteTerminal *terminal G_GNUC_UNUSED,
           const gchar *text,
           guint size,
           GString *bytes)
{
  g_string_append_len (bytes, text, size);
}

static gchar *
tmux_command (const gchar *socket,
              const gchar *command,
              const gchar *argument)
{
  const gchar *argv[] = { g_getenv ("TEST_TMUX"), "-N", "-S", socket, command, argument, NULL, NULL };
  gchar *output = NULL;
  gint status;
  GError *error = NULL;
  if (strcmp (command, "display-message") == 0)
    {
      argv[5] = "-p";
      argv[6] = argument;
    }
  g_assert_true (g_spawn_sync (NULL, (gchar **) argv, NULL, 0, NULL, NULL, &output, NULL, &status, &error));
  g_assert_no_error (error);
  g_assert_true (g_spawn_check_wait_status (status, &error));
  g_assert_no_error (error);
  return output;
}

static gboolean
ready (Fixture *fixture)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  do
    {
      gchar *text;
      gboolean found;
      settle ();
#if VTE_CHECK_VERSION(0, 80, 0)
      text = vte_terminal_get_text_format (fixture->terminal, VTE_FORMAT_TEXT);
#else
      text = vte_terminal_get_text (fixture->terminal, NULL, NULL, NULL);
#endif
      found = text != NULL && strstr (text, "CONTEXT-READY") != NULL;
      g_free (text);
      if (found)
        return TRUE;
    }
  while (g_get_monotonic_time () < deadline);
  return FALSE;
}

static void
right_click (Fixture *fixture,
             guint modifiers)
{
  GdkWindow *window = gtk_widget_get_window (fixture->widget);
  GdkDevice *pointer = gdk_seat_get_pointer (gdk_display_get_default_seat (gdk_window_get_display (window)));
  for (guint release = 0; release < 2; release++)
    {
      GdkEvent *event = gdk_event_new (release ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS);
      event->button.window = g_object_ref (window);
      event->button.button = 3;
      event->button.x = 10;
      event->button.y = 10;
      event->button.state = modifiers | (release ? GDK_BUTTON3_MASK : 0);
      gdk_event_set_device (event, pointer);
      gtk_widget_event (fixture->widget, event);
      gdk_event_free (event);
      if (!release)
        {
          GdkEvent *motion = gdk_event_new (GDK_MOTION_NOTIFY);
          motion->motion.window = g_object_ref (window);
          motion->motion.x = 20;
          motion->motion.y = 10;
          motion->motion.state = modifiers | GDK_BUTTON3_MASK;
          gdk_event_set_device (motion, pointer);
          gtk_widget_event (fixture->widget, motion);
          gdk_event_free (motion);
        }
    }
  settle ();
}

static void
context_through_tmux (Fixture *fixture,
                      gconstpointer data)
{
  const gchar *kind = data;
  gboolean mouse = g_strcmp0 (kind, "shell") != 0;
  gboolean history = g_strcmp0 (kind, "history") == 0;
  gboolean shifted = g_strcmp0 (kind, "shift") == 0;
  gboolean native = !mouse || history || shifted;
  gchar *root = g_dir_make_tmp ("xfce-context-menu-XXXXXX", NULL);
  gchar *socket = g_build_filename (root, "server.sock", NULL);
  gchar *config = g_build_filename (root, "tmux.conf", NULL);
  gchar *log = g_build_filename (root, "input", NULL);
  gchar *before = NULL, *after = NULL, *input = NULL;
  GString *bytes = g_string_new (NULL);
  guint menus = 0;
  pid_t child;

  g_assert_nonnull (g_getenv ("TEST_TMUX"));
  g_assert_true (g_file_set_contents (config, "set -g mouse on\nset -g status off\nset -g destroy-unattached on\n", -1, NULL));
  g_assert_true (g_file_set_contents (log, "", 0, NULL));
  g_object_set (fixture->preferences,
                "misc-hyperlink-open-button", 1u,
                "misc-right-click-action", TERMINAL_RIGHT_CLICK_ACTION_CONTEXT_MENU,
                NULL);
  child = fork ();
  g_assert_cmpint (child, >=, 0);
  if (child == 0)
    {
      if (setsid () < 0 || ioctl (fixture->slave, TIOCSCTTY, 0) < 0)
        _exit (126);
      for (int fd = 0; fd <= 2; fd++)
        if (dup2 (fixture->slave, fd) < 0)
          _exit (126);
      execl (g_getenv ("TEST_TMUX"), "tmux", "-S", socket, "-f", config,
             "new-session", "-s", "context", "--", self, "--child",
             mouse ? "mouse" : "shell", log, NULL);
      _exit (127);
    }
  g_assert_true (ready (fixture));
  if (history)
    {
      g_free (tmux_command (socket, "copy-mode", NULL));
      const gchar *argv[] = { g_getenv ("TEST_TMUX"), "-N", "-S", socket,
                              "send-keys", "-X", "-N", "5", "scroll-up", ";",
                              "send-keys", "-X", "begin-selection", ";",
                              "send-keys", "-X", "cursor-right", NULL };
      gint status;
      g_assert_true (g_spawn_sync (NULL, (gchar **) argv, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL));
      g_assert_true (g_spawn_check_wait_status (status, NULL));
      settle ();
    }
  before = tmux_command (socket, "display-message", "#{pane_in_mode}:#{selection_present}:#{scroll_position}");
  g_signal_connect (fixture->widget, "context-menu", G_CALLBACK (menu_requested), &menus);
  g_signal_connect (fixture->widget, "commit", G_CALLBACK (committed), bytes);
  right_click (fixture, shifted ? GDK_SHIFT_MASK : 0);
  after = tmux_command (socket, "display-message", "#{pane_in_mode}:#{selection_present}:#{scroll_position}");
  g_test_message ("Pane state before: %s; after: %s", g_strstrip (before), g_strstrip (after));
  g_assert_true (g_file_get_contents (log, &input, NULL, NULL));
  g_signal_handlers_disconnect_by_data (fixture->widget, &menus);
  g_signal_handlers_disconnect_by_data (fixture->widget, bytes);
  g_free (tmux_command (socket, "kill-server", NULL));
  waitpid (child, NULL, 0);
  remove_test_directory (root);

  /* Assert after cleanup so a failing routing check cannot leave a test server. */
  g_assert_cmpuint (menus, ==, native ? 1 : 0);
  if (history)
    g_assert_true (g_str_has_prefix (before, "1:1:"));
  if (native)
    {
      g_assert_cmpuint (bytes->len, ==, 0);
      g_assert_cmpstr (input, ==, "");
    }
  else
    {
      g_assert_nonnull (strstr (bytes->str, "\033[<2;"));
      g_assert_nonnull (strstr (input, "\033[<2;"));
    }
  g_assert_cmpstr (before, ==, after);
  g_free (input);
  g_free (before);
  g_free (after);
  g_string_free (bytes, TRUE);
  g_free (log);
  g_free (config);
  g_free (socket);
  g_free (root);
}

int
main (int argc,
      char **argv)
{
  if (argc == 4 && strcmp (argv[1], "--child") == 0)
    {
      struct termios attrs;
      char buffer[1024];
      ssize_t count;
      int log = open (argv[3], O_WRONLY | O_APPEND);
      if (log < 0 || tcgetattr (0, &attrs) < 0)
        return 1;
      cfmakeraw (&attrs);
      if (tcsetattr (0, TCSANOW, &attrs) < 0)
        return 1;
      if (strcmp (argv[2], "mouse") == 0)
        write (1, "\033[?1002h\033[?1006h", 16);
      for (guint i = 0; i < 60; i++)
        write (1, "history line\r\n", 14);
      write (1, "CONTEXT-READY\r\n", 15);
      while ((count = read (0, buffer, sizeof buffer)) > 0)
        if (write (log, buffer, count) != count)
          return 1;
      close (log);
      return 0;
    }
  if (g_getenv ("TEST_TMUX") == NULL)
    {
      gchar *tmux = g_find_program_in_path ("tmux");
      if (tmux == NULL)
        {
          g_print ("Context-menu integration tests require tmux.\n");
          return 77;
        }
      g_setenv ("TEST_TMUX", tmux, TRUE);
      g_free (tmux);
    }
  if (g_getenv ("TERMINAL_MOUSE_TEST_SESSION") == NULL)
    return run_isolated (argc, argv);
  self = g_canonicalize_filename (argv[0], NULL);
  g_test_init (&argc, &argv, NULL);
  gtk_init (&argc, &argv);
  g_test_add ("/context/tmux/shell", Fixture, "shell", setup, context_through_tmux, teardown);
  g_test_add ("/context/tmux/application", Fixture, "application", setup, context_through_tmux, teardown);
  g_test_add ("/context/tmux/shift", Fixture, "shift", setup, context_through_tmux, teardown);
  g_test_add ("/context/tmux/history", Fixture, "history", setup, context_through_tmux, teardown);
  int result = g_test_run ();
  g_free (self);
  return result;
}
