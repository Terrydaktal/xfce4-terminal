/* Exercise the real window's clipboard accelerators, not only the VTE widget. */
#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "terminal/terminal-screen.h"
#include "terminal/terminal-window.h"

#include "terminal-test-utils.h"

static gchar *self;

static int
record_input (const gchar *path, gboolean mouse_reporting)
{
  struct termios attrs;
  gchar buffer[256];
  ssize_t count;
  int output;
  int flags;

  if (tcgetattr (STDIN_FILENO, &attrs) != 0)
    return 1;
  cfmakeraw (&attrs);
  if (tcsetattr (STDIN_FILENO, TCSANOW, &attrs) != 0)
    return 1;
  flags = fcntl (STDIN_FILENO, F_GETFL);
  if (flags < 0 || fcntl (STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0)
    return 1;
  output = open (path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (output < 0)
    return 1;
  for (guint i = 0; i < 100; i++)
    dprintf (STDOUT_FILENO, "copy-test-history-%03u\r\n", i);
  if (mouse_reporting)
    dprintf (STDOUT_FILENO, "\033[?1002h\033[?1006h\033[7mCOPY-TEST-READY\033[0m\r\n");
  else
    dprintf (STDOUT_FILENO, "COPY-TEST-READY\r\n");
  while ((count = read (STDIN_FILENO, buffer, sizeof buffer)) > 0)
    {
      for (ssize_t offset = 0; offset < count;)
        {
          ssize_t written = write (output, buffer + offset, count - offset);
          if (written <= 0)
            return 1;
          offset += written;
        }
    }
  close (output);
  return 0;
}

static GtkWidget *
find_terminal (GtkWidget *widget)
{
  GtkWidget *terminal = NULL;

  if (VTE_IS_TERMINAL (widget))
    return widget;
  if (GTK_IS_CONTAINER (widget))
    {
      GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
      for (GList *child = children; child != NULL && terminal == NULL; child = child->next)
        terminal = find_terminal (child->data);
      g_list_free (children);
    }
  return terminal;
}

static void
setup_window (Fixture *fixture, gconstpointer mouse_reporting)
{
  TerminalTabAttr attr = { 0 };
  TerminalScreen *screen;
  VtePty *pty;
  struct termios attrs;
  GError *error = NULL;

  fixture->preferences = terminal_preferences_get ();
  g_object_set (fixture->preferences,
                "misc-prefer-mouse-selection", FALSE,
                "misc-copy-on-select", FALSE,
                "misc-highlight-urls", FALSE,
                NULL);
  fixture->window = terminal_window_new (NULL, FALSE, TERMINAL_VISIBILITY_HIDE,
                                         TERMINAL_VISIBILITY_HIDE, TERMINAL_VISIBILITY_HIDE);
  screen = terminal_screen_new (&attr, 80, 24);
  terminal_window_add (TERMINAL_WINDOW (fixture->window), screen);
  fixture->widget = find_terminal (GTK_WIDGET (screen));
  g_assert_nonnull (fixture->widget);
  fixture->terminal = VTE_TERMINAL (fixture->widget);
  gtk_accel_map_change_entry ("<Actions>/terminal-window/copy", GDK_KEY_c, GDK_CONTROL_MASK, TRUE);
  gtk_accel_map_change_entry ("<Actions>/terminal-window/paste", GDK_KEY_v, GDK_CONTROL_MASK, TRUE);
  gtk_widget_show (fixture->window);
  gtk_widget_grab_focus (fixture->widget);
  settle ();

  pty = vte_pty_new_sync (VTE_PTY_NO_HELPER, NULL, &error);
  g_assert_no_error (error);
  fixture->slave = open (ptsname (vte_pty_get_fd (pty)), O_RDWR | O_NOCTTY | O_NONBLOCK);
  g_assert_cmpint (fixture->slave, >=, 0);
  g_assert_cmpint (tcgetattr (fixture->slave, &attrs), ==, 0);
  cfmakeraw (&attrs);
  g_assert_cmpint (tcsetattr (fixture->slave, TCSANOW, &attrs), ==, 0);
  vte_terminal_set_pty (fixture->terminal, pty);
  g_object_unref (pty);
  vte_terminal_feed (fixture->terminal, "\033[Halpha beta gamma delta\r\nsecond line", -1);
  if (mouse_reporting != NULL)
    {
      /* An application's reverse-video highlight is not a VTE selection. */
      vte_terminal_feed (fixture->terminal, "\033[?1002h\033[?1006h\033[H\033[7malpha\033[0m", -1);
    }
  settle ();
}

static void
press_control_key (Fixture *fixture, guint keyval)
{
  GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
  GdkDisplay *display = gtk_widget_get_display (fixture->widget);
  GdkKeymapKey *keys = NULL;
  gint count = 0;

  event->key.window = g_object_ref (gtk_widget_get_window (fixture->widget));
  event->key.keyval = keyval;
  event->key.state = GDK_CONTROL_MASK;
  if (gdk_keymap_get_entries_for_keyval (gdk_keymap_get_for_display (display), keyval, &keys, &count)
      && count > 0)
    {
      event->key.hardware_keycode = keys[0].keycode;
      event->key.group = keys[0].group;
    }
  g_free (keys);
  gdk_event_set_device (event, gdk_seat_get_keyboard (gdk_display_get_default_seat (display)));
  gtk_widget_event (fixture->window, event);
  gdk_event_free (event);
  settle ();
}

static void
press_copy (Fixture *fixture)
{
  press_control_key (fixture, GDK_KEY_c);
}

static void
copy_native_selection (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  gchar buffer[16];
  gchar *text;

  gtk_clipboard_set_text (clipboard, "previous clipboard", -1);
  vte_terminal_select_all (fixture->terminal);
  g_assert_true (vte_terminal_get_has_selection (fixture->terminal));
  press_copy (fixture);
  text = gtk_clipboard_wait_for_text (clipboard);
  g_assert_nonnull (text);
  g_assert_nonnull (strstr (text, "alpha beta gamma delta"));
  g_assert_nonnull (strstr (text, "second line"));
  g_assert_cmpint (read (fixture->slave, buffer, sizeof buffer), ==, -1);
  g_assert_cmpint (errno, ==, EAGAIN);
  g_assert_true (vte_terminal_get_has_selection (fixture->terminal));
  g_free (text);
}

static void
copy_application_selection (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  gchar buffer[16];
  gchar *text;

  gtk_clipboard_set_text (clipboard, "previous clipboard", -1);
  g_assert_false (vte_terminal_get_has_selection (fixture->terminal));
  press_copy (fixture);
  g_assert_cmpint (read (fixture->slave, buffer, sizeof buffer), ==, 1);
  g_assert_cmpint (buffer[0], ==, '\003');
  text = gtk_clipboard_wait_for_text (clipboard);
  g_assert_cmpstr (text, ==, "previous clipboard");
  g_free (text);
}

static gchar *
tmux_command (const gchar *binary, const gchar *socket, const gchar *const *command)
{
  GPtrArray *argv = g_ptr_array_new ();
  GSubprocess *child;
  gchar *output;
  GError *error = NULL;

  g_ptr_array_add (argv, (gpointer) binary);
  g_ptr_array_add (argv, "-N");
  g_ptr_array_add (argv, "-S");
  g_ptr_array_add (argv, (gpointer) socket);
  for (guint i = 0; command[i] != NULL; i++)
    g_ptr_array_add (argv, (gpointer) command[i]);
  g_ptr_array_add (argv, NULL);
  child = g_subprocess_newv ((const gchar *const *) argv->pdata, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
  g_assert_no_error (error);
  g_assert_true (g_subprocess_communicate_utf8 (child, NULL, NULL, &output, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_subprocess_get_successful (child));
  g_object_unref (child);
  g_ptr_array_unref (argv);
  return output;
}

static void
copy_through_tmux (Fixture *fixture, gconstpointer data)
{
  gboolean selection = GPOINTER_TO_INT (data) == 1;
  gboolean stalled = GPOINTER_TO_INT (data) == 3;
  gboolean history = GPOINTER_TO_INT (data) < 2;
  gchar *binary = g_find_program_in_path ("tmux");
  gchar *directory, *socket, *config, *recording, *copied, *contents, *before, *after;
  const gchar *state[] = { "display-message", "-p", "-t", "=copy-test:.",
                           "#{pane_in_mode}|#{scroll_position}|#{selection_present}", NULL };
  const gchar *select[] = { "copy-mode", ";", "send-keys", "-X", "-N", "5", "scroll-up",
                            ";", "send-keys", "-X", "start-of-line",
                            ";", "send-keys", "-X", "begin-selection",
                            ";", "send-keys", "-X", "end-of-line", NULL };
  const gchar *browse[] = { "copy-mode", ";", "send-keys", "-X", "-N", "5", "scroll-up", NULL };
  const gchar *stop[] = { "kill-server", NULL };
  const gchar *show[] = { "show-buffer", NULL };
  GError *error = NULL;
  gint64 deadline;
  gsize length;
  pid_t client;
  int status;

  if (binary == NULL)
    {
      g_test_skip ("tmux is unavailable");
      return;
    }
  directory = g_dir_make_tmp ("xfce-terminal-copy-XXXXXX", &error);
  g_assert_no_error (error);
  socket = g_build_filename (directory, "server.sock", NULL);
  config = g_build_filename (directory, "tmux.conf", NULL);
  recording = g_build_filename (directory, "input", NULL);
  copied = g_build_filename (directory, "copied", NULL);
  contents = g_strdup_printf ("set -g status off\nset -g mouse on\nset -g scroll-on-input on\n"
                              "set -s exit-unattached on\nset -g destroy-unattached on\n"
                              "set -s set-clipboard off\nset -g history-limit 1000\n"
                              "set -g copy-command 'cat > %s'\n",
                              copied);
  g_assert_true (g_file_set_contents (config, contents, -1, &error));
  g_assert_no_error (error);
  g_free (contents);
  client = fork ();
  g_assert_cmpint (client, >=, 0);
  if (client == 0)
    {
      if (setsid () < 0 || ioctl (fixture->slave, TIOCSCTTY, 0) < 0)
        _exit (126);
      for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++)
        if (dup2 (fixture->slave, fd) < 0)
          _exit (126);
      gchar *argv[] = { binary, "-S", socket, "-f", config, "new-session", "-s", "copy-test",
                        "--", self, "--record", recording, history ? "history" : "mouse", NULL };
      execv (binary, argv);
      _exit (127);
    }
  deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  do
    {
      settle ();
      contents = vte_terminal_get_text_format (fixture->terminal, VTE_FORMAT_TEXT);
      gboolean ready = strstr (contents, "COPY-TEST-READY") != NULL;
      g_free (contents);
      if (ready)
        break;
    }
  while (g_get_monotonic_time () < deadline);
  if (!g_file_test (recording, G_FILE_TEST_EXISTS))
    {
      contents = vte_terminal_get_text_format (fixture->terminal, VTE_FORMAT_TEXT);
      g_test_message ("tmux startup output: %s", contents);
      g_free (contents);
    }
  g_assert_true (g_file_test (recording, G_FILE_TEST_EXISTS));
  if (history)
    g_free (tmux_command (binary, socket, selection ? select : browse));
  settle ();
  before = tmux_command (binary, socket, state);
  if (selection)
    g_assert_cmpstr (before, ==, "1|5|1\n");
  g_assert_false (vte_terminal_get_has_selection (fixture->terminal));
  if (stalled)
    {
      const gchar *pid_command[] = { "display-message", "-p", "#{pid}", NULL };
      gchar *pid_text = tmux_command (binary, socket, pid_command);
      pid_t server = (pid_t) g_ascii_strtoll (pid_text, NULL, 10);
      g_free (pid_text);
      g_assert_cmpint (server, >, 1);
      g_assert_cmpint (kill (server, SIGSTOP), ==, 0);
      gint64 started = g_get_monotonic_time ();
      press_copy (fixture);
      g_assert_cmpint (kill (server, SIGCONT), ==, 0);
      g_assert_cmpint (g_get_monotonic_time () - started, <, G_USEC_PER_SEC);
      settle ();
      g_assert_true (g_file_get_contents (recording, &contents, &length, NULL));
      g_assert_cmpuint (length, ==, 0);
      g_free (contents);
      after = tmux_command (binary, socket, state);
      g_assert_cmpstr (before, ==, after);
      goto cleanup;
    }
  press_copy (fixture);
  deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  do
    {
      settle ();
      contents = NULL;
      length = 0;
      g_file_get_contents (selection ? copied : recording, &contents, &length, NULL);
      if (length > 0)
        break;
      g_free (contents);
    }
  while (g_get_monotonic_time () < deadline);
  g_assert_cmpuint (length, >, 0);
  after = tmux_command (binary, socket, state);
  if (selection)
    {
      gchar *buffer = tmux_command (binary, socket, show);
      g_assert_cmpstr (contents, ==, buffer);
      g_assert_cmpstr (before, ==, after);
      g_assert_nonnull (strstr (contents, "copy-test-history-"));
      g_free (buffer);
      gchar *input;
      g_assert_true (g_file_get_contents (recording, &input, &length, NULL));
      g_assert_cmpuint (length, ==, 0);
      g_free (input);
    }
  else
    {
      g_assert_cmpuint (length, ==, 1);
      g_assert_cmpint (contents[0], ==, '\003');
      g_assert_true (g_str_has_prefix (after, "0|"));
    }
  g_free (contents);
cleanup:
  g_free (before);
  g_free (after);
  g_free (tmux_command (binary, socket, stop));
  while (waitpid (client, &status, 0) < 0 && errno == EINTR)
    ;
  remove_test_directory (directory);
  g_free (directory);
  g_free (socket);
  g_free (config);
  g_free (recording);
  g_free (copied);
  g_free (binary);
}

typedef enum
{
  PASTE_DIRECT_IMAGE,
  PASTE_TMUX_IMAGE,
  PASTE_TMUX_TEXT,
  PASTE_TMUX_OTHER_IMAGE,
  PASTE_TMUX_INACTIVE_CODEX_IMAGE
} PasteCase;

static void
setup_paste_window (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  setup_window (fixture, NULL);
}

static void
paste_to_application (Fixture *fixture, gconstpointer data)
{
  PasteCase test_case = GPOINTER_TO_INT (data);
  GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  gboolean through_tmux = test_case != PASTE_DIRECT_IMAGE;
  gboolean text_paste = test_case == PASTE_TMUX_TEXT;
  gboolean no_input = test_case == PASTE_TMUX_OTHER_IMAGE
                      || test_case == PASTE_TMUX_INACTIVE_CODEX_IMAGE;
  gchar *binary = through_tmux ? g_find_program_in_path ("tmux") : NULL;
  gchar *directory, *socket, *config, *recording, *agent, *contents, *inactive = NULL;
  const gchar *expected = no_input ? "" : text_paste ? "normal clipboard text" : "\026";
  const gchar *stop[] = { "kill-server", NULL };
  GError *error = NULL;
  gint64 deadline;
  gsize length;
  pid_t client;
  int status;

  if (through_tmux && binary == NULL)
    {
      g_test_skip ("tmux is unavailable");
      return;
    }
  directory = g_dir_make_tmp ("xfce-terminal-paste-XXXXXX", &error);
  g_assert_no_error (error);
  socket = g_build_filename (directory, "server.sock", NULL);
  config = g_build_filename (directory, "tmux.conf", NULL);
  recording = g_build_filename (directory, "input", NULL);
  agent = g_build_filename (directory, "codex", NULL);
  /* Only the fake agent's argv name changes; it records raw PTY input. */
  g_assert_cmpint (symlink (self, agent), ==, 0);
  g_assert_true (g_file_set_contents (config,
                                     "set -g status off\nset -s exit-unattached on\n"
                                     "set -g destroy-unattached on\nset -s set-clipboard off\n",
                                     -1, &error));
  g_assert_no_error (error);
  client = fork ();
  g_assert_cmpint (client, >=, 0);
  if (client == 0)
    {
      const gchar *program = test_case == PASTE_TMUX_OTHER_IMAGE ? self : agent;
      if (setsid () < 0 || ioctl (fixture->slave, TIOCSCTTY, 0) < 0)
        _exit (126);
      for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++)
        if (dup2 (fixture->slave, fd) < 0)
          _exit (126);
      if (through_tmux)
        {
          const gchar *argv[] = { binary, "-S", socket, "-f", config, "new-session", "-s", "paste-test",
                                  "--", program, "--record", recording, "history", NULL };
          execv (binary, (char *const *) argv);
        }
      else
        {
          const gchar *argv[] = { program, "--record", recording, "history", NULL };
          execv (program, (char *const *) argv);
        }
      _exit (127);
    }
  deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  do
    {
      settle ();
      contents = vte_terminal_get_text_format (fixture->terminal, VTE_FORMAT_TEXT);
      gboolean ready = strstr (contents, "COPY-TEST-READY") != NULL;
      g_free (contents);
      if (ready)
        break;
    }
  while (g_get_monotonic_time () < deadline);
  g_assert_true (g_file_test (recording, G_FILE_TEST_EXISTS));

  if (test_case == PASTE_TMUX_INACTIVE_CODEX_IMAGE)
    {
      /* A Codex process in another pane must not enable the fallback here. */
      inactive = recording;
      recording = g_build_filename (directory, "other-input", NULL);
      const gchar *split[] = { "split-window", "-h", "-t", "=paste-test:.", "--",
                               self, "--record", recording, "history", NULL };
      g_free (tmux_command (binary, socket, split));
      deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
      do
        settle ();
      while (!g_file_test (recording, G_FILE_TEST_EXISTS) && g_get_monotonic_time () < deadline);
      g_assert_true (g_file_test (recording, G_FILE_TEST_EXISTS));
    }
  if (text_paste)
    gtk_clipboard_set_text (clipboard, expected, -1);
  else
    {
      GdkPixbuf *image = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
      gdk_pixbuf_fill (image, 0xff0000ff);
      gtk_clipboard_set_image (clipboard, image);
      g_object_unref (image);
      g_assert_null (gtk_clipboard_wait_for_text (clipboard));
    }
  press_control_key (fixture, GDK_KEY_v);
  deadline = g_get_monotonic_time () + (no_input ? 250000 : G_USEC_PER_SEC);
  do
    {
      settle ();
      g_assert_true (g_file_get_contents (recording, &contents, &length, NULL));
      gboolean done = length > 0 || g_get_monotonic_time () >= deadline;
      if (done)
        break;
      g_free (contents);
    }
  while (TRUE);
  /* Clean up the test's private processes even when the regression fails. */
  if (through_tmux)
    g_free (tmux_command (binary, socket, stop));
  else
    g_assert_cmpint (kill (client, SIGTERM), ==, 0);
  while (waitpid (client, &status, 0) < 0 && errno == EINTR)
    ;
  if (inactive != NULL)
    {
      gchar *input;
      g_assert_true (g_file_get_contents (inactive, &input, NULL, NULL));
      g_assert_cmpstr (input, ==, "");
      g_free (input);
    }
  gtk_clipboard_clear (clipboard);
  remove_test_directory (directory);
  g_free (directory);
  g_free (socket);
  g_free (config);
  g_free (recording);
  g_free (inactive);
  g_free (agent);
  g_free (binary);
  g_assert_cmpmem (contents, length, expected, strlen (expected));
  g_free (contents);
}

int
main (int argc, char **argv)
{
  if (argc == 4 && strcmp (argv[1], "--record") == 0)
    return record_input (argv[2], strcmp (argv[3], "mouse") == 0);
  if (g_getenv ("TERMINAL_MOUSE_TEST_SESSION") == NULL)
    return run_isolated (argc, argv);
  g_unsetenv ("TMUX");
  g_unsetenv ("TMUX_PANE");
  g_setenv ("SHELL", "/bin/sh", TRUE);
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check (&argc, &argv))
    return 77;
  self = g_file_read_link ("/proc/self/exe", NULL);
  g_assert_nonnull (self);
  g_test_add ("/terminal/copy/native-selection", Fixture, NULL,
              setup_window, copy_native_selection, teardown);
  g_test_add ("/terminal/copy/application-selection", Fixture, GINT_TO_POINTER (1),
              setup_window, copy_application_selection, teardown);
  g_test_add ("/terminal/copy/no-selection-interrupt", Fixture, NULL,
              setup_window, copy_application_selection, teardown);
  g_test_add ("/terminal/copy/native-over-application-selection", Fixture, GINT_TO_POINTER (1),
              setup_window, copy_native_selection, teardown);
  g_test_add ("/terminal/copy/tmux-history-selection", Fixture, GINT_TO_POINTER (1),
              setup_window, copy_through_tmux, teardown);
  g_test_add ("/terminal/copy/tmux-application-selection", Fixture, GINT_TO_POINTER (2),
              setup_window, copy_through_tmux, teardown);
  g_test_add ("/terminal/copy/tmux-history-without-selection", Fixture, NULL,
              setup_window, copy_through_tmux, teardown);
  g_test_add ("/terminal/copy/tmux-query-timeout-does-not-interrupt", Fixture, GINT_TO_POINTER (3),
              setup_window, copy_through_tmux, teardown);
  g_test_add ("/terminal/paste/direct-codex-image", Fixture, GINT_TO_POINTER (PASTE_DIRECT_IMAGE),
              setup_paste_window, paste_to_application, teardown);
  g_test_add ("/terminal/paste/tmux-codex-image", Fixture, GINT_TO_POINTER (PASTE_TMUX_IMAGE),
              setup_paste_window, paste_to_application, teardown);
  g_test_add ("/terminal/paste/tmux-codex-text", Fixture, GINT_TO_POINTER (PASTE_TMUX_TEXT),
              setup_paste_window, paste_to_application, teardown);
  g_test_add ("/terminal/paste/tmux-other-image", Fixture, GINT_TO_POINTER (PASTE_TMUX_OTHER_IMAGE),
              setup_paste_window, paste_to_application, teardown);
  g_test_add ("/terminal/paste/tmux-inactive-codex-image", Fixture, GINT_TO_POINTER (PASTE_TMUX_INACTIVE_CODEX_IMAGE),
              setup_paste_window, paste_to_application, teardown);
  int result = g_test_run ();
  g_free (self);
  return result;
}
