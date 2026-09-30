/* Drive GTK key events through the real widget, PTY and an isolated tmux server. */
#include "terminal-test-utils.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static gchar *self;

static int
record_input (const gchar *path)
{
  struct termios attrs;
  char buffer[256];
  ssize_t count;
  int output;
  int flags = fcntl (STDIN_FILENO, F_GETFL);

  if (flags < 0 || fcntl (STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) != 0
      || tcgetattr (STDIN_FILENO, &attrs) != 0)
    return 1;
  cfmakeraw (&attrs);
  if (tcsetattr (STDIN_FILENO, TCSANOW, &attrs) != 0)
    return 1;
  output = open (path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (output < 0)
    return 1;
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

static void
key_event (Fixture *fixture, guint keyval, guint state)
{
  GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
  GdkDisplay *display = gtk_widget_get_display (fixture->widget);
  GdkKeymapKey *keys = NULL;
  gint count = 0;

  event->key.window = g_object_ref (gtk_widget_get_window (fixture->widget));
  event->key.keyval = keyval;
  event->key.state = state;
  if (gdk_keymap_get_entries_for_keyval (gdk_keymap_get_for_display (display), keyval, &keys, &count)
      && count > 0)
    {
      event->key.hardware_keycode = keys[0].keycode;
      event->key.group = keys[0].group;
    }
  g_free (keys);
  gdk_event_set_device (event, gdk_seat_get_keyboard (gdk_display_get_default_seat (display)));
  gtk_widget_event (fixture->widget, event);
  gdk_event_free (event);
}

static void
check_recording (const gchar *path, const gchar *expected)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  gchar *actual = NULL;

  do
    {
      g_free (actual);
      actual = NULL;
      settle ();
      g_file_get_contents (path, &actual, NULL, NULL);
      if (g_strcmp0 (actual, expected) == 0)
        break;
    }
  while (g_get_monotonic_time () < deadline);
  g_assert_cmpstr (actual, ==, expected);
  g_free (actual);
}

static pid_t
spawn_foreground (Fixture *fixture, gchar **argv)
{
  pid_t child = fork ();

  g_assert_cmpint (child, >=, 0);
  if (child == 0)
    {
      if (setsid () < 0 || ioctl (fixture->slave, TIOCSCTTY, 0) < 0)
        _exit (126);
      for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++)
        if (dup2 (fixture->slave, fd) < 0)
          _exit (126);
      execv (argv[0], argv);
      _exit (127);
    }
  return child;
}

static void
stop_child (pid_t child)
{
  int status;

  kill (child, SIGTERM);
  while (waitpid (child, &status, 0) < 0 && errno == EINTR)
    ;
}

static void
send_enter_keys (Fixture *fixture)
{
  key_event (fixture, GDK_KEY_Return, 0);
  key_event (fixture, GDK_KEY_Return, GDK_SHIFT_MASK);
  key_event (fixture, GDK_KEY_Return, GDK_CONTROL_MASK);
  key_event (fixture, GDK_KEY_Return, GDK_MOD1_MASK);
  key_event (fixture, GDK_KEY_KP_Enter, GDK_SHIFT_MASK);
  key_event (fixture, GDK_KEY_j, GDK_CONTROL_MASK);
}

static void
direct_keys (Fixture *fixture, gconstpointer data)
{
  const gchar *name = data;
  gchar *directory = g_dir_make_tmp ("xfce-terminal-keys-XXXXXX", NULL);
  gchar *executable = g_build_filename (directory, name, NULL);
  gchar *output = g_build_filename (directory, "input", NULL);
  gchar *argv[] = { executable, "--record-input", output, NULL };
  pid_t child;

  g_assert_cmpint (symlink (self, executable), ==, 0);
  child = spawn_foreground (fixture, argv);
  check_recording (output, "");
  send_enter_keys (fixture);
  check_recording (output, g_strcmp0 (name, "cat") == 0 ? "\r\r\r\033\r\r\n" : "\r\n\n\n\n\n");
  stop_child (child);
  remove_test_directory (directory);
  g_free (output);
  g_free (executable);
  g_free (directory);
}

static void
tmux_command (gchar **argv)
{
  gint status;
  GError *error = NULL;

  g_assert_true (g_spawn_sync (NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, &status, &error));
  g_assert_no_error (error);
  g_assert_true (WIFEXITED (status));
  g_assert_cmpint (WEXITSTATUS (status), ==, 0);
}

static void
tmux_keys (Fixture *fixture, gconstpointer data)
{
  gchar *tmux = g_find_program_in_path ("tmux");
  gchar *directory;
  gchar *socket;
  gchar *config;
  gchar *config_text;
  gchar *ai;
  gchar *other;
  gchar *ai_output;
  gchar *other_output;
  pid_t child;

  if (tmux == NULL)
    {
      g_test_skip ("tmux is not installed");
      return;
    }
  directory = g_dir_make_tmp ("xfce-terminal-keys-XXXXXX", NULL);
  socket = g_build_filename (directory, "socket", NULL);
  config = g_build_filename (directory, "tmux.conf", NULL);
  ai = g_build_filename (directory, (const gchar *) data, NULL);
  other = g_build_filename (directory, "cat", NULL);
  ai_output = g_build_filename (directory, "ai-input", NULL);
  other_output = g_build_filename (directory, "other-input", NULL);
  g_assert_cmpint (symlink (self, ai), ==, 0);
  g_assert_cmpint (symlink (self, other), ==, 0);
  config_text = g_strdup_printf ("source-file '%s/contrib/tmux-modified-enter.conf'\n"
                                 "set -s exit-unattached on\nset -s extended-keys off\n",
                                 TEST_SOURCE_DIR);
  g_assert_true (g_file_set_contents (config, config_text, -1, NULL));
  gchar *start[] = { tmux, "-S", socket, "-f", config, "new-session", "-s", "keys", "-n", "ai",
                    ai, "--record-input", ai_output, NULL };
  child = spawn_foreground (fixture, start);
  check_recording (ai_output, "");
  send_enter_keys (fixture);
  check_recording (ai_output, "\r\n\n\n\n\n");

  /* Keep the same tmux client, but switch the active pane to a non-AI program. */
  gchar *next[] = { tmux, "-S", socket, "new-window", "-t", "keys", "-n", "other",
                   other, "--record-input", other_output, NULL };
  tmux_command (next);
  check_recording (other_output, "");
  send_enter_keys (fixture);
  check_recording (other_output, "\r\r\r\033\r\r\n");
  check_recording (ai_output, "\r\n\n\n\n\n");

  gchar *previous[] = { tmux, "-S", socket, "select-window", "-t", "keys:ai", NULL };
  tmux_command (previous);
  key_event (fixture, GDK_KEY_Return, GDK_SHIFT_MASK);
  check_recording (ai_output, "\r\n\n\n\n\n\n");
  gchar *stop[] = { tmux, "-S", socket, "kill-server", NULL };
  tmux_command (stop);
  stop_child (child);
  remove_test_directory (directory);
  g_free (config_text);
  g_free (config);
  g_free (socket);
  g_free (ai);
  g_free (other);
  g_free (ai_output);
  g_free (other_output);
  g_free (directory);
  g_free (tmux);
}

int
main (int argc, char **argv)
{
  int result;

  if (argc == 3 && g_strcmp0 (argv[1], "--record-input") == 0)
    return record_input (argv[2]);
  if (g_getenv ("TERMINAL_MOUSE_TEST_SESSION") == NULL)
    return run_isolated (argc, argv);
  g_test_init (&argc, &argv, NULL);
  gtk_init (&argc, &argv);
  self = g_file_read_link ("/proc/self/exe", NULL);
  g_assert_nonnull (self);
  g_test_add ("/keys/direct-codex", Fixture, "codex", setup, direct_keys, teardown);
  g_test_add ("/keys/direct-gemini", Fixture, "gemini", setup, direct_keys, teardown);
  g_test_add ("/keys/direct-agy", Fixture, "agy", setup, direct_keys, teardown);
  g_test_add ("/keys/direct-other", Fixture, "cat", setup, direct_keys, teardown);
  g_test_add ("/keys/tmux-active-pane", Fixture, "codex", setup, tmux_keys, teardown);
  g_test_add ("/keys/tmux-versioned-codex", Fixture, "codex-dev", setup, tmux_keys, teardown);
  g_test_add ("/keys/tmux-gemini", Fixture, "gemini", setup, tmux_keys, teardown);
  g_test_add ("/keys/tmux-agy", Fixture, "agy", setup, tmux_keys, teardown);
  result = g_test_run ();
  g_free (self);
  return result;
}
