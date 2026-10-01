/* Exercise the real window's Copy accelerator, not only the VTE widget. */
#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "terminal/terminal-screen.h"
#include "terminal/terminal-window.h"

#include "terminal-test-utils.h"

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
press_copy (Fixture *fixture)
{
  GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
  GdkDisplay *display = gtk_widget_get_display (fixture->widget);
  GdkKeymapKey *keys = NULL;
  gint count = 0;

  event->key.window = g_object_ref (gtk_widget_get_window (fixture->widget));
  event->key.keyval = GDK_KEY_c;
  event->key.state = GDK_CONTROL_MASK;
  if (gdk_keymap_get_entries_for_keyval (gdk_keymap_get_for_display (display), GDK_KEY_c, &keys, &count)
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

int
main (int argc, char **argv)
{
  if (g_getenv ("TERMINAL_MOUSE_TEST_SESSION") == NULL)
    return run_isolated (argc, argv);
  g_unsetenv ("TMUX");
  g_unsetenv ("TMUX_PANE");
  g_setenv ("SHELL", "/bin/sh", TRUE);
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check (&argc, &argv))
    return 77;
  g_test_add ("/terminal/copy/native-selection", Fixture, NULL,
              setup_window, copy_native_selection, teardown);
  g_test_add ("/terminal/copy/application-selection", Fixture, GINT_TO_POINTER (1),
              setup_window, copy_application_selection, teardown);
  g_test_add ("/terminal/copy/no-selection-interrupt", Fixture, NULL,
              setup_window, copy_application_selection, teardown);
  g_test_add ("/terminal/copy/native-over-application-selection", Fixture, GINT_TO_POINTER (1),
              setup_window, copy_native_selection, teardown);
  int result = g_test_run ();
  return result;
}
