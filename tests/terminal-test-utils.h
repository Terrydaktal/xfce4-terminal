#ifndef TERMINAL_TEST_UTILS_H
#define TERMINAL_TEST_UTILS_H

/* Shared GTK/VTE fixture and isolated xfconf setup for input integration tests. */
#define _GNU_SOURCE

#include "terminal/terminal-preferences.h"
#include "terminal/terminal-widget.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct
{
  GtkWidget *window;
  GtkWidget *widget;
  VteTerminal *terminal;
  TerminalPreferences *preferences;
  int slave;
} Fixture;

static void
settle (void)
{
  gint64 deadline = g_get_monotonic_time () + 50000;

  do
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);
}

static void
setup (Fixture *fixture, gconstpointer data)
{
  GError *error = NULL;
  VtePty *pty;
  struct termios attrs;
  gboolean prefer_selection;

  fixture->preferences = terminal_preferences_get ();
  g_object_set (fixture->preferences,
                "misc-prefer-mouse-selection", TRUE,
                "misc-highlight-urls", FALSE,
                "misc-hyperlinks-enabled", TRUE,
                "misc-hyperlink-insert-button", 0u,
                "misc-hyperlink-insert-middle-click", FALSE,
                "misc-hyperlink-open-button", 3u,
                "misc-hyperlink-open-modifier", (guint) GDK_CONTROL_MASK,
                NULL);
  g_object_get (fixture->preferences, "misc-prefer-mouse-selection", &prefer_selection, NULL);
  g_assert_true (prefer_selection);
  fixture->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  fixture->widget = g_object_new (TERMINAL_TYPE_WIDGET, NULL);
  fixture->terminal = VTE_TERMINAL (fixture->widget);
  gtk_container_add (GTK_CONTAINER (fixture->window), fixture->widget);
  gtk_window_set_default_size (GTK_WINDOW (fixture->window), 640, 240);
  gtk_widget_show_all (fixture->window);
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
  if (data != NULL)
    vte_terminal_feed (fixture->terminal, "\033[?1002h\033[?1006h", -1);
  settle ();
}

static void
teardown (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  gtk_widget_destroy (fixture->window);
  close (fixture->slave);
  g_object_unref (fixture->preferences);
  settle ();
}
static void
remove_test_directory (const gchar *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const gchar *name;

  if (dir == NULL)
    return;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      gchar *child = g_build_filename (path, name, NULL);
      if (!g_file_test (child, G_FILE_TEST_IS_SYMLINK)
          && g_file_test (child, G_FILE_TEST_IS_DIR))
        remove_test_directory (child);
      else
        g_unlink (child);
      g_free (child);
    }
  g_dir_close (dir);
  g_rmdir (path);
}

static int
run_isolated (int argc, char **argv)
{
  gchar *runner = g_find_program_in_path ("dbus-run-session");
  gchar *config;
  const gchar **command;
  GSubprocessLauncher *launcher;
  GSubprocess *child;
  GError *error = NULL;
  int result;

  if (runner == NULL || g_getenv ("DISPLAY") == NULL)
    {
      g_print ("Input integration tests require dbus-run-session and Xvfb or an X11 display.\n");
      g_free (runner);
      return 77;
    }

  /* The bus must inherit the private config before it starts xfconfd. Run GTK
   * in a child too: its process-wide D-Bus references outlive widget teardown. */
  config = g_dir_make_tmp ("xfce-terminal-mouse-XXXXXX", &error);
  g_assert_no_error (error);
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "TERMINAL_MOUSE_TEST_SESSION", "1", TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_CONFIG_HOME", config, TRUE);
  g_subprocess_launcher_setenv (launcher, "GDK_BACKEND", "x11", TRUE);
  g_subprocess_launcher_setenv (launcher, "GTK_THEME", "Adwaita", TRUE);
  g_subprocess_launcher_setenv (launcher, "NO_AT_BRIDGE", "1", TRUE);
  g_subprocess_launcher_setenv (launcher, "GIO_USE_VFS", "local", TRUE);
  command = g_new0 (const gchar *, argc + 3);
  command[0] = runner;
  command[1] = "--";
  for (int i = 0; i < argc; i++)
    command[i + 2] = argv[i];
  child = g_subprocess_launcher_spawnv (launcher, command, &error);
  g_assert_no_error (error);
  g_assert_true (g_subprocess_wait (child, NULL, &error));
  g_assert_no_error (error);
  result = g_subprocess_get_if_exited (child) ? g_subprocess_get_exit_status (child) : 1;
  g_object_unref (child);
  g_object_unref (launcher);
  g_free (command);
  g_free (runner);
  remove_test_directory (config);
  g_free (config);
  return result;
}

#endif
