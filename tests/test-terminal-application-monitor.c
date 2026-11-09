#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "terminal/terminal-application-monitor.h"
#include "terminal/terminal-build-id.h"
#include "freshness-test-service.h"

typedef struct
{
  gchar *directory;
  gchar *program;
  gchar *wrapper;
  gchar *socket;
  GPid child;
  gint master;
  TerminalApplicationMonitor *monitor;
} Fixture;

static const gchar *struck_codex = "c\314\266o\314\266d\314\266e\314\266x\314\266";

static void
iterate (guint milliseconds)
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
copy_image (const gchar *source, const gchar *target)
{
  gchar *contents, *temporary = g_strconcat (target, ".new", NULL);
  gsize size;
  g_assert_true (g_file_get_contents (source, &contents, &size, NULL));
  g_assert_true (g_file_set_contents (temporary, contents, size, NULL));
  g_assert_cmpint (g_chmod (temporary, 0700), ==, 0);
  g_assert_cmpint (g_rename (temporary, target), ==, 0);
  g_free (contents);
  g_free (temporary);
}

static void
setup (Fixture *fixture, gconstpointer data)
{
  fixture->master = -1;
  fixture->directory = g_dir_make_tmp ("terminal-application-XXXXXX", NULL);
  g_assert_nonnull (fixture->directory);
  fixture->program = g_build_filename (fixture->directory, "codex", NULL);
  fixture->wrapper = g_build_filename (fixture->directory, "node", NULL);
  fixture->socket = g_build_filename (fixture->directory, "tmux.sock", NULL);
  copy_image (APPLICATION_FIXTURE, fixture->program);
  fixture->monitor = terminal_application_monitor_new ();
}

static void
start (Fixture *fixture, const gchar *const *arguments)
{
  gint slave;
  fixture->master = posix_openpt (O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  g_assert_cmpint (fixture->master, >=, 0);
  g_assert_cmpint (grantpt (fixture->master), ==, 0);
  g_assert_cmpint (unlockpt (fixture->master), ==, 0);
  slave = open (ptsname (fixture->master), O_RDWR | O_NOCTTY | O_CLOEXEC);
  g_assert_cmpint (slave, >=, 0);
  fixture->child = fork ();
  g_assert_cmpint (fixture->child, >=, 0);
  if (fixture->child == 0)
    {
      if (setsid () < 0 || ioctl (slave, TIOCSCTTY, 0) < 0)
        _exit (126);
      for (gint fd = 0; fd <= 2; fd++)
        if (dup2 (slave, fd) < 0)
          _exit (126);
      close (slave);
      close (fixture->master);
      execv (arguments[0], (gchar *const *) arguments);
      _exit (127);
    }
  close (slave);
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  gchar bytes[1024];
  while (read (fixture->master, bytes, sizeof bytes) <= 0 && g_get_monotonic_time () < deadline)
    iterate (10);
  g_assert_cmpint (tcgetpgrp (fixture->master), >, 1);
  terminal_application_monitor_set_target (fixture->monitor, fixture->master, fixture->child);
}

static void
wait_title (Fixture *fixture, const gchar *input, const gchar *expected)
{
  gint64 deadline = g_get_monotonic_time () + 7 * G_TIME_SPAN_SECOND;
  gchar *title;
  do
    {
      terminal_application_monitor_refresh (fixture->monitor);
      iterate (50);
      title = terminal_application_monitor_format_title (fixture->monitor, input);
      if (g_str_equal (title, expected) || g_get_monotonic_time () >= deadline)
        break;
      g_free (title);
    }
  while (TRUE);
  g_assert_cmpstr (title, ==, expected);
  g_free (title);
}

static void
wait_label (Fixture *fixture, const gchar *label)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  do
    {
      terminal_application_monitor_refresh (fixture->monitor);
      iterate (50);
    }
  while (g_strcmp0 (terminal_application_monitor_get_label (fixture->monitor), label) != 0
         && g_get_monotonic_time () < deadline);
  g_assert_cmpstr (terminal_application_monitor_get_label (fixture->monitor), ==, label);
}

static void
tmux_command (Fixture *fixture, const gchar *const *command)
{
  GPtrArray *arguments = g_ptr_array_new ();
  gchar *binary = g_find_program_in_path ("tmux");
  gchar *output = NULL, *error_output = NULL;
  gint status;
  g_assert_nonnull (binary);
  g_ptr_array_add (arguments, binary);
  g_ptr_array_add (arguments, "-N");
  g_ptr_array_add (arguments, "-S");
  g_ptr_array_add (arguments, fixture->socket);
  for (guint i = 0; command[i] != NULL; i++)
    g_ptr_array_add (arguments, (gpointer) command[i]);
  g_ptr_array_add (arguments, NULL);
  g_assert_true (g_spawn_sync (NULL, (gchar **) arguments->pdata, NULL, 0, NULL, NULL,
                               &output, &error_output, &status, NULL));
  if (status != 0)
    g_test_message ("tmux: %s", error_output);
  g_assert_cmpint (status, ==, 0);
  g_free (output);
  g_free (error_output);
  g_free (binary);
  g_ptr_array_unref (arguments);
}

static void
teardown (Fixture *fixture, gconstpointer data)
{
  terminal_application_monitor_set_target (fixture->monitor, -1, -1);
  g_object_unref (fixture->monitor);
  if (g_file_test (fixture->socket, G_FILE_TEST_EXISTS))
    {
      const gchar *command[] = { "kill-server", NULL };
      tmux_command (fixture, command);
    }
  if (fixture->child > 1)
    {
      kill (-fixture->child, SIGKILL);
      kill (fixture->child, SIGKILL);
      while (waitpid (fixture->child, NULL, 0) < 0 && errno == EINTR)
        ;
    }
  if (fixture->master >= 0)
    close (fixture->master);
  iterate (50);
  GDir *directory = g_dir_open (fixture->directory, 0, NULL);
  const gchar *name;
  while ((name = g_dir_read_name (directory)) != NULL)
    {
      gchar *path = g_build_filename (fixture->directory, name, NULL);
      g_assert_cmpint (g_unlink (path), ==, 0);
      g_free (path);
    }
  g_dir_close (directory);
  g_assert_cmpint (g_rmdir (fixture->directory), ==, 0);
  g_free (fixture->directory);
  g_free (fixture->program);
  g_free (fixture->wrapper);
  g_free (fixture->socket);
}

static void
replacement (Fixture *fixture, gconstpointer data)
{
  const gchar *arguments[] = { fixture->program, "--hold", NULL };
  gchar *legacy = g_strconcat (struck_codex, " - project - Terminal", NULL);
  const gchar *fresh = "codex - project - Terminal";
  start (fixture, arguments);
  wait_label (fixture, "codex");
  wait_title (fixture, legacy, fresh); /* Initial fresh check also removes a legacy marker. */
  copy_image (APPLICATION_FIXTURE_NEXT, fixture->program);
  wait_title (fixture, fresh, legacy);
  g_assert_true (terminal_application_monitor_is_stale (fixture->monitor));
  if (data == NULL)
    {
      copy_image (APPLICATION_FIXTURE, fixture->program);
      wait_title (fixture, fresh, fresh);
      g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
    }
  else
    {
      g_assert_cmpint (write (fixture->master, "e", 1), ==, 1);
      wait_title (fixture, legacy, fresh); /* exec keeps PID/start time but changes image inode. */
      g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
    }
  g_free (legacy);
}

static void
same_build (Fixture *fixture, gconstpointer data)
{
  const gchar *arguments[] = { fixture->program, "--hold", NULL };
  start (fixture, arguments);
  wait_label (fixture, "codex");
  copy_image (APPLICATION_FIXTURE, fixture->program);
  iterate (1400);
  g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
}

static void
wrapped (Fixture *fixture, gconstpointer data)
{
  const gchar *mode = data == NULL ? "--wrap" : data == GINT_TO_POINTER (2) ? "--mixed"
                                                                            : "--ambiguous";
  const gchar *arguments[] = { fixture->wrapper, mode, fixture->program, fixture->wrapper, NULL };
  copy_image (APPLICATION_FIXTURE, fixture->wrapper);
  start (fixture, arguments);
  if (data != NULL)
    {
      iterate (100);
      wait_label (fixture, NULL);
      g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
    }
  else
    {
      wait_label (fixture, "codex");
      copy_image (APPLICATION_FIXTURE_NEXT, fixture->program);
      gchar *expected = g_strconcat (struck_codex, " - Terminal", NULL);
      wait_title (fixture, "codex - Terminal", expected);
      g_free (expected);
    }
}

static void
unsupported (Fixture *fixture, gconstpointer data)
{
  const gchar *name = data;
  gchar *path = g_build_filename (fixture->directory, name, NULL);
  const gchar *arguments[] = { path, "--hold", NULL };
  copy_image (APPLICATION_FIXTURE, path);
  start (fixture, arguments);
  iterate (100);
  wait_label (fixture, NULL);
  copy_image (APPLICATION_FIXTURE_NEXT, path);
  iterate (100);
  wait_label (fixture, NULL);
  g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
  g_free (path);
}

static void
invalid_candidate (Fixture *fixture, gconstpointer data)
{
  const gchar *arguments[] = { fixture->program, "--hold", NULL };
  gchar *legacy = g_strconcat (struck_codex, " - Terminal", NULL);
  gchar *sentinel = g_build_filename (fixture->directory, "must-not-run", NULL);
  gchar *script = g_strdup_printf ("#!/bin/sh\ntouch '%s'\n", sentinel);
  gchar *temporary = g_strconcat (fixture->program, ".new", NULL);
  start (fixture, arguments);
  wait_label (fixture, "codex");
  wait_title (fixture, legacy, "codex - Terminal");
  g_assert_true (g_file_set_contents (temporary, script, -1, NULL));
  g_assert_cmpint (g_chmod (temporary, 0700), ==, 0);
  g_assert_cmpint (g_rename (temporary, fixture->program), ==, 0);
  iterate (1300);
  g_assert_false (g_file_test (sentinel, G_FILE_TEST_EXISTS));
  g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
  copy_image (APPLICATION_FIXTURE_NEXT, fixture->program);
  wait_title (fixture, "codex - Terminal", legacy);
  g_assert_false (g_file_test (sentinel, G_FILE_TEST_EXISTS));
  g_free (legacy);
  g_free (sentinel);
  g_free (script);
  g_free (temporary);
}

static void
retarget (Fixture *fixture, gconstpointer data)
{
  gchar *one = g_build_filename (fixture->directory, "one", NULL);
  gchar *two = g_build_filename (fixture->directory, "two", NULL);
  gchar *entry = g_build_filename (fixture->directory, "launch", NULL);
  const gchar *arguments[] = { entry, "--hold", NULL };
  copy_image (APPLICATION_FIXTURE, one);
  copy_image (APPLICATION_FIXTURE_NEXT, two);
  g_assert_cmpint (symlink (one, entry), ==, 0);
  start (fixture, arguments);
  wait_label (fixture, "one");
  g_assert_cmpint (g_unlink (entry), ==, 0);
  g_assert_cmpint (symlink (two, entry), ==, 0);
  wait_title (fixture, "project - Terminal", "o\314\266n\314\266e\314\266 - project - Terminal");
  g_free (one);
  g_free (two);
  g_free (entry);
}

static void
target_changed (Fixture *fixture, gconstpointer data)
{
  const gchar *arguments[] = { fixture->program, "--hold", NULL };
  start (fixture, arguments);
  terminal_application_monitor_set_target (fixture->monitor, -1, -1);
  iterate (100);
  g_assert_null (terminal_application_monitor_get_label (fixture->monitor));
  copy_image (APPLICATION_FIXTURE_NEXT, fixture->program);
  iterate (1200);
  g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
  terminal_application_monitor_set_target (fixture->monitor, fixture->master, fixture->child);
  wait_label (fixture, "codex");
  gchar *expected = g_strconcat (struck_codex, " - Terminal", NULL);
  wait_title (fixture, "codex - Terminal", expected);
  g_free (expected);
}

static void
through_tmux (Fixture *fixture, gconstpointer data)
{
  gchar *tmux = g_find_program_in_path ("tmux");
  g_assert_nonnull (tmux);
  const gchar *arguments[] = { tmux, "-S", fixture->socket, "-f", "/dev/null",
                               "new-session", "-s", "test", fixture->program, "--hold", NULL };
  start (fixture, arguments);
  wait_label (fixture, "codex");
  copy_image (APPLICATION_FIXTURE_NEXT, fixture->program);
  gchar *expected = g_strconcat (struck_codex, " - Terminal", NULL);
  wait_title (fixture, "codex - Terminal", expected);
  copy_image (APPLICATION_FIXTURE, fixture->wrapper);
  const gchar *new_window[] = { "new-window", "-t", "test:", "-n", "interpreter", fixture->wrapper, "--hold", NULL };
  tmux_command (fixture, new_window);
  wait_label (fixture, NULL);
  g_assert_false (terminal_application_monitor_is_stale (fixture->monitor));
  const gchar *select_window[] = { "select-window", "-t", "test:0", NULL };
  tmux_command (fixture, select_window);
  wait_label (fixture, "codex");
  wait_title (fixture, "codex - Terminal", expected);
  g_free (expected);
  g_free (tmux);
}

static void
title_format (void)
{
  const gchar *input[] = { "codex", "Codex - project - Terminal", "notes - codex - Terminal",
                           "/home/codex - Terminal", "codex.md - Terminal", "Advice about codex - Terminal" };
  for (guint i = 0; i < G_N_ELEMENTS (input); i++)
    {
      gchar *stale = terminal_application_title_format (input[i], "codex", TRUE);
      gchar *twice = terminal_application_title_format (stale, "codex", TRUE);
      gchar *fresh = terminal_application_title_format (stale, "codex", FALSE);
      g_assert_cmpstr (twice, ==, stale);
      g_assert_true (g_utf8_validate (stale, -1, NULL));
      if (i < 3)
        g_assert_cmpstr (fresh, ==, input[i]);
      else
        {
          gchar *expected = g_strconcat (struck_codex, " - ", input[i], NULL);
          g_assert_cmpstr (stale, ==, expected);
          g_free (expected);
        }
      g_free (stale);
      g_free (twice);
      g_free (fresh);
    }
  gchar *both = terminal_application_title_format ("codex - T\314\266e\314\266r\314\266m\314\266i\314\266n\314\266a\314\266l\314\266", "codex", TRUE);
  gchar *expected = g_strconcat (struck_codex, " - T\314\266e\314\266r\314\266m\314\266i\314\266n\314\266a\314\266l\314\266", NULL);
  g_assert_cmpstr (both, ==, expected);
  g_free (both);
  g_free (expected);
}

static void
title_with_directory (void)
{
  const gchar *input[] = { "fish ~", "fish ~ - Terminal", "fish ~/repos/project - Terminal",
                           "fish /tmp/project - Terminal", "fish ~/directory with spaces - Terminal",
                           "notes - fish ~/repos/project - Terminal" };
  const gchar *expected[] = { "f\314\266i\314\266s\314\266h\314\266 ~",
                              "f\314\266i\314\266s\314\266h\314\266 ~ - Terminal",
                              "f\314\266i\314\266s\314\266h\314\266 ~/repos/project - Terminal",
                              "f\314\266i\314\266s\314\266h\314\266 /tmp/project - Terminal",
                              "f\314\266i\314\266s\314\266h\314\266 ~/directory with spaces - Terminal",
                              "notes - f\314\266i\314\266s\314\266h\314\266 ~/repos/project - Terminal" };

  for (guint i = 0; i < G_N_ELEMENTS (input); i++)
    {
      gchar *stale = terminal_application_title_format (input[i], "fish", TRUE);
      gchar *twice = terminal_application_title_format (stale, "fish", TRUE);
      gchar *fresh = terminal_application_title_format (stale, "fish", FALSE);
      g_assert_cmpstr (stale, ==, expected[i]);
      g_assert_cmpstr (twice, ==, expected[i]);
      g_assert_cmpstr (fresh, ==, input[i]);
      g_free (stale);
      g_free (twice);
      g_free (fresh);
    }
}

int
main (int argc, char **argv)
{
  if (g_getenv ("TERMINAL_APPLICATION_TEST_BUS") == NULL)
    {
      GPtrArray *command = g_ptr_array_new ();
      g_ptr_array_add (command, "dbus-run-session");
      g_ptr_array_add (command, "--");
      for (gint i = 0; i < argc; i++) g_ptr_array_add (command, argv[i]);
      g_ptr_array_add (command, NULL);
      GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
      g_subprocess_launcher_setenv (launcher, "TERMINAL_APPLICATION_TEST_BUS", "1", TRUE);
      GError *error = NULL;
      GSubprocess *test = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) command->pdata, &error);
      g_assert_no_error (error);
      g_assert_true (g_subprocess_wait (test, NULL, &error));
      g_assert_no_error (error);
      gint status = g_subprocess_get_if_exited (test) ? g_subprocess_get_exit_status (test) : 1;
      g_object_unref (test);
      g_object_unref (launcher);
      g_ptr_array_unref (command);
      return status;
    }
  g_test_init (&argc, &argv, NULL);
  g_setenv ("TERM", "xterm-256color", TRUE);
  g_test_add_func ("/application/title-format", title_format);
  g_test_add_func ("/application/title-with-directory", title_with_directory);
  g_test_add ("/application/replacement-and-rollback", Fixture, NULL, setup, replacement, teardown);
  g_test_add ("/application/exec-in-same-pid", Fixture, GINT_TO_POINTER (1), setup, replacement, teardown);
  g_test_add ("/application/identical-build-new-inode", Fixture, NULL, setup, same_build, teardown);
  g_test_add ("/application/native-child-of-runtime", Fixture, NULL, setup, wrapped, teardown);
  g_test_add ("/application/ambiguous-runtime-children", Fixture, GINT_TO_POINTER (1), setup, wrapped, teardown);
  g_test_add ("/application/native-plus-interpreted-ambiguous", Fixture, GINT_TO_POINTER (2), setup, wrapped, teardown);
  g_test_add ("/application/interpreted-unknown", Fixture, "node", setup, unsupported, teardown);
  g_test_add ("/application/remote-unknown", Fixture, "ssh", setup, unsupported, teardown);
  g_test_add ("/application/invalid-candidate-never-executed", Fixture, NULL, setup, invalid_candidate, teardown);
  g_test_add ("/application/launch-symlink-retarget", Fixture, NULL, setup, retarget, teardown);
  g_test_add ("/application/stale-task-after-tab-switch", Fixture, NULL, setup, target_changed, teardown);
  g_test_add ("/application/tmux-active-pane", Fixture, NULL, setup, through_tmux, teardown);
  GSubprocess *service = freshness_test_service_start ();
  gint result = g_test_run ();
  freshness_test_service_stop (service);
  return result;
}
