#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "terminal/terminal-build-id.h"
#include "terminal/terminal-build-monitor.h"

typedef struct
{
  gchar *directory;
  gchar *running;
  gchar *candidate;
  TerminalBuildMonitor *monitor;
  guint changes;
} BuildFixture;

static void
put_integer (guint8 *bytes,
             guint size,
             guint64 value,
             gboolean little_endian)
{
  for (guint i = 0; i < size; i++)
    {
      bytes[little_endian ? i : size - i - 1] = value & 255;
      value >>= 8;
    }
}

static void
make_image (guint8 *bytes,
            gboolean is64,
            gboolean little_endian,
            guint8 identity)
{
  guint phsize = is64 ? 56 : 32;

  memset (bytes, 0, 320);
  memcpy (bytes, "\177ELF", 4);
  bytes[4] = is64 ? 2 : 1;
  bytes[5] = little_endian ? 1 : 2;
  bytes[6] = 1;
  put_integer (bytes + 16, 2, 3, little_endian);
  put_integer (bytes + 20, 4, 1, little_endian);
  put_integer (bytes + (is64 ? 32 : 28), is64 ? 8 : 4, 64, little_endian);
  put_integer (bytes + (is64 ? 52 : 40), 2, is64 ? 64 : 52, little_endian);
  put_integer (bytes + (is64 ? 54 : 42), 2, phsize, little_endian);
  put_integer (bytes + (is64 ? 56 : 44), 2, 2, little_endian);
  put_integer (bytes + 64, 4, 4, little_endian); /* PT_NOTE */
  put_integer (bytes + 64 + (is64 ? 8 : 4), is64 ? 8 : 4, 192, little_endian);
  put_integer (bytes + 64 + (is64 ? 32 : 16), is64 ? 8 : 4, 24, little_endian);
  put_integer (bytes + 64 + phsize, 4, 1, little_endian); /* PT_LOAD */
  put_integer (bytes + 64 + phsize + (is64 ? 32 : 16), is64 ? 8 : 4, 320, little_endian);
  put_integer (bytes + 192, 4, 4, little_endian);
  put_integer (bytes + 196, 4, 8, little_endian);
  put_integer (bytes + 200, 4, 3, little_endian); /* NT_GNU_BUILD_ID */
  memcpy (bytes + 204, "GNU\0", 4);
  memset (bytes + 208, identity, 8);
}

static void
write_image (const gchar *path,
             guint8 identity)
{
  guint8 bytes[320];
  gchar *temporary = g_strconcat (path, ".new", NULL);
  GError *error = NULL;

  make_image (bytes, TRUE, TRUE, identity);
  g_assert_true (g_file_set_contents (temporary, (const gchar *) bytes, sizeof bytes, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_rename (temporary, path), ==, 0);
  g_free (temporary);
}

static void
remove_directory (const gchar *path)
{
  GDir *directory = g_dir_open (path, 0, NULL);
  const gchar *name;

  if (directory == NULL)
    return;
  while ((name = g_dir_read_name (directory)) != NULL)
    {
      gchar *child = g_build_filename (path, name, NULL);
      if (!g_file_test (child, G_FILE_TEST_IS_SYMLINK) && g_file_test (child, G_FILE_TEST_IS_DIR))
        remove_directory (child);
      else
        g_unlink (child);
      g_free (child);
    }
  g_dir_close (directory);
  g_assert_cmpint (g_rmdir (path), ==, 0);
}

static void
process_events (gint64 milliseconds)
{
  gint64 deadline = g_get_monotonic_time () + milliseconds * 1000;

  do
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);
}

static void
wait_stale (TerminalBuildMonitor *monitor,
            gboolean expected,
            guint seconds)
{
  gint64 deadline = g_get_monotonic_time () + seconds * G_TIME_SPAN_SECOND;

  while (terminal_build_monitor_is_stale (monitor) != expected && g_get_monotonic_time () < deadline)
    process_events (10);
  g_assert_cmpint (terminal_build_monitor_is_stale (monitor), ==, expected);
}

static void
build_changed (TerminalBuildMonitor *monitor,
               BuildFixture *fixture)
{
  fixture->changes++;
}

static void
setup (BuildFixture *fixture,
       gconstpointer data)
{
  GError *error = NULL;

  fixture->directory = g_dir_make_tmp ("terminal-build-test.XXXXXX", &error);
  g_assert_no_error (error);
  fixture->running = g_build_filename (fixture->directory, "running", NULL);
  fixture->candidate = g_build_filename (fixture->directory, "candidate", NULL);
  write_image (fixture->running, 0xaa);
  write_image (fixture->candidate, 0xaa);
}

static void
start_monitor (BuildFixture *fixture,
               const gchar *entry,
               TerminalBuildLaunchKind kind)
{
  fixture->monitor = terminal_build_monitor_new (fixture->running, entry, kind);
  g_signal_connect (fixture->monitor, "changed", G_CALLBACK (build_changed), fixture);
}

static void
teardown (BuildFixture *fixture,
          gconstpointer data)
{
  gpointer remaining = fixture->monitor;
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  if (remaining != NULL)
    {
      g_signal_handlers_disconnect_by_data (remaining, fixture);
      g_object_add_weak_pointer (remaining, &remaining);
      g_clear_object (&fixture->monitor);
      while (remaining != NULL && g_get_monotonic_time () < deadline)
        process_events (10);
      g_assert_null (remaining);
    }
  remove_directory (fixture->directory);
  g_free (fixture->directory);
  g_free (fixture->running);
  g_free (fixture->candidate);
}

static void
wait_update (TerminalBuildMonitor *monitor, gboolean expected)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  terminal_build_monitor_refresh (monitor);
  while (terminal_build_monitor_has_update (monitor) != expected && g_get_monotonic_time () < deadline)
    process_events (10);
  g_assert_cmpint (terminal_build_monitor_has_update (monitor), ==, expected);
}

static void
update_availability (BuildFixture *fixture, gconstpointer data)
{
  start_monitor (fixture, fixture->candidate, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  g_assert_false (terminal_build_monitor_has_update (fixture->monitor));
  write_image (fixture->candidate, 0xbb);
  wait_stale (fixture->monitor, TRUE, 6);
  g_assert_false (terminal_build_monitor_has_update (fixture->monitor));
  guint changes = fixture->changes;
  g_assert_cmpint (g_chmod (fixture->candidate, 0700), ==, 0);
  wait_update (fixture->monitor, TRUE);
  g_assert_cmpuint (fixture->changes, >, changes);
  changes = fixture->changes;
  g_assert_cmpint (g_chmod (fixture->candidate, 0600), ==, 0);
  wait_update (fixture->monitor, FALSE);
  g_assert_true (terminal_build_monitor_is_stale (fixture->monitor));
  g_assert_cmpuint (fixture->changes, >, changes);
  g_assert_cmpint (g_chmod (fixture->candidate, 0700), ==, 0);
  wait_update (fixture->monitor, TRUE);
  g_assert_cmpint (g_unlink (fixture->candidate), ==, 0);
  wait_update (fixture->monitor, FALSE);
  g_assert_true (terminal_build_monitor_is_stale (fixture->monitor));
  write_image (fixture->candidate, 0xbb);
  g_assert_cmpint (g_chmod (fixture->candidate, 0700), ==, 0);
  wait_update (fixture->monitor, TRUE);
  g_assert_true (g_file_set_contents (fixture->candidate, "incomplete", -1, NULL));
  wait_update (fixture->monitor, FALSE);
  g_assert_true (terminal_build_monitor_is_stale (fixture->monitor));
  write_image (fixture->candidate, 0xaa);
  wait_stale (fixture->monitor, FALSE, 6);
  g_assert_false (terminal_build_monitor_has_update (fixture->monitor));
}

static void
identity_formats (BuildFixture *fixture,
                  gconstpointer data)
{
  guint8 bytes[320];

  for (guint is64 = 0; is64 <= 1; is64++)
    for (guint little = 0; little <= 1; little++)
      {
        gchar *id;
        make_image (bytes, is64, little, 0xab);
        g_assert_true (g_file_set_contents (fixture->candidate, (const gchar *) bytes, sizeof bytes, NULL));
        id = terminal_build_id_read (fixture->candidate);
        g_assert_cmpstr (id, ==, "abababababababab");
        g_free (id);
      }
  gchar *id = terminal_build_id_read ("/proc/self/exe");
  g_assert_nonnull (id);
  g_assert_cmpuint (strlen (id), >=, 16);
  g_free (id);
}

static void
identity_invalid (BuildFixture *fixture,
                  gconstpointer data)
{
  guint8 bytes[320];

  for (guint fault = 0; fault < 12; fault++)
    {
      gsize size = sizeof bytes;
      make_image (bytes, TRUE, TRUE, 0xaa);
      switch (fault)
        {
        case 0:
          bytes[0] = 0;
          break;
        case 1:
          bytes[4] = 3;
          break;
        case 2:
          bytes[5] = 0;
          break;
        case 3:
          bytes[16] = 1;
          break;
        case 4:
          put_integer (bytes + 56, 2, 65535, TRUE);
          break;
        case 5:
          memset (bytes + 32, 255, 8);
          break;
        case 6:
          put_integer (bytes + 196, 4, G_MAXUINT32, TRUE);
          break;
        case 7:
          bytes[204] = 'x';
          break;
        case 8:
          bytes[200] = 7;
          break;
        case 9:
          size--;
          break; /* Valid notes but an incomplete load segment. */
        case 10:
          bytes[20] = 0;
          break;
        case 11:
          bytes[52] = 0;
          break;
        }
      g_assert_true (g_file_set_contents (fixture->candidate, (const gchar *) bytes, size, NULL));
      g_assert_null (terminal_build_id_read (fixture->candidate));
    }
  g_assert_null (terminal_build_id_read (fixture->directory));
  g_assert_cmpint (g_unlink (fixture->candidate), ==, 0);
  g_assert_cmpint (mkfifo (fixture->candidate, 0600), ==, 0);
  g_assert_null (terminal_build_id_read (fixture->candidate));
}

static void
replacement (BuildFixture *fixture,
             gconstpointer data)
{
  start_monitor (fixture, fixture->candidate, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  write_image (fixture->candidate, 0xbb);
  wait_stale (fixture->monitor, TRUE, 6);
  g_assert_cmpuint (fixture->changes, ==, 1);
  write_image (fixture->candidate, 0xcc);
  process_events (1500);
  g_assert_true (terminal_build_monitor_is_stale (fixture->monitor));
  g_assert_cmpuint (fixture->changes, ==, 1);
  write_image (fixture->candidate, 0xaa);
  wait_stale (fixture->monitor, FALSE, 6);
  g_assert_cmpuint (fixture->changes, ==, 2);
}

static void
same_identity (BuildFixture *fixture,
               gconstpointer data)
{
  start_monitor (fixture, fixture->candidate, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  process_events (100);
  for (guint i = 0; i < 4; i++)
    {
      write_image (fixture->candidate, 0xaa);
      terminal_build_monitor_refresh (fixture->monitor);
    }
  process_events (1500);
  g_assert_false (terminal_build_monitor_is_stale (fixture->monitor));
  g_assert_cmpuint (fixture->changes, ==, 0);
}

static void
invalid_candidate (BuildFixture *fixture,
                   gconstpointer data)
{
  start_monitor (fixture, fixture->candidate, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  write_image (fixture->candidate, 0xbb);
  wait_stale (fixture->monitor, TRUE, 6);
  g_assert_cmpint (g_unlink (fixture->candidate), ==, 0);
  terminal_build_monitor_refresh (fixture->monitor);
  process_events (1500);
  g_assert_true (terminal_build_monitor_is_stale (fixture->monitor));
  g_assert_true (g_file_set_contents (fixture->candidate, "\177ELF", 4, NULL));
  process_events (1500);
  g_assert_true (terminal_build_monitor_is_stale (fixture->monitor));
  g_assert_cmpuint (fixture->changes, ==, 1);
  write_image (fixture->candidate, 0xaa);
  wait_stale (fixture->monitor, FALSE, 6);
}

static void
symlink_retarget (BuildFixture *fixture,
                  gconstpointer data)
{
  gchar *entry = g_build_filename (fixture->directory, "launch", NULL);
  gchar *temporary = g_strconcat (entry, ".new", NULL);

  g_assert_cmpint (symlink (fixture->running, entry), ==, 0);
  start_monitor (fixture, entry, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  process_events (100);
  write_image (fixture->candidate, 0xbb);
  g_assert_cmpint (symlink (fixture->candidate, temporary), ==, 0);
  g_assert_cmpint (g_rename (temporary, entry), ==, 0);
  wait_stale (fixture->monitor, TRUE, 6);
  g_assert_cmpint (g_unlink (entry), ==, 0);
  g_assert_cmpint (symlink (fixture->running, entry), ==, 0);
  wait_stale (fixture->monitor, FALSE, 6);
  g_free (entry);
  g_free (temporary);
}

static gchar *
make_repo (BuildFixture *fixture,
           const gchar *name,
           guint8 identity)
{
  gchar *directory = g_build_filename (fixture->directory, name, "build", "terminal", NULL);
  gchar *binary = g_build_filename (directory, "xfce4-terminal", NULL);
  gchar *wrapper = g_build_filename (fixture->directory, name, "run-patched-xfce4-terminal.sh", NULL);

  g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
  write_image (binary, identity);
  g_assert_true (g_file_set_contents (wrapper, "not executable; must never be invoked\n", -1, NULL));
  g_free (directory);
  g_free (binary);
  return wrapper;
}

static void
wrapper_retarget (BuildFixture *fixture,
                  gconstpointer data)
{
  gchar *a = make_repo (fixture, "a", 0xaa);
  gchar *b = make_repo (fixture, "b", 0xbb);
  gchar *entry = g_build_filename (fixture->directory, "launch", NULL);
  gchar *binary = g_build_filename (fixture->directory, "b", "build", "terminal", "xfce4-terminal", NULL);

  g_assert_cmpint (symlink (a, entry), ==, 0);
  start_monitor (fixture, entry, TERMINAL_BUILD_LAUNCH_REPO_WRAPPER);
  gchar *target = terminal_build_monitor_resolve_executable (fixture->monitor);
  gchar *expected = g_build_filename (fixture->directory, "a", "build", "terminal", "xfce4-terminal", NULL);
  g_assert_cmpstr (target, ==, expected);
  g_assert_cmpstr (terminal_build_monitor_get_launch_path (fixture->monitor), ==, entry);
  g_assert_cmpint (terminal_build_monitor_get_launch_kind (fixture->monitor), ==, TERMINAL_BUILD_LAUNCH_REPO_WRAPPER);
  g_free (target);
  g_free (expected);
  process_events (100);
  g_assert_cmpint (g_unlink (entry), ==, 0);
  g_assert_cmpint (symlink (b, entry), ==, 0);
  wait_stale (fixture->monitor, TRUE, 6);
  /* Re-arm monitors in the new repo: another rebuild must be detected there. */
  target = terminal_build_monitor_resolve_executable (fixture->monitor);
  g_assert_cmpstr (target, ==, binary);
  g_free (target);
  write_image (binary, 0xaa);
  wait_stale (fixture->monitor, FALSE, 6);
  g_free (a);
  g_free (b);
  g_free (entry);
  g_free (binary);
}

static void
fallback (BuildFixture *fixture,
          gconstpointer data)
{
  gchar *directory = g_build_filename (fixture->directory, "not-created-yet", NULL);
  gchar *entry = g_build_filename (directory, "binary", NULL);

  start_monitor (fixture, entry, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  process_events (100);
  g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
  write_image (entry, 0xbb);
  /* There was no parent directory to monitor. Exercise the real 30s timer. */
  wait_stale (fixture->monitor, TRUE, 35);
  write_image (entry, 0xaa);
  wait_stale (fixture->monitor, FALSE, 6);
  g_free (entry);
  g_free (directory);
}

static void
refresh_from_signal (TerminalBuildMonitor *monitor,
                     gpointer data)
{
  terminal_build_monitor_refresh (monitor);
}

static void
reentrant_refresh (BuildFixture *fixture,
                   gconstpointer data)
{
  start_monitor (fixture, fixture->candidate, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
  g_signal_connect (fixture->monitor, "changed", G_CALLBACK (refresh_from_signal), NULL);
  write_image (fixture->candidate, 0xbb);
  wait_stale (fixture->monitor, TRUE, 6);
  process_events (100);
  write_image (fixture->candidate, 0xaa);
  wait_stale (fixture->monitor, FALSE, 6);
  process_events (100);
  g_assert_cmpuint (fixture->changes, ==, 2);
}

static void
title_format (void)
{
  const gchar *struck = "T\314\266e\314\266r\314\266m\314\266i\314\266n\314\266a\314\266l\314\266";
  const gchar *titles[] = {
    "Terminal", "diet - Terminal", "Terminal - diet", "Xfce Terminal",
    "Codex - diet", "/home/lewis/Terminal", "Terminal.md", "MyTerminal",
    "Codex - C\314\266o\314\266d\314\266e\314\266x\314\266 - Terminal",
    "Terminal - /home/lewis/Terminal - Terminal"
  };

  for (guint i = 0; i < G_N_ELEMENTS (titles); i++)
    {
      gchar *expected;
      gchar *fresh = terminal_build_title_format (titles[i], "Terminal", FALSE);
      gchar *stale = terminal_build_title_format (titles[i], "Terminal", TRUE);
      gchar *twice = terminal_build_title_format (stale, "Terminal", TRUE);

      if (i == 0)
        expected = g_strdup (struck);
      else if (i == 1)
        expected = g_strconcat ("diet - ", struck, NULL);
      else if (i == 2)
        expected = g_strconcat (struck, " - diet", NULL);
      else if (i == 3)
        expected = g_strconcat ("Xfce ", struck, NULL);
      else if (i == 8)
        expected = g_strconcat ("Codex - C\314\266o\314\266d\314\266e\314\266x\314\266 - ", struck, NULL);
      else if (i == 9)
        expected = g_strconcat ("Terminal - /home/lewis/Terminal - ", struck, NULL);
      else
        expected = g_strconcat (titles[i], " - ", struck, NULL);
      g_assert_cmpstr (fresh, ==, titles[i]);
      g_assert_cmpstr (stale, ==, expected);
      g_assert_cmpstr (twice, ==, expected);
      g_assert_true (g_utf8_validate (stale, -1, NULL));
      g_free (fresh);
      g_free (stale);
      g_free (twice);
      g_free (expected);
    }
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/build/identity/formats", BuildFixture, NULL, setup, identity_formats, teardown);
  g_test_add ("/build/identity/invalid", BuildFixture, NULL, setup, identity_invalid, teardown);
  g_test_add ("/build/monitor/replacement-and-rollback", BuildFixture, NULL, setup, replacement, teardown);
  g_test_add ("/build/monitor/same-build-id", BuildFixture, NULL, setup, same_identity, teardown);
  g_test_add ("/build/monitor/invalid-candidate", BuildFixture, NULL, setup, invalid_candidate, teardown);
  g_test_add ("/build/monitor/update-availability", BuildFixture, NULL, setup, update_availability, teardown);
  g_test_add ("/build/monitor/symlink-retarget", BuildFixture, NULL, setup, symlink_retarget, teardown);
  g_test_add ("/build/monitor/wrapper-retarget-and-rebuild", BuildFixture, NULL, setup, wrapper_retarget, teardown);
  g_test_add ("/build/monitor/reentrant-refresh", BuildFixture, NULL, setup, reentrant_refresh, teardown);
  g_test_add ("/build/monitor/periodic-fallback", BuildFixture, NULL, setup, fallback, teardown);
  g_test_add_func ("/build/title/only-terminal-label", title_format);
  return g_test_run ();
}
