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
encoded_path_double_click (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const gchar *path = "/trash/(agy)qwen-daocloud-dflash2+qwen-r9700-archive-2026-09-14_01-43-17-pid-3543756.4KDLrfow";
  gchar *escaped = g_uri_escape_string (path, "/", FALSE);
  gchar *output = g_strdup_printf ("\033[H\033[2JWARNING: type '\033]8;;file://%s\033\\%s\033]8;;\033\\' to permanently delete it: ", escaped, path);
  const gchar *word_exceptions[] = { NULL, "-./?%&#:_~=@" };
  const gsize offsets[] = { 0, 7, 10, 15, strchr (path, '+') - path, strlen (path) - 1 };
  gchar *input;

  vte_terminal_feed (fixture->terminal, output, -1);
  settle ();
  for (guint i = 0; i < G_N_ELEMENTS (word_exceptions); i++)
    {
      vte_terminal_set_word_char_exceptions (fixture->terminal, word_exceptions[i]);
      for (guint j = 0; j < G_N_ELEMENTS (offsets); j++)
        {
          double_click_at (fixture, strlen ("WARNING: type '") + offsets[j]);
          assert_selection (fixture, path);
          g_assert_cmpstr (vte_terminal_get_word_char_exceptions (fixture->terminal), ==, word_exceptions[i]);
        }
    }
  input = read_input (fixture);
  g_assert_cmpstr (input, ==, "");

  g_free (input);
  g_free (output);
  g_free (escaped);
}

static void
encoded_path_with_regex (Fixture *fixture, gconstpointer data)
{
  GError *error = NULL;
  VteRegex *regex = vte_regex_new_for_match ("qwen[-a-z0-9]+", -1, PCRE2_MULTILINE, &error);

  g_assert_no_error (error);
  g_assert_nonnull (regex);
  g_assert_cmpint (vte_terminal_match_add_regex (fixture->terminal, regex, 0), >=, 0);
  vte_regex_unref (regex);
  encoded_path_double_click (fixture, data);
}

static void
file_link_punctuation (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const struct
  {
    const gchar *uri;
    const gchar *label;
  } cases[] = {
    { "file:///tmp/%28agy%29file+name.txt", "/tmp/(agy)file+name.txt" },
    { "file:///tmp/%5Bagy%5Dfile%2Bname.txt", "/tmp/[agy]file+name.txt" },
    { "file:///tmp/(agy)file+name.txt", "/tmp/(agy)file+name.txt" },
    { "file:///tmp/%28agy%29file%2Bname.txt", "file:///tmp/%28agy%29file%2Bname.txt" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gchar *output = g_strdup_printf ("\033[H\033[2J'\033]8;;%s\033\\%s\033]8;;\033\\'", cases[i].uri, cases[i].label);

      vte_terminal_feed (fixture->terminal, output, -1);
      settle ();
      for (gsize j = 0; j < strlen (cases[i].label); j++)
        {
          double_click_at (fixture, j + 1);
          assert_selection (fixture, cases[i].label);
        }
      g_free (output);
    }
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
link_labels_double_click (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const gchar *labels[] = {
    "Open (archive) + notes",
    "/tmp/Telegram Backup/(agy)file+name.txt",
    "'quoted label'",
    "x",
  };

  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      gchar *output = g_strdup_printf ("\033[H\033[2J'\033]8;;https://example.org/target\033\\%s\033]8;;\033\\'", labels[i]);

      vte_terminal_feed (fixture->terminal, output, -1);
      settle ();
      for (gsize j = 0; j < strlen (labels[i]); j++)
        {
          double_click_at (fixture, j + 1);
          assert_selection (fixture, labels[i]);
        }
      g_free (output);
    }

  gchar *input = read_input (fixture);
  g_assert_cmpstr (input, ==, "");
  g_free (input);
  vte_terminal_copy_clipboard_format (fixture->terminal, VTE_FORMAT_TEXT);
  gchar *clipboard = gtk_clipboard_wait_for_text (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD));
  g_assert_cmpstr (clipboard, ==, "x");
  g_free (clipboard);
}

static void
detected_links_double_click (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const struct { const gchar *prefix; const gchar *link; const gchar *suffix; } cases[] = {
    { "Read(", "~/Dev/fsx/README.md", ")" },
    { "WARNING: type ", "'/trash/(agy)qwen-dflash+archive'", " to permanently delete it:" },
    { "WARNING: ", "/trash/(agy)qwen-dflash+archive", " text" },
    { "type ", "'/tmp/Telegram Backup/(agy)file+name.txt'", " now" },
    { "'", "https://example.org/(archive)/file?q=a+b#anchor", "'" },
    { "'", "file:///tmp/path%20with%20spaces/(archive)", "'" },
  };

  g_object_set (fixture->preferences, "misc-highlight-urls", TRUE,
                "misc-auto-detect-file-paths", TRUE, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gchar *output = g_strdup_printf ("\033[H\033[2J%s%s%s", cases[i].prefix, cases[i].link, cases[i].suffix);

      vte_terminal_feed (fixture->terminal, output, -1);
      settle ();
      for (gsize j = 0; j < strlen (cases[i].link); j++)
        {
          double_click_at (fixture, strlen (cases[i].prefix) + j);
          assert_selection (fixture, cases[i].link);
        }
      g_free (output);
    }
}

static void
wrapped_links_double_click (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const gchar *label = "/tmp/Telegram Backup/(agy)file+name.txt";
  const gchar *url = "https://example.org/(archive)/file?q=a+b#anchor";
  glong columns = vte_terminal_get_column_count (fixture->terminal);
  gchar *prefix = g_strnfill (columns - 4, ' ');
  gchar *output = g_strdup_printf ("\033[H\033[2J%s\033]8;;file:///tmp/other\033\\%s\033]8;;\033\\ trailing", prefix, label);

  vte_terminal_feed (fixture->terminal, output, -1);
  settle ();
  for (gsize i = 0; i < strlen (label); i++)
    {
      double_click_at (fixture, strlen (prefix) + i);
      assert_selection (fixture, label);
    }
  g_free (output);

  g_object_set (fixture->preferences, "misc-highlight-urls", TRUE, NULL);
  output = g_strdup_printf ("\033[H\033[2J%s%s trailing", prefix, url);
  vte_terminal_feed (fixture->terminal, output, -1);
  settle ();
  for (gsize i = 0; i < strlen (url); i++)
    {
      double_click_at (fixture, strlen (prefix) + i);
      assert_selection (fixture, url);
    }
  g_free (output);
  g_free (prefix);
}

static void
unicode_link_double_click (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const gchar *label = "/tmp/caf\303\251-\347\225\214-e\314\201 (notes).txt";
  gchar *output = g_strdup_printf ("\033[H\033[2J'\033]8;;file:///tmp/other\033\\%s\033]8;;\033\\'", label);

  vte_terminal_feed (fixture->terminal, output, -1);
  settle ();
  /* The CJK character occupies two cells; the combining accent occupies none. */
  for (gsize i = 0; i < 26; i++)
    {
      double_click_at (fixture, i + 1);
      assert_selection (fixture, label);
    }
  g_free (output);
}

static void
link_selection_gesture (Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
  const gchar *label = "label (with spaces)";
  gchar *output = g_strdup_printf ("\033[H\033[2J\033]8;;https://example.org\033\\%s\033]8;;\033\\ trailing", label);
  gchar *input, *primary;
  glong column, row, initial_row;

  vte_terminal_feed (fixture->terminal, output, -1);
  settle ();
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 7.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 7.1);
  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 7.1);
  mouse_event (fixture, GDK_2BUTTON_PRESS, 1, GDK_BUTTON1_MASK, 7.1);
  mouse_event (fixture, GDK_MOTION_NOTIFY, 1, GDK_BUTTON1_MASK, 7.2);
  assert_selection (fixture, label);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 7.2);
  assert_selection (fixture, label);
  primary = gtk_clipboard_wait_for_text (gtk_clipboard_get (GDK_SELECTION_PRIMARY));
  g_assert_cmpstr (primary, ==, label);
  g_free (primary);

  /* Completing the synthetic selection must not leave PTY reads suspended. */
  vte_terminal_get_cursor_position (fixture->terminal, &column, &initial_row);
  const gchar *cursor_move = "\033[3;4H";
  g_assert_cmpint (write (fixture->slave, cursor_move, strlen (cursor_move)), ==, strlen (cursor_move));
  settle ();
  vte_terminal_get_cursor_position (fixture->terminal, &column, &row);
  g_assert_cmpint (column, ==, 3);
  g_assert_cmpint (row, ==, initial_row + 2);

  mouse_event (fixture, GDK_BUTTON_PRESS, 1, 0, 7.1);
  mouse_event (fixture, GDK_3BUTTON_PRESS, 1, GDK_BUTTON1_MASK, 7.1);
  mouse_event (fixture, GDK_BUTTON_RELEASE, 1, GDK_BUTTON1_MASK, 7.1);
  gchar *selected = vte_terminal_get_text_selected (fixture->terminal, VTE_FORMAT_TEXT);
  g_assert_true (g_str_has_prefix (selected, "label (with spaces) trailing"));
  g_free (selected);
  drag (fixture, 0, 0.1, 5.1);
  assert_selection (fixture, "label");
  input = read_input (fixture);
  g_assert_cmpstr (input, ==, "");
  g_free (input);
  g_free (output);
}

static void
scrolled_link_double_click (Fixture *fixture, gconstpointer data)
{
  const gchar *label = "/tmp/with spaces/(agy)file.txt";
  const gchar *filler = "filler\r\n";
  glong column, link_row;
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (fixture->widget));
  gdouble unit = 1;

#if VTE_CHECK_VERSION(0, 66, 0)
  vte_terminal_set_scroll_unit_is_pixels (fixture->terminal, data != NULL);
  if (data != NULL)
    unit = vte_terminal_get_char_height (fixture->terminal);
#else
  (void) data;
#endif
  for (guint i = 0; i < 40; i++)
    vte_terminal_feed (fixture->terminal, filler, -1);
  gchar *output = g_strdup_printf ("\033]8;;file:///tmp/other\033\\%s\033]8;;\033\\", label);
  vte_terminal_feed (fixture->terminal, output, -1);
  settle ();
  vte_terminal_get_cursor_position (fixture->terminal, &column, &link_row);
  for (guint i = 0; i < 40; i++)
    vte_terminal_feed (fixture->terminal, "\r\nfiller", -1);
  settle ();
  gtk_adjustment_set_value (adjustment, (link_row - 1.25) * unit);
  settle ();
  double_click_position (fixture, 8.1, 1.75);
  assert_selection (fixture, label);
  double_click_position (fixture, 17.1, 1.75);
  assert_selection (fixture, label);
  g_free (output);
}

static void
default_selection_link (Fixture *fixture, gconstpointer data)
{
  g_object_set (fixture->preferences, "misc-prefer-mouse-selection", FALSE, NULL);
  link_labels_double_click (fixture, data);
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
  g_test_add ("/mouse/encoded-path-double-click", Fixture, NULL, setup, encoded_path_double_click, teardown);
  g_test_add ("/mouse/encoded-path-double-click-with-tracking", Fixture, GINT_TO_POINTER (1), setup, encoded_path_double_click, teardown);
  g_test_add ("/mouse/encoded-path-double-click-with-regex", Fixture, NULL, setup, encoded_path_with_regex, teardown);
  g_test_add ("/mouse/file-link-punctuation", Fixture, NULL, setup, file_link_punctuation, teardown);
  g_test_add ("/mouse/link-labels-double-click", Fixture, NULL, setup, link_labels_double_click, teardown);
  g_test_add ("/mouse/link-labels-double-click-with-tracking", Fixture, GINT_TO_POINTER (1), setup, link_labels_double_click, teardown);
  g_test_add ("/mouse/detected-links-double-click", Fixture, NULL, setup, detected_links_double_click, teardown);
  g_test_add ("/mouse/wrapped-links-double-click", Fixture, NULL, setup, wrapped_links_double_click, teardown);
  g_test_add ("/mouse/unicode-link-double-click", Fixture, NULL, setup, unicode_link_double_click, teardown);
  g_test_add ("/mouse/link-selection-gesture", Fixture, GINT_TO_POINTER (1), setup, link_selection_gesture, teardown);
  g_test_add ("/mouse/scrolled-link-double-click", Fixture, NULL, setup, scrolled_link_double_click, teardown);
#if VTE_CHECK_VERSION(0, 66, 0)
  g_test_add ("/mouse/pixel-scrolled-link-double-click", Fixture, GINT_TO_POINTER (1), setup, scrolled_link_double_click, teardown);
#endif
  g_test_add ("/mouse/default-selection-link", Fixture, NULL, setup, default_selection_link, teardown);
  g_test_add ("/mouse/single-click", Fixture, GINT_TO_POINTER (1), setup, single_click, teardown);
  g_test_add ("/mouse/hyperlink-shortcuts", Fixture, GINT_TO_POINTER (1), setup, hyperlink_shortcuts, teardown);
  return g_test_run ();
}
