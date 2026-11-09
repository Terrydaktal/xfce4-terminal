/* Exercise the real window's clipboard accelerators, not only the VTE widget. */
#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "terminal/terminal-build-monitor.h"
#include "terminal/terminal-freshness-client.h"
#include "terminal/terminal-screen.h"
#include "terminal/terminal-window.h"
#include "terminal/terminal-update.h"

#include "terminal-test-utils.h"
#include "test-terminal-launching.h"
#include "test-terminal-update.h"
#include "freshness-test-service.h"

static gchar *self;
static gchar *build_directory;
static gchar *build_candidate;

static void
replace_build_candidate (const gchar *source)
{
  gchar *contents = NULL;
  gsize length;
  gchar *temporary = g_strconcat (build_candidate, ".new", NULL);

  if (g_str_equal (source, self))
    /* Establish an actual launch alias, not an unrelated copy of the same build. */
    g_assert_cmpint (symlink (source, temporary), ==, 0);
  else
    {
      g_assert_true (g_file_get_contents (source, &contents, &length, NULL));
      g_assert_true (g_file_set_contents (temporary, contents, length, NULL));
      g_assert_cmpint (g_chmod (temporary, 0700), ==, 0);
    }
  g_assert_cmpint (g_rename (temporary, build_candidate), ==, 0);
  g_free (temporary);
  g_free (contents);
}

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
find_menu_image (GtkWidget *widget, gpointer data)
{
  GtkWidget **image = data;

  if (*image != NULL)
    return;
  if (GTK_IS_IMAGE (widget))
    *image = widget;
  else if (GTK_IS_CONTAINER (widget))
    gtk_container_forall (GTK_CONTAINER (widget), find_menu_image, image);
}

static void
feed_full_scrollback (Fixture *fixture)
{
  GString *output = g_string_new ("\033[2J\033[H\033[1;38;2;255;0;0mscrollback-first <&> \317\200\033[0m\r\n");
  vte_terminal_set_scrollback_lines (fixture->terminal, 1000);
  for (guint i = 0; i < 120; i++)
    g_string_append_printf (output, "scrollback-line-%03u\r\n", i);
  g_string_append (output, "scrollback-last");
  vte_terminal_feed (fixture->terminal, output->str, output->len);
  g_string_free (output, TRUE);
  settle ();
  g_assert_cmpint (vte_terminal_get_row_count (fixture->terminal), <, 120);
}


static void
copy_all_scrollback (Fixture *fixture, gconstpointer data)
{
  const gboolean as_html = GPOINTER_TO_INT (data);
  const gchar *labels[] = { "_Copy", "Copy as _HTML", "Copy All Scrollback", "Copy All Scrollback as HTML",
                            "Save All Scrollback...", "Save All Scrollback as HTML..." };
  TerminalScreen *screen = TERMINAL_SCREEN (gtk_widget_get_ancestor (fixture->widget, TERMINAL_TYPE_SCREEN));
  GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  GtkSettings *settings = gtk_widget_get_settings (fixture->widget);
  GdkAtom html_target = gdk_atom_intern_static_string ("text/html");
  GtkWidget *menu = NULL;
  GtkWidget *copy_items[G_N_ELEMENTS (labels)] = { NULL };
  GList *items;
  gchar *text;
  gchar buffer[16];
  gboolean menu_images;

  feed_full_scrollback (fixture);
  g_assert_false (vte_terminal_get_has_selection (fixture->terminal));

  g_object_get (settings, "gtk-menu-images", &menu_images, NULL);
  g_object_set (settings, "gtk-menu-images", FALSE, NULL);
  g_signal_emit_by_name (screen, "get-context-menu", &menu);
  g_assert_nonnull (menu);
  g_object_ref_sink (menu);
  gtk_widget_show_all (menu);
  items = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *item = items; item != NULL; item = item->next)
    for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
      if (g_strcmp0 (gtk_menu_item_get_label (item->data), labels[i]) == 0)
        {
          g_assert_null (copy_items[i]);
          if (i > 0)
            {
              g_assert_nonnull (copy_items[i - 1]);
              g_assert_nonnull (item->prev);
              g_assert_true (item->prev->data == copy_items[i - 1]);
            }
          copy_items[i] = item->data;
        }
  g_list_free (items);
  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      g_assert_nonnull (copy_items[i]);
      g_assert_cmpint (gtk_widget_get_sensitive (copy_items[i]), ==, i >= 2);
      if (i >= 2)
        {
          GtkWidget *image = NULL;
          const gchar *icon_name = NULL;
          find_menu_image (copy_items[i], &image);
          g_assert_true (GTK_IS_IMAGE (image));
          g_assert_true (gtk_widget_get_visible (image));
          gtk_image_get_icon_name (GTK_IMAGE (image), &icon_name, NULL);
          g_assert_cmpstr (icon_name, ==, i < 4 ? "edit-copy" : "document-save-as");
        }
    }

  gtk_clipboard_set_text (clipboard, "previous clipboard", -1);
  gtk_menu_item_activate (GTK_MENU_ITEM (copy_items[as_html ? 3 : 2]));
  text = gtk_clipboard_wait_for_text (clipboard);
  g_assert_nonnull (text);
  g_assert_nonnull (strstr (text, as_html ? "scrollback-first &lt;&amp;&gt; \317\200" : "scrollback-first <&> \317\200"));
  g_assert_nonnull (strstr (text, "scrollback-last"));
  for (guint i = 0; i < 120; i++)
    {
      gchar *line = g_strdup_printf ("scrollback-line-%03u", i);
      g_assert_nonnull (strstr (text, line));
      g_free (line);
    }
  g_assert_cmpint (gtk_clipboard_wait_is_target_available (clipboard, html_target), ==, as_html);
  if (as_html)
    {
      GtkSelectionData *selection = gtk_clipboard_wait_for_contents (clipboard, html_target);
      GError *error = NULL;
      gchar *html;
      g_assert_nonnull (selection);
      g_assert_cmpint (gtk_selection_data_get_length (selection), >, 0);
      html = g_convert ((const gchar *) gtk_selection_data_get_data (selection),
                        gtk_selection_data_get_length (selection), "UTF-8", "UTF-16", NULL, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (html);
      g_assert_cmpstr (html, ==, text);
      g_assert_nonnull (strstr (html, "scrollback-first &lt;&amp;&gt; \317\200"));
      g_assert_nonnull (strstr (html, "scrollback-last"));
      g_assert_nonnull (strstr (html, "#FF0000"));
      g_assert_nonnull (strstr (html, "<b>"));
      g_free (html);
      gtk_selection_data_free (selection);
    }
  g_free (text);
  g_assert_true (vte_terminal_get_has_selection (fixture->terminal));
  g_assert_cmpint (read (fixture->slave, buffer, sizeof buffer), ==, -1);
  g_assert_cmpint (errno, ==, EAGAIN);
  gtk_clipboard_clear (clipboard);
  gtk_widget_destroy (menu);
  g_object_unref (menu);
  g_object_set (settings, "gtk-menu-images", menu_images, NULL);
}


typedef struct
{
  Fixture *fixture;
  const gchar *directory;
  gboolean as_html, cancel;
  guint attempts;
  GtkWidget *dialog;
} SaveDialog;

static gboolean
respond_save_dialog (gpointer data)
{
  SaveDialog *save = data;
  GList *windows = gtk_window_list_toplevels ();
  g_assert_cmpuint (++save->attempts, <, 100);
  for (GList *item = windows; item != NULL; item = item->next)
    if (GTK_IS_FILE_CHOOSER_DIALOG (item->data)
        && gtk_window_get_transient_for (item->data) == GTK_WINDOW (save->fixture->window))
      {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (item->data);
        if (save->dialog == NULL)
          {
            gchar *name = gtk_file_chooser_get_current_name (chooser);
            g_assert_cmpstr (name, ==, save->as_html ? "scrollback.html" : "scrollback.txt");
            g_free (name);
            g_assert_true (gtk_file_chooser_get_do_overwrite_confirmation (chooser));
            g_assert_true (gtk_file_chooser_set_current_folder (chooser, save->directory));
            gtk_file_chooser_set_current_name (chooser, "custom export");
            save->dialog = item->data;
          }
        else
          gtk_dialog_response (GTK_DIALOG (item->data), save->cancel ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_ACCEPT);
        break;
      }
  g_list_free (windows);
  return G_SOURCE_CONTINUE;
}

static void
save_all_scrollback (Fixture *fixture, gconstpointer data)
{
  gboolean as_html = GPOINTER_TO_INT (data) & 1;
  gboolean cancel = GPOINTER_TO_INT (data) & 2;
  TerminalScreen *screen = TERMINAL_SCREEN (gtk_widget_get_ancestor (fixture->widget, TERMINAL_TYPE_SCREEN));
  GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (fixture->terminal));
  gchar *directory = g_dir_make_tmp ("terminal-save-scrollback.XXXXXX", NULL);
  gchar *path = g_build_filename (directory, "custom export", NULL);
  GtkWidget *menu = NULL, *action = NULL;
  SaveDialog save = { fixture, directory, as_html, cancel, 0, NULL };
  gchar *before, *after, *text;
  gchar buffer[16];

  if (cancel && !as_html)
    g_assert_true (g_file_set_contents (path, "existing export must remain", -1, NULL));
  feed_full_scrollback (fixture);
  vte_terminal_select_all (fixture->terminal);
  before = vte_terminal_get_text_selected (fixture->terminal, VTE_FORMAT_TEXT);
  gtk_clipboard_set_text (clipboard, "clipboard must not change", -1);
  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_lower (adjustment));
  gdouble position = gtk_adjustment_get_value (adjustment);
  GOutputStream *snapshot = g_memory_output_stream_new_resizable ();
  GError *error = NULL;
  g_assert_true (terminal_screen_save_scrollback (screen, snapshot, as_html, &error));
  g_assert_no_error (error);
  g_object_unref (snapshot);
  after = vte_terminal_get_text_selected (fixture->terminal, VTE_FORMAT_TEXT);
  g_assert_cmpstr (before, ==, after);
  g_free (after);
  g_free (before);
  g_signal_emit_by_name (screen, "get-context-menu", &menu);
  g_assert_nonnull (menu);
  g_object_ref_sink (menu);
  GList *items = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *item = items; item != NULL; item = item->next)
    {
      const gchar *label = gtk_menu_item_get_label (item->data);
      g_assert_cmpstr (label, !=, "Sa_ve Contents...");
      if (g_strcmp0 (label, as_html ? "Save All Scrollback as HTML..." : "Save All Scrollback...") == 0)
        {
          g_assert_null (action);
          action = item->data;
        }
    }
  g_list_free (items);
  g_assert_nonnull (action);
  guint timer = g_timeout_add (50, respond_save_dialog, &save);
  gtk_menu_item_activate (GTK_MENU_ITEM (action));
  g_source_remove (timer);
  g_assert_nonnull (save.dialog);
  if (cancel && !as_html)
    {
      g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
      g_assert_cmpstr (text, ==, "existing export must remain");
      g_free (text);
    }
  else if (cancel)
    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  else
    {
      g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
      g_assert_true (g_utf8_validate (text, -1, NULL));
      g_assert_nonnull (strstr (text, as_html ? "scrollback-first &lt;&amp;&gt; \317\200" : "scrollback-first <&> \317\200"));
      g_assert_nonnull (strstr (text, "scrollback-last"));
      for (guint i = 0; i < 120; i++)
        {
          gchar *line = g_strdup_printf ("scrollback-line-%03u", i);
          g_assert_nonnull (strstr (text, line));
          g_free (line);
        }
      if (as_html)
        {
          g_assert_true (g_str_has_prefix (text, "<!doctype html>"));
          g_assert_nonnull (strstr (text, "<meta charset=\"utf-8\">"));
          g_assert_nonnull (strstr (text, "#FF0000"));
          g_assert_nonnull (strstr (text, "<b>"));
          g_assert_true (g_str_has_suffix (text, "</body></html>\n"));
        }
      else
        g_assert_null (strstr (text, "<pre>"));
      g_free (text);
    }
  text = gtk_clipboard_wait_for_text (clipboard);
  g_assert_cmpstr (text, ==, "clipboard must not change");
  g_free (text);
  g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), ==, position);
  g_assert_cmpint (read (fixture->slave, buffer, sizeof buffer), ==, -1);
  g_assert_cmpint (errno, ==, EAGAIN);
  gtk_clipboard_clear (clipboard);
  gtk_widget_destroy (menu);
  g_object_unref (menu);
  remove_test_directory (directory);
  g_free (path);
  g_free (directory);
}

static void
save_scrollback_empty_and_truncated (Fixture *fixture, gconstpointer data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (gtk_widget_get_ancestor (fixture->widget, TERMINAL_TYPE_SCREEN));
  gboolean as_html = GPOINTER_TO_INT (data);
  feed_full_scrollback (fixture);
  vte_terminal_set_scrollback_lines (fixture->terminal, 12);
  settle ();
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (fixture->terminal));
  glong row;
  vte_terminal_get_cursor_position (fixture->terminal, NULL, &row);
  g_assert_cmpfloat (row, >, gtk_adjustment_get_upper (adjustment));
  for (guint empty = 0; empty < 2; empty++)
    {
      GOutputStream *stream = g_memory_output_stream_new_resizable ();
      GError *error = NULL;
      g_assert_true (terminal_screen_save_scrollback (screen, stream, as_html, &error));
      g_assert_no_error (error);
      gsize length = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (stream));
      gchar *text = length > 0 ? g_strndup (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (stream)), length) : g_strdup ("");
      g_assert_null (strstr (text, "scrollback-first"));
      g_assert_null (strstr (text, "scrollback-line-000"));
      g_assert_cmpint (strstr (text, "scrollback-last") != NULL, ==, !empty);
      if (as_html)
        {
          g_assert_nonnull (strstr (text, "<pre>"));
          g_assert_true (g_str_has_suffix (text, "</body></html>\n"));
        }
      g_free (text);
      g_object_unref (stream);
      vte_terminal_reset (fixture->terminal, TRUE, TRUE);
      settle ();
    }
}

static void
save_scrollback_stream_failure (Fixture *fixture, gconstpointer data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (gtk_widget_get_ancestor (fixture->widget, TERMINAL_TYPE_SCREEN));
  GOutputStream *stream = g_memory_output_stream_new_resizable ();
  GError *error = NULL;
  g_assert_true (g_output_stream_close (stream, NULL, NULL));
  g_assert_false (terminal_screen_save_scrollback (screen, stream, GPOINTER_TO_INT (data), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED);
  g_clear_error (&error);
  g_object_unref (stream);
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

static void
wait_window_title (GtkWidget *window,
                   const gchar *expected)
{
  gint64 deadline = g_get_monotonic_time () + 6 * G_TIME_SPAN_SECOND;

  while (g_strcmp0 (gtk_window_get_title (GTK_WINDOW (window)), expected) != 0
         && g_get_monotonic_time () < deadline)
    settle ();
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (window)), ==, expected);
}

static void
wait_current_launch_alias (void)
{
  TerminalFreshnessClient *client = terminal_freshness_client_new ();
  terminal_freshness_client_set_target (client, getpid (), 0, 0, 0, build_candidate);
  gint64 deadline = g_get_monotonic_time () + 6 * G_TIME_SPAN_SECOND;
  while (!terminal_freshness_client_is_known (client) && g_get_monotonic_time () < deadline)
    settle ();
  g_assert_true (terminal_freshness_client_is_known (client));
  g_assert_false (terminal_freshness_client_is_stale (client));
  g_object_unref (client);
}

static void
window_build_title (Fixture *fixture,
                    gconstpointer data)
{
  GSubprocess *service = freshness_test_service_start ();
  wait_current_launch_alias ();
  const gchar *struck = "T\314\266e\314\266r\314\266m\314\266i\314\266n\314\266a\314\266l\314\266";
  TerminalTabAttr attr = { 0 };
  TerminalScreen *first = TERMINAL_SCREEN (gtk_widget_get_ancestor (fixture->widget, TERMINAL_TYPE_SCREEN));
  TerminalScreen *second = terminal_screen_new (&attr, 80, 24);
  GtkWidget *other = terminal_window_new (NULL, FALSE, TERMINAL_VISIBILITY_HIDE,
                                         TERMINAL_VISIBILITY_HIDE, TERMINAL_VISIBILITY_HIDE);
  gchar *expected = g_strconcat ("Codex - diet - ", struck, NULL);
  gchar *other_expected = g_strconcat ("Fish - ", struck, NULL);
  gchar *raw;
  gboolean urgency = gtk_window_get_urgency_hint (GTK_WINDOW (fixture->window));

  /* Both windows share a checker, but preserve their independent raw tab titles. */
  g_object_set (first, "custom-title", "Codex - diet - Terminal", NULL);
  g_object_set (second, "custom-title", "Fish", NULL);
  terminal_window_add (TERMINAL_WINDOW (other), second);
  gtk_widget_show (other);
  wait_window_title (fixture->window, "Codex - diet - Terminal");
  wait_window_title (other, "Fish");
  replace_build_candidate ("/usr/bin/true");
  wait_window_title (fixture->window, expected);
  wait_window_title (other, other_expected);
  raw = terminal_screen_get_title (first);
  g_assert_cmpstr (raw, ==, "Codex - diet - Terminal");
  g_free (raw);
  g_assert_cmpint (gtk_window_get_urgency_hint (GTK_WINDOW (fixture->window)), ==, urgency);

  /* A new OSC title while stale must still pass through the decorator. */
  g_object_set (fixture->preferences, "title-mode", TERMINAL_TITLE_REPLACE, NULL);
  g_object_set (second, "custom-title", NULL, NULL);
  vte_terminal_feed (VTE_TERMINAL (find_terminal (GTK_WIDGET (second))), "\033]0;Gemini - Terminal\a", -1);
  g_free (other_expected);
  other_expected = g_strconcat ("Gemini - ", struck, NULL);
  wait_window_title (other, other_expected);
  replace_build_candidate (self);
  wait_window_title (fixture->window, "Codex - diet - Terminal");
  wait_window_title (other, "Gemini - Terminal");
  g_object_ref (other);
  gtk_widget_destroy (other);
  replace_build_candidate ("/usr/bin/true");
  wait_window_title (fixture->window, expected);
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (other)), ==, "Gemini - Terminal");
  g_object_unref (other);
  replace_build_candidate (self);
  wait_window_title (fixture->window, "Codex - diet - Terminal");
  g_free (other_expected);
  g_free (expected);
  freshness_test_service_stop (service);
}

static void
replace_application_image (const gchar *source,
                           const gchar *target)
{
  gchar *contents, *temporary = g_strconcat (target, ".new", NULL);
  gsize length;
  g_assert_true (g_file_get_contents (source, &contents, &length, NULL));
  g_assert_true (g_file_set_contents (temporary, contents, length, NULL));
  g_assert_cmpint (g_chmod (temporary, 0700), ==, 0);
  g_assert_cmpint (g_rename (temporary, target), ==, 0);
  g_free (contents);
  g_free (temporary);
}

static void
window_application_title (void)
{
  GSubprocess *service = freshness_test_service_start ();
  wait_current_launch_alias ();
  const gchar *struck = "c\314\266o\314\266d\314\266e\314\266x\314\266";
  const gchar *struck_terminal = "T\314\266e\314\266r\314\266m\314\266i\314\266n\314\266a\314\266l\314\266";
  gchar *directory = g_dir_make_tmp ("terminal-window-application.XXXXXX", NULL);
  gchar *program = g_build_filename (directory, "codex", NULL);
  gchar *runtime = g_build_filename (directory, "node", NULL);
  gchar *arguments[] = { program, "--hold", NULL };
  gchar *runtime_arguments[] = { runtime, "--hold", NULL };
  gchar *expected = g_strconcat (struck, " - project - Terminal", NULL);
  gchar *both = g_strconcat (struck, " - project - ", struck_terminal, NULL);
  TerminalTabAttr attr = { .command = arguments, .hold = TRUE };
  TerminalTabAttr other_attr = { .command = runtime_arguments, .hold = TRUE };
  TerminalPreferences *preferences = terminal_preferences_get ();
  GtkWidget *window = terminal_window_new (NULL, FALSE, TERMINAL_VISIBILITY_HIDE,
                                           TERMINAL_VISIBILITY_HIDE, TERMINAL_VISIBILITY_HIDE);
  TerminalScreen *screen, *other;
  gchar *raw;

  replace_application_image (APPLICATION_FIXTURE, program);
  replace_application_image (APPLICATION_FIXTURE, runtime);
  screen = terminal_screen_new (&attr, 80, 24);
  /* A normal native process with no cooperation beyond an ordinary OSC title. */
  g_object_set (screen, "custom-title", expected, NULL);
  terminal_window_add (TERMINAL_WINDOW (window), screen);
  gtk_widget_show (window);
  terminal_screen_launch_child (screen);
  wait_window_title (window, "codex - project - Terminal");

  replace_application_image (APPLICATION_FIXTURE_NEXT, program);
  wait_window_title (window, expected);
  raw = terminal_screen_get_title (screen);
  g_assert_cmpstr (raw, ==, expected); /* The raw title is not rewritten by the terminal. */
  g_free (raw);
  g_assert_false (gtk_window_get_urgency_hint (GTK_WINDOW (window)));

  other = terminal_screen_new (&other_attr, 80, 24);
  g_object_set (other, "custom-title", "node - other - Terminal", NULL);
  terminal_window_add (TERMINAL_WINDOW (window), other);
  terminal_screen_launch_child (other);
  wait_window_title (window, "node - other - Terminal");
  GtkNotebook *notebook = GTK_NOTEBOOK (gtk_widget_get_ancestor (GTK_WIDGET (screen), GTK_TYPE_NOTEBOOK));
  g_assert_nonnull (notebook);
  gtk_notebook_set_current_page (notebook, 0);
  wait_window_title (window, expected);

  replace_build_candidate ("/usr/bin/true");
  wait_window_title (window, both);
  replace_build_candidate (self);
  wait_window_title (window, expected);

  terminal_screen_feed_text (screen, "e");
  wait_window_title (window, "codex - project - Terminal");
  g_object_ref (window);
  gtk_widget_destroy (window);
  gchar *destroyed_title = g_strdup (gtk_window_get_title (GTK_WINDOW (window)));
  replace_application_image (APPLICATION_FIXTURE, program);
  settle ();
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (window)), ==, destroyed_title);
  g_free (destroyed_title);
  g_object_unref (window);
  g_object_unref (preferences);
  remove_test_directory (directory);
  g_free (program);
  g_free (runtime);
  g_free (directory);
  g_free (expected);
  g_free (both);
  freshness_test_service_stop (service);
}

static void
wait_update_item (GtkWidget *item, gboolean sensitive)
{
  TerminalBuildMonitor *monitor = terminal_build_monitor_get ();
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  terminal_build_monitor_refresh (monitor);
  while (gtk_widget_get_sensitive (item) != sensitive && g_get_monotonic_time () < deadline)
    settle ();
  g_assert_cmpint (gtk_widget_get_sensitive (item), ==, sensitive);
  g_assert_true (gtk_widget_get_visible (item));
  g_object_unref (monitor);
}

static void
unexpected_launcher_call (GDBusConnection *connection,
                          const gchar *sender,
                          const gchar *path,
                          const gchar *interface,
                          const gchar *method,
                          GVariant *parameters,
                          GDBusMethodInvocation *invocation,
                          gpointer data)
{
  guint *calls = data;
  (*calls)++;
  g_dbus_method_invocation_return_error (
    invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
    "Terminal-initiated updates must not contact Application Launcher");
}

static GDBusConnection *
watch_launcher_calls (guint *calls, guint *registration)
{
  GError *error = NULL;
  GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (
    "<node><interface name='com.terrydaktal.ApplicationLauncher.Tracker1'>"
    "<method name='BeginTerminalUpgradePreview'><arg type='s' direction='out'/></method>"
    "<method name='GetTerminalUpgradeReport'><arg type='s' direction='in'/>"
    "<arg type='s' direction='out'/></method>"
    "<method name='StartTerminalUpgrade'><arg type='s' direction='in'/>"
    "<arg type='as' direction='in'/><arg type='b' direction='in'/>"
    "<arg type='s' direction='out'/></method></interface></node>",
    &error);
  g_assert_no_error (error);
  static const GDBusInterfaceVTable table = { .method_call = unexpected_launcher_call };
  *registration = g_dbus_connection_register_object (
    connection, "/Tracker", node->interfaces[0], &table, calls, NULL, &error);
  g_assert_no_error (error);
  g_dbus_node_info_unref (node);
  GVariant *reply = g_dbus_connection_call_sync (
    connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "RequestName",
    g_variant_new ("(su)", "com.terrydaktal.ApplicationLauncher", 4),
    G_VARIANT_TYPE ("(u)"), 0, 1000, NULL, &error);
  g_assert_no_error (error);
  guint result;
  g_variant_get (reply, "(u)", &result);
  g_assert_cmpuint (result, ==, 1);
  g_variant_unref (reply);
  return connection;
}

static void
update_context_menu (Fixture *fixture, gconstpointer data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (gtk_widget_get_ancestor (fixture->widget, TERMINAL_TYPE_SCREEN));
  GtkWidget *menu = NULL;
  GtkWidget *update = NULL;
  GList *items;
  guint launcher_calls = 0, registration = 0;
  GDBusConnection *launcher = data != NULL ? watch_launcher_calls (&launcher_calls, &registration) : NULL;
  g_signal_emit_by_name (screen, "get-context-menu", &menu);
  g_assert_nonnull (menu);
  g_object_ref_sink (menu);
  gtk_widget_show_all (menu);
  items = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *item = items; item != NULL; item = item->next)
    if (g_strcmp0 (gtk_menu_item_get_label (item->data), "_Update in Place") == 0)
      update = item->data;
  g_list_free (items);
  g_assert_nonnull (update);
  g_assert_true (gtk_widget_get_visible (update));
  g_assert_false (gtk_widget_get_sensitive (update));
  gtk_menu_item_activate (GTK_MENU_ITEM (update));
  g_assert_false (terminal_update_is_pending (GTK_WINDOW (fixture->window)));

  /* An open menu follows availability, independently of the stale-title state. */
  replace_build_candidate ("/usr/bin/true");
  wait_update_item (update, TRUE);
  g_assert_cmpint (g_chmod (build_candidate, 0600), ==, 0);
  wait_update_item (update, FALSE);
  g_assert_cmpint (g_chmod (build_candidate, 0700), ==, 0);
  wait_update_item (update, TRUE);
  replace_build_candidate ("/dev/null");
  wait_update_item (update, FALSE);
  gtk_menu_item_activate (GTK_MENU_ITEM (update));
  g_assert_false (terminal_update_is_pending (GTK_WINDOW (fixture->window)));
  replace_build_candidate ("/usr/bin/true");
  wait_update_item (update, TRUE);
  g_assert_cmpint (g_unlink (build_candidate), ==, 0);
  wait_update_item (update, FALSE);
  replace_build_candidate (self);
  wait_update_item (update, FALSE);
  replace_build_candidate ("/usr/bin/true");
  wait_update_item (update, TRUE);
  gtk_menu_item_activate (GTK_MENU_ITEM (update));
  g_assert_true (terminal_update_is_pending (GTK_WINDOW (fixture->window)));
  gtk_widget_destroy (menu);
  g_object_unref (menu);
  replace_build_candidate (self);
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (terminal_update_is_pending (GTK_WINDOW (fixture->window)) && g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
  g_assert_false (terminal_update_is_pending (GTK_WINDOW (fixture->window)));
  g_assert_true (gtk_widget_get_visible (fixture->window));
  GList *windows = gtk_window_list_toplevels ();
  for (GList *item = windows; item != NULL; item = item->next)
    if (GTK_IS_DIALOG (item->data) && gtk_window_get_transient_for (item->data) == GTK_WINDOW (fixture->window))
      gtk_widget_destroy (item->data);
  g_list_free (windows);
  if (launcher != NULL)
    {
      g_assert_cmpuint (launcher_calls, ==, 0);
      g_dbus_connection_unregister_object (launcher, registration);
      GVariant *reply = g_dbus_connection_call_sync (
        launcher, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ReleaseName",
        g_variant_new ("(s)", "com.terrydaktal.ApplicationLauncher"),
        G_VARIANT_TYPE ("(u)"), 0, 1000, NULL, NULL);
      g_assert_nonnull (reply);
      g_variant_unref (reply);
      g_object_unref (launcher);
    }
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
  self = g_file_read_link ("/proc/self/exe", NULL);
  g_assert_nonnull (self);
  build_directory = g_dir_make_tmp ("terminal-window-build.XXXXXX", NULL);
  g_assert_nonnull (build_directory);
  build_candidate = g_build_filename (build_directory, "xfce4-terminal", NULL);
  replace_build_candidate (self);
  terminal_build_monitor_initialize (build_candidate);
  g_assert_null (g_getenv ("XFCE4_TERMINAL_LAUNCH_PATH"));
  g_assert_null (g_getenv ("XFCE4_TERMINAL_LAUNCH_KIND"));
  if (!gtk_init_check (&argc, &argv))
    return 77;
  terminal_update_register_tests (build_candidate);
  terminal_launching_register_tests ();
  g_test_add_func ("/terminal/title/active-application-build", window_application_title);
  g_test_add ("/terminal/update/context-menu", Fixture, NULL, setup_window, update_context_menu, teardown);
  g_test_add ("/terminal/update/context-menu-with-launcher", Fixture, GINT_TO_POINTER (TRUE), setup_window, update_context_menu, teardown);
  g_test_add ("/terminal/title/stale-build-and-rollback", Fixture, NULL,
              setup_window, window_build_title, teardown);
  g_test_add ("/terminal/copy/native-selection", Fixture, NULL,
              setup_window, copy_native_selection, teardown);
  g_test_add ("/terminal/copy/all-scrollback-text", Fixture, NULL,
              setup_window, copy_all_scrollback, teardown);
  g_test_add ("/terminal/copy/all-scrollback-html", Fixture, GINT_TO_POINTER (TRUE),
              setup_window, copy_all_scrollback, teardown);
  g_test_add ("/terminal/save/all-scrollback-text", Fixture, NULL,
              setup_window, save_all_scrollback, teardown);
  g_test_add ("/terminal/save/all-scrollback-html", Fixture, GINT_TO_POINTER (1),
              setup_window, save_all_scrollback, teardown);
  g_test_add ("/terminal/save/cancel-text", Fixture, GINT_TO_POINTER (2),
              setup_window, save_all_scrollback, teardown);
  g_test_add ("/terminal/save/cancel-html", Fixture, GINT_TO_POINTER (3),
              setup_window, save_all_scrollback, teardown);
  g_test_add ("/terminal/save/write-error-text", Fixture, NULL,
              setup_window, save_scrollback_stream_failure, teardown);
  g_test_add ("/terminal/save/write-error-html", Fixture, GINT_TO_POINTER (TRUE),
              setup_window, save_scrollback_stream_failure, teardown);
  g_test_add ("/terminal/save/empty-and-truncated-text", Fixture, NULL,
              setup_window, save_scrollback_empty_and_truncated, teardown);
  g_test_add ("/terminal/save/empty-and-truncated-html", Fixture, GINT_TO_POINTER (TRUE),
              setup_window, save_scrollback_empty_and_truncated, teardown);
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
  remove_test_directory (build_directory);
  g_free (build_directory);
  g_free (build_candidate);
  g_free (self);
  return result;
}
