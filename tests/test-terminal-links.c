/* Real widget + isolated tmux, with harmless desktop/file-manager recorders. */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "terminal/terminal-tmux.h"

#include "terminal-test-utils.h"

static gchar *self;

static gboolean
wait_for_text (VteTerminal *terminal, const gchar *expected)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  do
    {
      gchar *text;
      gboolean found;
      settle ();
#if VTE_CHECK_VERSION(0, 80, 0)
      text = vte_terminal_get_text_format (terminal, VTE_FORMAT_TEXT);
#else
      text = vte_terminal_get_text (terminal, NULL, NULL, NULL);
#endif
      found = text != NULL && strstr (text, expected) != NULL;
      g_free (text);
      if (found)
        return TRUE;
    }
  while (g_get_monotonic_time () < deadline);
  return FALSE;
}

static GdkEvent *
pointer_event_at (Fixture *fixture, GdkEventType type, guint modifiers, gdouble column, gdouble row)
{
  GdkEvent *event = gdk_event_new (type);
  GdkWindow *window = gtk_widget_get_window (fixture->widget);
  GList *children = gdk_window_get_children (window);
  GtkBorder padding;
  GdkDevice *pointer = gdk_seat_get_pointer (gdk_display_get_default_seat (gdk_window_get_display (window)));

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
  event->any.window = g_object_ref (window);
  if (type == GDK_ENTER_NOTIFY)
    {
      event->crossing.x = padding.left + column * vte_terminal_get_char_width (fixture->terminal);
      event->crossing.y = padding.top + row * vte_terminal_get_char_height (fixture->terminal);
      event->crossing.state = modifiers;
      event->crossing.mode = GDK_CROSSING_NORMAL;
      event->crossing.detail = GDK_NOTIFY_NONLINEAR;
    }
  else if (type == GDK_MOTION_NOTIFY)
    {
      event->motion.x = padding.left + column * vte_terminal_get_char_width (fixture->terminal);
      event->motion.y = padding.top + row * vte_terminal_get_char_height (fixture->terminal);
      event->motion.state = modifiers;
    }
  else
    {
      event->button.button = 1;
      event->button.state = modifiers;
      event->button.x = padding.left + column * vte_terminal_get_char_width (fixture->terminal);
      event->button.y = padding.top + row * vte_terminal_get_char_height (fixture->terminal);
    }
  gdk_event_set_device (event, pointer);
  return event;
}

static void
click_position (Fixture *fixture, guint modifiers, gdouble column, gdouble row, GdkEventType press)
{
  for (guint release = 0; release < 2; release++)
    {
      GdkEvent *event = pointer_event_at (fixture, release ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS,
                                         modifiers | (release ? GDK_BUTTON1_MASK : 0), column, row);
      if (!release && press != GDK_BUTTON_PRESS)
        {
          gtk_widget_event (fixture->widget, event);
          event->type = press;
          event->button.state |= GDK_BUTTON1_MASK;
        }
      gtk_widget_event (fixture->widget, event);
      gdk_event_free (event);
    }
}

static void
click_at (Fixture *fixture, guint modifiers, gdouble column, GdkEventType press)
{
  click_position (fixture, modifiers, column, 0.5, press);
}

static void
expect_open (const gchar *log, const gchar *expected)
{
  gchar *actual = NULL;
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  do
    {
      g_clear_pointer (&actual, g_free);
      g_file_get_contents (log, &actual, NULL, NULL);
      if (g_strcmp0 (actual, expected) == 0)
        break;
      settle ();
    }
  while (g_get_monotonic_time () < deadline);
  g_assert_cmpstr (actual, ==, expected);
  g_free (actual);
}

static void
links_through_tmux (Fixture *fixture, gconstpointer data)
{
  const gchar *kind = data;
  gchar *root = g_dir_make_tmp ("xfce-tmux-links-XXXXXX", NULL);
  gchar *socket = g_build_filename (root, "server.sock", NULL);
  gchar *pane = g_build_filename (root, "pane", NULL);
  gchar *exports = g_build_filename (pane, "exports", NULL);
  gchar *parenthesized = g_build_filename (exports, "EXACT_ITEM_COUNT.html", NULL);
  gchar *html = g_build_filename (exports, "VOLUME.html", NULL);
  gchar *hash_filename = g_build_filename (exports, "VOLUME.html#combination_922", NULL);
  gchar *target = g_build_filename (pane, "link file.txt", NULL);
  gchar *plain = g_build_filename (pane, "source.txt", NULL);
  gchar *program = g_build_filename (root, "codex", NULL);
  gchar *manager = g_build_filename (root, "link-manager", NULL);
  gchar *log = g_build_filename (root, "opened", NULL);
  gchar *quoted_self = g_shell_quote (self);
  gchar *command = g_strconcat (quoted_self, " --record-open", NULL);
  gchar *uri, *expected, *parent_uri, *escaped;
  const gchar *selected_path;
  gboolean has_fragment = strstr (kind, "fragment") != NULL;
  gboolean literal_hash = strstr (kind, "hash-filename") != NULL;
  gboolean selected = g_strcmp0 (kind, "selected-fragment") == 0;
  gboolean wrapped = g_str_has_prefix (kind, "wrapped-");
  gboolean native = g_strcmp0 (kind, "wrapped-native") == 0;
  gboolean separate = g_strcmp0 (kind, "wrapped-separate") == 0;
  gboolean wrapped_osc8 = g_strcmp0 (kind, "wrapped-osc8") == 0;
  gdouble click_column = wrapped ? separate ? 58.5 : 8.5 : selected ? 55.5 : 3.5;
  gdouble click_row = wrapped ? 1.5 : 0.5;
  GAppInfo *handler;
  GError *error = NULL;
  pid_t child;

  g_assert_nonnull (g_getenv ("TEST_TMUX"));
  g_assert_cmpint (g_mkdir (pane, 0700), ==, 0);
  g_assert_cmpint (g_mkdir (exports, 0700), ==, 0);
  g_assert_true (g_file_set_contents (parenthesized, "<!doctype html>\n", -1, NULL));
  g_assert_true (g_file_set_contents (html, "<!doctype html><div id='combination_922'>test</div>\n", -1, NULL));
  if (literal_hash)
    g_assert_true (g_file_set_contents (hash_filename, "<!doctype html>literal hash filename\n", -1, NULL));
  g_assert_true (g_file_set_contents (target, "file for hyperlink test\n", -1, NULL));
  g_assert_true (g_file_set_contents (plain, "file for plain path test\n", -1, NULL));
  g_assert_cmpint (symlink (self, program), ==, 0);
  g_assert_cmpint (symlink (self, manager), ==, 0);
  g_setenv ("LINK_OPEN_LOG", log, TRUE);
  handler = g_app_info_create_from_commandline (command, "Hyperlink test recorder",
                                                G_APP_INFO_CREATE_SUPPORTS_URIS, &error);
  g_assert_no_error (error);
  g_assert_true (g_app_info_set_as_default_for_type (handler, "text/plain", &error));
  g_assert_no_error (error);
  g_assert_true (g_app_info_set_as_default_for_type (handler, "text/html", &error));
  g_assert_no_error (error);
  g_object_set (fixture->preferences,
                "misc-prefer-mouse-selection", FALSE,
                "misc-highlight-urls", TRUE,
                "misc-auto-detect-file-paths", TRUE,
                "misc-auto-detect-file-path-apps", g_strcmp0 (kind, "denied") == 0 ? "not-codex" : "codex",
                "misc-hyperlink-open-button", 1u,
                "misc-hyperlink-open-modifier", (guint) GDK_CONTROL_MASK,
                "misc-hyperlink-file-manager", manager,
                NULL);

  if (wrapped)
    {
      gtk_window_resize (GTK_WINDOW (fixture->window), 1600, 240);
      settle ();
    }

  child = fork ();
  g_assert_cmpint (child, >=, 0);
  if (child == 0)
    {
      if (setsid () < 0 || ioctl (fixture->slave, TIOCSCTTY, 0) < 0 || chdir (root) < 0)
        _exit (126);
      for (int fd = 0; fd <= 2; fd++)
        if (dup2 (fixture->slave, fd) < 0)
          _exit (126);
      if (native)
        {
          /* Unlike tmux, the raw emitter does not reset inherited PTY flags. */
          if (fcntl (0, F_SETFL, fcntl (0, F_GETFL) & ~O_NONBLOCK) < 0)
            _exit (126);
          execl (program, "codex", "--emit-links", pane, kind, NULL);
          _exit (127);
        }
      execl (g_getenv ("TEST_TMUX"), "tmux", "-S", socket,
             "new-session", "-A", "-s", "links", "--",
             program, "--emit-links", pane, kind, NULL);
      _exit (127);
    }
  g_assert_true (wait_for_text (fixture->terminal, "LINK-READY"));
  if (wrapped && !g_str_has_suffix (kind, "-click"))
    {
      gboolean motion = g_strcmp0 (kind, "wrapped-motion") == 0;
      GdkEvent *event = pointer_event_at (fixture, GDK_ENTER_NOTIFY, 0,
                                         motion ? 150.5 : click_column, motion ? 3.5 : click_row);
      gchar *match;
      gchar *full_path = g_strdup_printf ("%s/\n    database/telegram_backup.fragments-v7.txt", pane);
      gint tag;

      /* No preceding query or hover over row zero may prime VTE's match cache. */
      gtk_widget_event (fixture->widget, event);
      gdk_event_free (event);
      if (motion)
        {
          event = pointer_event_at (fixture, GDK_MOTION_NOTIFY, 0, click_column, click_row);
          gtk_widget_event (fixture->widget, event);
          gdk_event_free (event);
        }
      event = pointer_event_at (fixture, GDK_BUTTON_PRESS, 0, click_column, click_row);
      match = wrapped_osc8 ? vte_terminal_hyperlink_check_event (fixture->terminal, event)
                          : vte_terminal_match_check_event (fixture->terminal, event, &tag);
      g_test_message ("bottom-first match: %s", match != NULL ? match : "(none)");
      if (separate || wrapped_osc8)
        {
          g_free (full_path);
          full_path = wrapped_osc8 ? g_filename_to_uri (plain, NULL, NULL) : g_strdup ("./source.txt");
        }
      g_assert_cmpstr (match, ==, full_path);
      g_free (full_path);
      g_free (match);
      gdk_event_free (event);
    }
  if (selected)
    {
      g_object_set (fixture->preferences, "misc-highlight-urls", FALSE,
                    "misc-prefer-mouse-selection", TRUE, NULL);
      vte_terminal_set_word_char_exceptions (fixture->terminal, "-./#_");
      click_at (fixture, 0, 3.5, GDK_BUTTON_PRESS);
      click_at (fixture, 0, 3.5, GDK_2BUTTON_PRESS);
      gchar *text = vte_terminal_get_text_selected (fixture->terminal, VTE_FORMAT_TEXT);
      g_assert_cmpstr (text, ==, "exports/VOLUME.html#combination_922");
      g_free (text);
    }
  if (g_strcmp0 (kind, "timeout") == 0)
    {
      gchar *args[] = { (gchar *) g_getenv ("TEST_TMUX"), "-N", "-S", socket,
                        "display-message", "-p", "#{pid}", NULL };
      gchar *output = NULL;
      pid_t server, foreground;
      gint64 started, elapsed;
      g_assert_true (g_spawn_sync (NULL, args, NULL, 0, NULL, NULL, &output, NULL, NULL, NULL));
      server = (pid_t) g_ascii_strtoll (output, NULL, 10);
      g_free (output);
      g_assert_cmpint (server, >, 0);
      g_assert_cmpint (terminal_tmux_pane_foreground_pid (child), >, 0);
      g_assert_cmpint (kill (server, SIGSTOP), ==, 0);
      started = g_get_monotonic_time ();
      foreground = terminal_tmux_pane_foreground_pid (child);
      elapsed = g_get_monotonic_time () - started;
      /* Resume the test-owned server even if an assertion below fails. */
      kill (server, SIGCONT);
      g_assert_cmpint (foreground, ==, 0);
      g_assert_cmpint (elapsed, <, G_USEC_PER_SEC);
    }
  selected_path = has_fragment ? html : literal_hash ? hash_filename
                  : g_strcmp0 (kind, "osc8") == 0 ? target
                  : g_strcmp0 (kind, "parenthesized") == 0 ? parenthesized : plain;
  gchar *wrapped_target = wrapped ? g_build_filename (pane, "database", "telegram_backup.fragments-v7.txt", NULL) : NULL;
  if (wrapped && !separate && !wrapped_osc8)
    selected_path = wrapped_target;
  uri = g_filename_to_uri (selected_path, NULL, NULL);
  if (has_fragment)
    {
      gchar *anchored_uri = g_strconcat (uri, "#combination_922", NULL);
      g_free (uri);
      uri = anchored_uri;
    }
  expected = g_strconcat ("open ", uri, NULL);
  click_position (fixture, GDK_CONTROL_MASK, click_column, click_row, GDK_BUTTON_PRESS);
  if (g_strcmp0 (kind, "denied") == 0)
    {
      for (guint i = 0; i < 5; i++)
        settle ();
      g_assert_false (g_file_test (log, G_FILE_TEST_EXISTS));
      goto cleanup;
    }
  expect_open (log, expected);
  g_free (expected);
  g_unlink (log);

  gchar *selected_parent = g_path_get_dirname (selected_path);
  parent_uri = g_filename_to_uri (selected_parent, NULL, NULL);
  g_free (selected_parent);
  escaped = g_uri_escape_string (selected_path, "/", FALSE);
  expected = g_strdup_printf ("parent %s/?select=%s", parent_uri, escaped);
  click_position (fixture, GDK_CONTROL_MASK | GDK_SHIFT_MASK, click_column, click_row, GDK_BUTTON_PRESS);
  expect_open (log, expected);
  g_free (escaped);
  g_free (parent_uri);

cleanup:
  {
    gchar *args[] = { (gchar *) g_getenv ("TEST_TMUX"), "-N", "-S", socket, "kill-server", NULL };
    gint status;
    if (native)
      g_assert_cmpint (kill (child, SIGTERM), ==, 0);
    else
      {
        g_assert_true (g_spawn_sync (NULL, args, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL));
        g_assert_true (WIFEXITED (status));
        g_assert_cmpint (WEXITSTATUS (status), ==, 0);
      }
    g_assert_cmpint (waitpid (child, &status, 0), ==, child);
  }
  g_app_info_delete (handler);
  g_object_unref (handler);
  g_free (expected);
  g_free (uri);
  g_free (wrapped_target);
  g_free (command);
  g_free (quoted_self);
  g_free (log);
  g_free (manager);
  g_free (program);
  g_free (plain);
  g_free (target);
  g_free (parenthesized);
  g_free (hash_filename);
  g_free (html);
  g_free (exports);
  g_free (pane);
  g_free (socket);
  remove_test_directory (root);
  g_free (root);
}

int
main (int argc, char **argv)
{
  if (argc == 3 && g_strcmp0 (argv[1], "--record-open") == 0)
    {
      gchar *uri = g_path_is_absolute (argv[2]) ? g_filename_to_uri (argv[2], NULL, NULL) : g_strdup (argv[2]);
      gchar *text = g_strconcat ("open ", uri, NULL);
      return g_file_set_contents (g_getenv ("LINK_OPEN_LOG"), text, -1, NULL) ? 0 : 1;
    }
  if (argc == 2 && g_str_has_suffix (argv[0], "/link-manager"))
    {
      gchar *text = g_strconcat ("parent ", argv[1], NULL);
      return g_file_set_contents (g_getenv ("LINK_OPEN_LOG"), text, -1, NULL) ? 0 : 1;
    }
  if (argc == 4 && g_strcmp0 (argv[1], "--emit-links") == 0)
    {
      struct termios attrs;
      gchar *path, *uri, *output;
      char buffer[64];
      if (chdir (argv[2]) != 0 || tcgetattr (0, &attrs) != 0)
        return 1;
      cfmakeraw (&attrs);
      tcsetattr (0, TCSANOW, &attrs);
      path = g_build_filename (argv[2], "link file.txt", NULL);
      uri = g_filename_to_uri (path, NULL, NULL);
      if (g_strcmp0 (argv[3], "osc8") == 0)
        output = g_strdup_printf ("\033[H\033[2J\033]8;;%s\033\\FILE-LINK\033]8;;\033\\\r\nLINK-READY", uri);
      else if (g_strcmp0 (argv[3], "absolute") == 0)
        output = g_strdup_printf ("\033[H\033[2J%s/source.txt\r\nLINK-READY", argv[2]);
      else if (g_str_has_prefix (argv[3], "wrapped-"))
        {
          gchar *directory = g_build_filename (argv[2], "database", NULL);
          gchar *file = g_build_filename (directory, "telegram_backup.fragments-v7.txt", NULL);
          if (g_mkdir (directory, 0700) != 0 || !g_file_set_contents (file, "test\n", -1, NULL))
            return 1;
          if (g_strcmp0 (argv[3], "wrapped-osc8") == 0)
            {
              gchar *plain_path = g_build_filename (argv[2], "source.txt", NULL);
              gchar *tail_uri = g_filename_to_uri (plain_path, NULL, NULL);
              g_free (plain_path);
              output = g_strdup_printf ("\033[H\033[2JRepaired database (%s/\r\n    \033]8;;%s\033\\database/telegram_backup.fragments-v7.txt\033]8;;\033\\): enter this path in\r\nLINK-READY", argv[2], tail_uri);
              g_free (tail_uri);
            }
          else
            output = g_strdup_printf ("\033[H\033[2J%sRepaired database (%s/\r\n    database/telegram_backup.fragments-v7.txt): %s\r\nLINK-READY",
                                      g_strcmp0 (argv[3], "wrapped-wide") == 0 ? "\xe7\x95\x8c" "e\xcc\x81 " : "",
                                      argv[2], g_strcmp0 (argv[3], "wrapped-separate") == 0 ? "then ./source.txt" : "enter this path in");
          g_free (file);
          g_free (directory);
        }
      else if (g_strcmp0 (argv[3], "parenthesized") == 0)
        output = g_strdup ("\033[H\033[2J(exports/EXACT_ITEM_COUNT.html)\r\nLINK-READY");
      else if (g_strcmp0 (argv[3], "osc8-fragment") == 0
               || g_strcmp0 (argv[3], "osc8-hash-filename") == 0)
        {
          gchar *html_path = g_build_filename (argv[2], "exports",
                                               g_strcmp0 (argv[3], "osc8-fragment") == 0
                                                 ? "VOLUME.html" : "VOLUME.html#combination_922", NULL);
          gchar *html_uri = g_filename_to_uri (html_path, NULL, NULL);
          output = g_strdup_printf ("\033[H\033[2J\033]8;;%s%s\033\\HTML-LINK\033]8;;\033\\\r\nLINK-READY",
                                    html_uri, g_strcmp0 (argv[3], "osc8-fragment") == 0 ? "#combination_922" : "");
          g_free (html_uri);
          g_free (html_path);
        }
      else if (g_strcmp0 (argv[3], "parenthesized-fragment") == 0
               || g_strcmp0 (argv[3], "parenthesized-hash-filename") == 0)
        output = g_strdup ("\033[H\033[2J(exports/VOLUME.html#combination_922)\r\nLINK-READY");
      else if (g_strcmp0 (argv[3], "relative-fragment") == 0
               || g_strcmp0 (argv[3], "selected-fragment") == 0
               || g_strcmp0 (argv[3], "hash-filename") == 0)
        output = g_strdup ("\033[H\033[2Jexports/VOLUME.html#combination_922\r\nLINK-READY");
      else
        output = g_strdup ("\033[H\033[2J./source.txt\r\nLINK-READY");
      write (1, output, strlen (output));
      while (read (0, buffer, sizeof buffer) > 0)
        ;
      return 0;
    }

  /* Caller supplies a private HOME/XDG tree, bus and Xvfb display. */
  (void) run_isolated;
  g_assert_cmpstr (g_getenv ("TERMINAL_LINK_TEST_ISOLATED"), ==, "1");
  self = g_canonicalize_filename (argv[0], NULL);
  g_test_init (&argc, &argv, NULL);
  gtk_init (&argc, &argv);
  g_test_add ("/links/tmux/osc8", Fixture, "osc8", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/absolute", Fixture, "absolute", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/relative", Fixture, "relative", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/parenthesized", Fixture, "parenthesized", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/wrapped-hover", Fixture, "wrapped-hover", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/wrapped-click", Fixture, "wrapped-click", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/wrapped-motion", Fixture, "wrapped-motion", setup, links_through_tmux, teardown);
  g_test_add ("/links/native/wrapped-hover", Fixture, "wrapped-native", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/wrapped-wide", Fixture, "wrapped-wide", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/wrapped-osc8", Fixture, "wrapped-osc8", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/wrapped-separate", Fixture, "wrapped-separate", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/relative-fragment", Fixture, "relative-fragment", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/parenthesized-fragment", Fixture, "parenthesized-fragment", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/selected-fragment", Fixture, "selected-fragment", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/osc8-fragment", Fixture, "osc8-fragment", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/hash-filename", Fixture, "hash-filename", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/parenthesized-hash-filename", Fixture, "parenthesized-hash-filename", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/osc8-hash-filename", Fixture, "osc8-hash-filename", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/denied", Fixture, "denied", setup, links_through_tmux, teardown);
  g_test_add ("/links/tmux/timeout", Fixture, "timeout", setup, links_through_tmux, teardown);
  return g_test_run ();
}
