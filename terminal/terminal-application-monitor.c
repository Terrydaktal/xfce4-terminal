#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "terminal-application-monitor.h"
#include "terminal-build-id.h"
#include "terminal-build-monitor.h"
#include "terminal-tmux.h"

typedef struct
{
  GPid pid;
  gint group;
  guint64 start;
  gchar *executable;
  gchar *label;
  gchar *launch_path;
  gchar *build_id;
  struct stat image;
} Application;

typedef struct
{
  gint fd;
  guint generation;
  Application *previous;
} ApplicationCheck;

struct _TerminalApplicationMonitor
{
  GObject parent_instance;
  gint original_fd;
  gint fd;
  GPid child_pid;
  guint generation;
  gboolean checking;
  gboolean check_again;
  Application *application;
  TerminalBuildMonitor *build;
};

G_DEFINE_TYPE (TerminalApplicationMonitor, terminal_application_monitor, G_TYPE_OBJECT)

static void
application_free (gpointer data)
{
  Application *application = data;
  if (application == NULL)
    return;
  g_free (application->executable);
  g_free (application->label);
  g_free (application->launch_path);
  g_free (application->build_id);
  g_free (application);
}

static void
check_free (gpointer data)
{
  ApplicationCheck *check = data;
  close (check->fd);
  application_free (check->previous);
  g_free (check);
}

/* Proc files can be large or disappear midway through a read. Never use an
 * unbounded read, and never inspect another user's command/environment data. */
static gchar *
read_proc (GPid pid,
           const gchar *entry,
           gsize limit,
           gsize *length)
{
  gchar *path = g_strdup_printf ("/proc/%d/%s", (gint) pid, entry);
  gchar *bytes = NULL;
  struct stat info;
  gint fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
  gsize used = 0;

  g_free (path);
  if (fd < 0)
    return NULL;
  if (fstat (fd, &info) != 0 || info.st_uid != getuid () || !S_ISREG (info.st_mode))
    goto out;
  bytes = g_malloc (limit + 1);
  while (used < limit)
    {
      ssize_t count = read (fd, bytes + used, limit - used);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        goto invalid;
      if (count == 0)
        {
          bytes[used] = '\0';
          if (length != NULL)
            *length = used;
          goto out;
        }
      used += count;
    }
invalid:
  g_clear_pointer (&bytes, g_free);
out:
  close (fd);
  return bytes;
}

static Application *
read_application (GPid pid)
{
  gchar *contents = read_proc (pid, "stat", 4096, NULL);
  gchar *end = contents != NULL ? strrchr (contents, ')') : NULL;
  gchar **fields = NULL;
  gchar *path = NULL;
  Application *application = NULL;

  if (end == NULL || end[1] != ' ')
    goto out;
  fields = g_strsplit (end + 2, " ", 0);
  if (g_strv_length (fields) < 20 || fields[0][0] == 'Z' || fields[0][0] == 'X')
    goto out;
  application = g_new0 (Application, 1);
  application->pid = pid;
  application->group = (gint) g_ascii_strtoll (fields[2], NULL, 10);
  application->start = g_ascii_strtoull (fields[19], NULL, 10);
  path = g_strdup_printf ("/proc/%d/exe", (gint) pid);
  application->executable = g_file_read_link (path, NULL);
  if (application->executable == NULL || application->start == 0
      || g_stat (path, &application->image) != 0)
    goto invalid;
  if (g_str_has_suffix (application->executable, " (deleted)"))
    application->executable[strlen (application->executable) - strlen (" (deleted)")] = '\0';
  application->label = g_path_get_basename (application->executable);
  if (!g_path_is_absolute (application->executable) || !g_utf8_validate (application->label, -1, NULL))
    goto invalid;
  for (const gchar *p = application->label; *p != '\0'; p = g_utf8_next_char (p))
    if (g_unichar_iscntrl (g_utf8_get_char (p)))
      goto invalid;
  goto out;
invalid:
  g_clear_pointer (&application, application_free);
out:
  g_strfreev (fields);
  g_free (contents);
  g_free (path);
  return application;
}

static gboolean
same_application (const Application *a,
                  const Application *b)
{
  if (a == NULL || b == NULL)
    return a == b;
  return a->pid == b->pid && a->start == b->start
         && a->image.st_dev == b->image.st_dev && a->image.st_ino == b->image.st_ino
         && g_str_equal (a->executable, b->executable);
}

static gboolean
is_shell (const gchar *name)
{
  const gchar *shells[] = { "fish", "bash", "zsh", "sh", "dash", "ksh", NULL };
  return g_strv_contains (shells, name);
}

static gboolean
is_runtime (const gchar *name)
{
  const gchar *runtimes[] = { "node", "nodejs", "deno", "bun", "python", "python3", "ruby",
                              "perl", "php", "java", "dotnet", "lua", "env", NULL };
  return g_strv_contains (runtimes, name) || g_str_has_prefix (name, "python3.");
}

static gboolean
is_remote (const gchar *name)
{
  const gchar *names[] = { "ssh", "mosh", "mosh-client", "rlogin", "telnet", "sudo", "su", NULL };
  return g_strv_contains (names, name);
}

static gboolean
interactive_shell (Application *application)
{
  gsize size = 0;
  gchar *arguments = read_proc (application->pid, "cmdline", 65536, &size);
  gboolean interactive = arguments != NULL;

  for (gsize offset = arguments != NULL ? strlen (arguments) + 1 : 0; offset < size && interactive;)
    {
      const gchar *argument = arguments + offset;
      /* Only a known interactive/login invocation is a shell, not a script. */
      interactive = g_str_equal (argument, "-l") || g_str_equal (argument, "--login")
                    || g_str_equal (argument, "-i") || g_str_equal (argument, "--interactive");
      offset += strlen (argument) + 1;
    }
  g_free (arguments);
  return interactive;
}

static GArray *
process_children (GPid pid)
{
  gchar *path = g_strdup_printf ("/proc/%d/task", (gint) pid);
  GDir *tasks = g_dir_open (path, 0, NULL);
  GArray *children = g_array_new (FALSE, FALSE, sizeof (GPid));
  const gchar *task;
  guint count = 0;
  gboolean valid = tasks != NULL;

  g_free (path);
  while (valid && (task = g_dir_read_name (tasks)) != NULL)
    {
      gchar *entry = g_strdup_printf ("task/%s/children", task);
      gchar *contents = read_proc (pid, entry, 16384, NULL);
      gchar *position = contents;
      g_free (entry);
      if (++count > 256 || contents == NULL)
        valid = FALSE;
      while (valid && *position != '\0')
        {
          gchar *end;
          gint64 value = g_ascii_strtoll (position, &end, 10);
          if (value <= 1 || value > G_MAXINT || end == position || children->len >= 128)
            {
              valid = FALSE;
              break;
            }
          GPid child = (GPid) value;
          g_array_append_val (children, child);
          position = end;
          while (g_ascii_isspace (*position))
            position++;
        }
      g_free (contents);
    }
  if (tasks != NULL)
    g_dir_close (tasks);
  if (!valid)
    g_clear_pointer (&children, g_array_unref);
  return children;
}

static Application *
resolve_application (GPid pid,
                     gint group,
                     guint depth,
                     guint *remaining)
{
  Application *application, *selected = NULL;
  GArray *children;
  gboolean has_children;

  if (pid <= 1 || depth > 8 || *remaining == 0)
    return NULL;
  (*remaining)--;
  application = read_application (pid);
  if (application == NULL)
    return NULL;
  if (application->group != group || is_remote (application->label))
    goto out;
  if (g_str_equal (application->label, "tmux"))
    {
      gint foreground = terminal_tmux_pane_foreground_pid (pid);
      selected = resolve_application (foreground, foreground, depth + 1, remaining);
      goto out;
    }
  if (!is_shell (application->label) && !is_runtime (application->label))
    return application;

  children = process_children (pid);
  if (children == NULL)
    goto out;
  has_children = FALSE;
  for (guint i = 0; i < children->len; i++)
    {
      GPid child = g_array_index (children, GPid, i);
      Application *peer = read_application (child);
      if (peer != NULL && peer->group != group)
        {
          application_free (peer);
          continue;
        }
      has_children = TRUE;
      Application *candidate = peer != NULL ? resolve_application (child, group, depth + 1, remaining) : NULL;
      application_free (peer);
      if (candidate == NULL || selected != NULL)
        {
          application_free (candidate);
          g_clear_pointer (&selected, application_free);
          g_array_unref (children);
          goto out; /* A pipeline or unresolved peer is not one known application. */
        }
      selected = candidate;
    }
  g_array_unref (children);
  if (selected == NULL && !has_children && is_shell (application->label) && interactive_shell (application))
    {
      selected = application;
      application = NULL;
    }
out:
  application_free (application);
  return selected;
}

static gboolean
matches_executable (const gchar *candidate,
                    Application *application)
{
  struct stat info;
  gchar *resolved = realpath (candidate, NULL);
  gboolean matches = g_strcmp0 (resolved, application->executable) == 0
                     || (g_stat (candidate, &info) == 0
                         && info.st_dev == application->image.st_dev && info.st_ino == application->image.st_ino);
  g_free (resolved);
  return matches;
}

static gchar *
launch_path (Application *application)
{
  gchar *arguments = read_proc (application->pid, "cmdline", 65536, NULL);
  gchar *result = g_strdup (application->executable);

  if (arguments != NULL && arguments[0] != '\0')
    {
      gchar *candidate = NULL;
      if (g_path_is_absolute (arguments))
        {
          gchar *name = g_path_get_basename (arguments);
          /* An absolute native argv[0] retains a launch symlink across rebuilds.
           * An arbitrary exec -a alias must not redirect the version check. */
          if (matches_executable (arguments, application) || g_str_equal (name, application->label))
            candidate = g_strdup (arguments);
          g_free (name);
        }
      else
        {
          gchar *cwd_path = g_strdup_printf ("/proc/%d/cwd", (gint) application->pid);
          gchar *cwd = g_file_read_link (cwd_path, NULL);
          if (cwd != NULL && strchr (arguments, '/') != NULL)
            {
              gchar *relative = g_canonicalize_filename (arguments, cwd);
              if (matches_executable (relative, application))
                candidate = g_strdup (relative);
              g_free (relative);
            }
          else if (cwd != NULL)
            {
              gsize size;
              gchar *environment = read_proc (application->pid, "environ", 131072, &size);
              for (gsize offset = 0; environment != NULL && offset < size;)
                {
                  const gchar *entry = environment + offset;
                  if (g_str_has_prefix (entry, "PATH="))
                    {
                      gchar **paths = g_strsplit (entry + 5, ":", 129);
                      for (guint i = 0; paths[i] != NULL && i < 128; i++)
                        {
                          gchar *directory = g_canonicalize_filename (paths[i], cwd);
                          gchar *path = g_build_filename (directory, arguments, NULL);
                          g_free (directory);
                          if (matches_executable (path, application))
                            candidate = g_strdup (path);
                          g_free (path);
                          if (candidate != NULL)
                            break;
                        }
                      g_strfreev (paths);
                      break;
                    }
                  offset += strlen (entry) + 1;
                }
              g_free (environment);
            }
          g_free (cwd);
          g_free (cwd_path);
        }
      if (candidate != NULL)
        {
          g_free (result);
          result = candidate;
        }
    }
  g_free (arguments);
  return result;
}

static void
check_worker (GTask *task,
              gpointer source_object,
              gpointer task_data,
              GCancellable *cancellable)
{
  ApplicationCheck *check = task_data;
  Application *application = NULL;
#ifdef __linux__
  gint group = tcgetpgrp (check->fd);
  guint remaining = 128;

  application = resolve_application (group, group, 0, &remaining);
  if (application != NULL)
    {
      gchar *path = g_strdup_printf ("/proc/%d/exe", (gint) application->pid);
      Application *after;
      if (same_application (application, check->previous))
        {
          application->build_id = g_strdup (check->previous->build_id);
          application->launch_path = g_strdup (check->previous->launch_path);
        }
      else
        {
          application->build_id = terminal_build_id_read (path);
          application->launch_path = launch_path (application);
        }
      g_free (path);
      after = read_application (application->pid);
      if (!same_application (application, after) || application->build_id == NULL
          || tcgetpgrp (check->fd) != group)
        g_clear_pointer (&application, application_free);
      application_free (after);
    }
#endif
  g_task_return_pointer (task, application, application_free);
}

static void
build_checked (TerminalBuildMonitor *build,
               TerminalApplicationMonitor *monitor)
{
  g_signal_emit_by_name (monitor, "changed");
}

static void
clear_application (TerminalApplicationMonitor *monitor)
{
  if (monitor->build != NULL)
    g_signal_handlers_disconnect_by_data (monitor->build, monitor);
  g_clear_object (&monitor->build);
  g_clear_pointer (&monitor->application, application_free);
}

static void
check_finished (GObject *object,
                GAsyncResult *result,
                gpointer data)
{
  TerminalApplicationMonitor *monitor = TERMINAL_APPLICATION_MONITOR (object);
  ApplicationCheck *check = g_task_get_task_data (G_TASK (result));
  Application *application = g_task_propagate_pointer (G_TASK (result), NULL);

  monitor->checking = FALSE;
  if (check->generation == monitor->generation)
    {
      if (!same_application (application, monitor->application))
        {
          clear_application (monitor);
          monitor->application = application;
          application = NULL;
          if (monitor->application != NULL)
            {
              monitor->build = terminal_build_monitor_new_for_id (monitor->application->build_id,
                                                                  monitor->application->launch_path);
              g_signal_connect (monitor->build, "checked", G_CALLBACK (build_checked), monitor);
            }
          g_signal_emit_by_name (monitor, "changed");
        }
    }
  application_free (application);
  if (monitor->check_again)
    {
      monitor->check_again = FALSE;
      terminal_application_monitor_refresh (monitor);
    }
}

void
terminal_application_monitor_refresh (TerminalApplicationMonitor *monitor)
{
  ApplicationCheck *check;
  GTask *task;
  if (monitor->fd < 0 || monitor->child_pid <= 1)
    return;
  if (monitor->checking)
    {
      monitor->check_again = TRUE;
      return;
    }
  check = g_new0 (ApplicationCheck, 1);
  check->fd = fcntl (monitor->fd, F_DUPFD_CLOEXEC, 3);
  if (check->fd < 0)
    {
      g_free (check);
      return;
    }
  check->generation = monitor->generation;
  if (monitor->application != NULL)
    {
      check->previous = g_new (Application, 1);
      *check->previous = *monitor->application;
      check->previous->executable = g_strdup (monitor->application->executable);
      check->previous->label = g_strdup (monitor->application->label);
      check->previous->launch_path = g_strdup (monitor->application->launch_path);
      check->previous->build_id = g_strdup (monitor->application->build_id);
    }
  monitor->checking = TRUE;
  task = g_task_new (monitor, NULL, check_finished, NULL);
  g_task_set_task_data (task, check, check_free);
  g_task_run_in_thread (task, check_worker);
  g_object_unref (task);
}

void
terminal_application_monitor_set_target (TerminalApplicationMonitor *monitor,
                                         gint pty_fd,
                                         GPid child_pid)
{
  if (monitor->original_fd == pty_fd && monitor->child_pid == child_pid)
    return;
  monitor->generation++;
  if (monitor->fd >= 0)
    close (monitor->fd);
  monitor->original_fd = pty_fd;
  monitor->fd = pty_fd >= 0 ? fcntl (pty_fd, F_DUPFD_CLOEXEC, 3) : -1;
  monitor->child_pid = child_pid;
  clear_application (monitor);
  g_signal_emit_by_name (monitor, "changed");
  terminal_application_monitor_refresh (monitor);
}

const gchar *
terminal_application_monitor_get_label (TerminalApplicationMonitor *monitor)
{
  return monitor->application != NULL ? monitor->application->label : NULL;
}

gboolean
terminal_application_monitor_is_stale (TerminalApplicationMonitor *monitor)
{
  return monitor->build != NULL && terminal_build_monitor_is_stale (monitor->build);
}

/* Match labels only at the beginning or as an explicit " - label" component,
 * never an incidental word inside a path or conversation title. Accept a legacy
 * application-owned strike without adding a second combining character. */
gchar *
terminal_application_title_format (const gchar *title,
                                   const gchar *label,
                                   gboolean stale)
{
  GString *result = g_string_new (NULL);
  gboolean found = FALSE;
  if (label == NULL || label[0] == '\0' || !g_utf8_validate (label, -1, NULL)
      || !g_utf8_validate (title, -1, NULL))
    {
      g_string_free (result, TRUE);
      return g_strdup (title);
    }
  for (const gchar *position = title; *position != '\0';)
    {
      const gchar *cursor = position;
      const gchar *letter = label;
      gboolean boundary = position == title || (position - title >= 3 && strncmp (position - 3, " - ", 3) == 0);
      while (boundary && *letter != '\0' && *cursor != '\0'
             && g_unichar_tolower (g_utf8_get_char (letter)) == g_unichar_tolower (g_utf8_get_char (cursor)))
        {
          letter = g_utf8_next_char (letter);
          cursor = g_utf8_next_char (cursor);
          if (g_str_has_prefix (cursor, "\314\266"))
            cursor += 2;
        }
      if (boundary && *letter == '\0' && (*cursor == '\0' || g_str_has_prefix (cursor, " - ") || g_str_has_prefix (cursor, ": ")))
        {
          while (position < cursor)
            {
              gunichar character = g_utf8_get_char (position);
              if (character != 0x336)
                {
                  g_string_append_unichar (result, character);
                  if (stale)
                    g_string_append (result, "\314\266");
                }
              position = g_utf8_next_char (position);
            }
          found = TRUE;
        }
      else
        {
          g_string_append_len (result, position, g_utf8_next_char (position) - position);
          position = g_utf8_next_char (position);
        }
    }
  if (stale && !found)
    {
      gchar *struck = terminal_build_title_format (label, label, TRUE);
      g_string_prepend (result, " - ");
      g_string_prepend (result, struck);
      g_free (struck);
    }
  return g_string_free (result, FALSE);
}

gchar *
terminal_application_monitor_format_title (TerminalApplicationMonitor *monitor,
                                           const gchar *title)
{
  if (monitor->build == NULL || (!terminal_build_monitor_is_known (monitor->build) && !terminal_build_monitor_is_stale (monitor->build)))
    return g_strdup (title);
  return terminal_application_title_format (title, terminal_application_monitor_get_label (monitor),
                                            terminal_application_monitor_is_stale (monitor));
}

static void
terminal_application_monitor_finalize (GObject *object)
{
  TerminalApplicationMonitor *monitor = TERMINAL_APPLICATION_MONITOR (object);
  if (monitor->fd >= 0)
    close (monitor->fd);
  clear_application (monitor);
  G_OBJECT_CLASS (terminal_application_monitor_parent_class)->finalize (object);
}

static void
terminal_application_monitor_class_init (TerminalApplicationMonitorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = terminal_application_monitor_finalize;
  g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
terminal_application_monitor_init (TerminalApplicationMonitor *monitor)
{
  monitor->fd = -1;
  monitor->original_fd = -1;
}

TerminalApplicationMonitor *
terminal_application_monitor_new (void)
{
  return g_object_new (TERMINAL_TYPE_APPLICATION_MONITOR, NULL);
}
