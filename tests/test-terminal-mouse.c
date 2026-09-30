/* Exercise actual VTE selection and PTY mouse reports. */
#include "terminal-test-utils.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

static gchar *
read_input (Fixture *fixture)
{
  GString *input = g_string_new (NULL);
  gchar buffer[1024];
  ssize_t size;

  settle ();
  while ((size = read (fixture->slave, buffer, sizeof buffer)) > 0)
    g_string_append_len (input, buffer, size);
  g_assert_true (size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
  return g_string_free (input, FALSE);
}

static void
mouse_event_at (Fixture *fixture, GdkEventType type, guint button, guint state, double column, double row)
{
  GdkEvent *event = gdk_event_new (type);
  GdkWindow *window = gtk_widget_get_window (fixture->widget);
  GList *children = gdk_window_get_children (window);
  GdkDevice *pointer = gdk_seat_get_pointer (gdk_display_get_default_seat (gdk_window_get_display (window)));
  GtkBorder padding;
  double x, y;

  /* VTE rejects pointer coordinates unless they belong to its input window. */
  for (GList *child = children; child != NULL; child = child->next)
    {
      gpointer owner;
      gdk_window_get_user_data (child->data, &owner);
      if (owner == fixture->widget && gdk_window_is_input_only (child->data))
        {
          window = child->data;
          break;
        }
    }
  g_list_free (children);

  gtk_style_context_get_padding (gtk_widget_get_style_context (fixture->widget), GTK_STATE_FLAG_NORMAL, &padding);
  x = padding.left + column * vte_terminal_get_char_width (fixture->terminal);
  y = padding.top + row * vte_terminal_get_char_height (fixture->terminal);
  event->any.window = g_object_ref (window);
  if (type == GDK_MOTION_NOTIFY)
    {
      event->motion.x = x;
      event->motion.y = y;
      event->motion.state = state;
    }
  else if (type == GDK_SCROLL)
    {
      event->scroll.x = x;
      event->scroll.y = y;
      event->scroll.state = state;
      event->scroll.direction = GDK_SCROLL_UP;
    }
  else
    {
      event->button.x = x;
      event->button.y = y;
      event->button.button = button;
      event->button.state = state;
    }
  gdk_event_set_device (event, pointer);
  gtk_widget_event (fixture->widget, event);
  gdk_event_free (event);
}

static void
mouse_event (Fixture *fixture, GdkEventType type, guint button, guint state, double column)
{
  mouse_event_at (fixture, type, button, state, column, 0.5);
}

static void
double_click_position (Fixture *fixture, double column, double row)
{
  mouse_event_at (fixture, GDK_BUTTON_PRESS, 1, 0, column, row);
  mouse_event_at (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, column, row);
  mouse_event_at (fixture, GDK_BUTTON_PRESS, 1, 0, column, row);
  mouse_event_at (fixture, GDK_2BUTTON_PRESS, 1, GDK_BUTTON1_MASK, column, row);
  mouse_event_at (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, column, row);
}

static void
double_click_at (Fixture *fixture, gsize offset)
{
  glong columns = vte_terminal_get_column_count (fixture->terminal);
  double_click_position (fixture, offset % columns + 0.1, offset / columns + 0.5);
}

static void
drag (Fixture *fixture, guint modifiers, double start, double end)
{
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, modifiers, start);
  mouse_event (fixture, GDK_MOTION_NOTIFY, 1, modifiers | GDK_BUTTON1_MASK, end);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, modifiers | GDK_BUTTON1_MASK, end);
}

static void
assert_selection (Fixture *fixture, const gchar *expected)
{
  gchar *selected = vte_terminal_get_text_selected (fixture->terminal, VTE_FORMAT_TEXT);
  g_assert_cmpstr (selected, ==, expected);
  g_free (selected);
}

static void
local_selection (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  gchar *input;
  gchar *clipboard;

  drag (fixture, 0, 0.1, 5.1);
  assert_selection (fixture, "alpha");
  input = read_input (fixture);
  g_assert_cmpstr (input, ==, "");
  g_free (input);
  vte_terminal_copy_clipboard_format (fixture->terminal, VTE_FORMAT_TEXT);
  clipboard = gtk_clipboard_wait_for_text (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD));
  g_assert_cmpstr (clipboard, ==, "alpha");
  g_free (clipboard);

  drag (fixture, 0, 11.1, 16.1);
  assert_selection (fixture, "gamma");
}

static void
application_selection (Fixture *fixture, gconstpointer data)
{
  gchar *input;
  guint modifiers = GPOINTER_TO_UINT (data);

  drag (fixture, modifiers, 0.1, 5.1);
  g_assert_false (vte_terminal_get_has_selection (fixture->terminal));
  input = read_input (fixture);
  g_assert_nonnull (strstr (input, "\033[<0;"));
  g_assert_nonnull (strstr (input, "\033[<32;"));
  g_assert_null (strstr (input, "\033[<4;"));
  g_free (input);
}

static void
disabled_preference (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  g_object_set (fixture->preferences, "misc-prefer-mouse-selection", FALSE, NULL);
  application_selection (fixture, NULL);
}

static void
gesture_lifetime (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 0.1);
  g_object_set (fixture->preferences, "misc-prefer-mouse-selection", FALSE, NULL);
  mouse_event (fixture, GDK_MOTION_NOTIFY, 1, GDK_BUTTON1_MASK | GDK_SHIFT_MASK, 5.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 5.1);
  assert_selection (fixture, "alpha");
}

static void
wheel_forwarding (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  gchar *input;

  drag (fixture, 0, 0.1, 5.1);
  mouse_event (fixture, GDK_SCROLL, 0, 0, 2.1);
  input = read_input (fixture);
  g_assert_nonnull (strstr (input, "\033[<64;"));
  g_assert_null (strstr (input, "\033[<68;"));
  g_free (input);
}

static void
multiple_clicks (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 1.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 1.1);
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 1.1);
  mouse_event (fixture, GDK_2BUTTON_PRESS, 1, GDK_BUTTON1_MASK, 1.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 1.1);
  assert_selection (fixture, "alpha");
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 1.1);
  mouse_event (fixture, GDK_3BUTTON_PRESS, 1, GDK_BUTTON1_MASK, 1.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 1.1);
  g_assert_true (vte_terminal_get_has_selection (fixture->terminal));
  gchar *selected = vte_terminal_get_text_selected (fixture->terminal, VTE_FORMAT_TEXT);
  g_assert_true (g_str_has_prefix (selected, "alpha beta gamma delta"));
  g_free (selected);
}



static void
single_click (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  gchar *input;

  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 1.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 1.1);
  g_assert_false (vte_terminal_get_has_selection (fixture->terminal));
  input = read_input (fixture);
  g_assert_cmpstr (input, ==, "");
  g_free (input);
}



static void
hyperlink_shortcuts (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const guint buttons[] = { 1, 2, 1 };
  const guint modifiers[] = { GDK_CONTROL_MASK, 0, 0 };

  vte_terminal_feed (fixture->terminal,
                     "\033[H\033[2K\033]8;;file:///tmp/xfce-mouse-test.txt\033\\path/to/file\033]8;;\033\\", -1);
  settle ();
  for (guint i = 0; i < G_N_ELEMENTS (buttons); i++)
    {
      gchar *input;

      g_object_set (fixture->preferences,
                    "misc-hyperlink-insert-button", 1u,
                    "misc-hyperlink-insert-modifier", modifiers[i],
                    "misc-hyperlink-insert-middle-click", TRUE,
                    NULL);
      mouse_event (fixture, GDK_BUTTON_PRESS, buttons[i], modifiers[i], 1.1);
      input = read_input (fixture);
      g_assert_cmpstr (input, ==, "/tmp/xfce-mouse-test.txt");
      g_free (input);
      mouse_event (fixture, GDK_BUTTON_RELEASE, buttons[i], modifiers[i], 1.1);
      input = read_input (fixture);
      g_free (input);
    }
}

int
main (int argc, char **argv)
{
  if (g_getenv ("TERMINAL_MOUSE_TEST_SESSION") == NULL)
    return run_isolated (argc, argv);

  g_test_init (&argc, &argv, NULL);
  gtk_init (&argc, &argv);

  g_test_add ("/mouse/selection-and-copy", Fixture, GINT_TO_POINTER (1), setup, local_selection, teardown);
  g_test_add ("/mouse/repeated-selection-without-tracking", Fixture, NULL, setup, local_selection, teardown);
  g_test_add ("/mouse/shift-bypass", Fixture, GUINT_TO_POINTER (GDK_SHIFT_MASK), setup, application_selection, teardown);
  g_test_add ("/mouse/disabled-preference", Fixture, GINT_TO_POINTER (1), setup, disabled_preference, teardown);
  g_test_add ("/mouse/gesture-lifetime", Fixture, GINT_TO_POINTER (1), setup, gesture_lifetime, teardown);
  g_test_add ("/mouse/wheel-forwarding", Fixture, GINT_TO_POINTER (1), setup, wheel_forwarding, teardown);
  g_test_add ("/mouse/multiple-clicks", Fixture, GINT_TO_POINTER (1), setup, multiple_clicks, teardown);
#if VTE_CHECK_VERSION(0, 66, 0)
#endif
  g_test_add ("/mouse/single-click", Fixture, GINT_TO_POINTER (1), setup, single_click, teardown);
  g_test_add ("/mouse/hyperlink-shortcuts", Fixture, GINT_TO_POINTER (1), setup, hyperlink_shortcuts, teardown);
  return g_test_run ();
}
