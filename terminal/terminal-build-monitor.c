#include <glib/gstdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "terminal-build-monitor.h"
#include "terminal-build-id.h"

typedef struct
{
  GFileMonitor *monitor;
  GFile *entry;
} BuildWatch;

typedef struct
{
  gchar *path;
  gchar *id;
  struct stat info;
  GPtrArray *paths;
  gboolean executable;
} BuildCheck;

struct _TerminalBuildMonitor
{
  GObject parent_instance;
  gchar *launch_path;
  gchar *running_id;
  TerminalBuildLaunchKind kind;
  GPtrArray *watches;
  BuildCheck *last_check;
  guint debounce_id;
  guint fallback_id;
  gboolean checking;
  gboolean check_again;
  gboolean stale;
};

static gchar *startup_path;
static gchar *startup_id;
static TerminalBuildLaunchKind startup_kind;
static TerminalBuildMonitor *shared_monitor;

G_DEFINE_TYPE (TerminalBuildMonitor, terminal_build_monitor, G_TYPE_OBJECT)

static void
watch_free (gpointer data)
{
  BuildWatch *watch = data;

  g_signal_handlers_destroy (watch->monitor);
  g_file_monitor_cancel (watch->monitor);
  g_object_unref (watch->monitor);
  g_object_unref (watch->entry);
  g_free (watch);
}

static void
check_free (gpointer data)
{
  BuildCheck *check = data;

  g_free (check->path);
  g_free (check->id);
  g_ptr_array_unref (check->paths);
  g_free (check);
}

static void
terminal_build_monitor_finalize (GObject *object)
{
  TerminalBuildMonitor *monitor = TERMINAL_BUILD_MONITOR (object);

  if (monitor->debounce_id != 0)
    g_source_remove (monitor->debounce_id);
  if (monitor->fallback_id != 0)
    g_source_remove (monitor->fallback_id);
  g_ptr_array_unref (monitor->watches);
  g_clear_pointer (&monitor->last_check, check_free);
  g_free (monitor->launch_path);
  g_free (monitor->running_id);
  G_OBJECT_CLASS (terminal_build_monitor_parent_class)->finalize (object);
}

static void
terminal_build_monitor_class_init (TerminalBuildMonitorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = terminal_build_monitor_finalize;
  g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new ("checked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
terminal_build_monitor_init (TerminalBuildMonitor *monitor)
{
  monitor->watches = g_ptr_array_new_with_free_func (watch_free);
}

static gboolean
debounced_check (gpointer data)
{
  TerminalBuildMonitor *monitor = data;

  monitor->debounce_id = 0;
  terminal_build_monitor_refresh (monitor);
  return G_SOURCE_REMOVE;
}

static void
file_changed (GFileMonitor *file_monitor,
              GFile *file,
              GFile *other_file,
              GFileMonitorEvent event,
              TerminalBuildMonitor *monitor)
{
  for (guint i = 0; i < monitor->watches->len; i++)
    {
      BuildWatch *watch = g_ptr_array_index (monitor->watches, i);

      if (watch->monitor == file_monitor
          && ((file != NULL && g_file_equal (file, watch->entry))
              || (other_file != NULL && g_file_equal (other_file, watch->entry))))
        {
          if (monitor->debounce_id != 0)
            g_source_remove (monitor->debounce_id);
          monitor->debounce_id = g_timeout_add (1000, debounced_check, monitor);
          return;
        }
    }
}

static void
add_path (GPtrArray *paths,
          const gchar *path)
{
  if (path == NULL)
    return;
  for (guint i = 0; i < paths->len; i++)
    if (g_str_equal (path, g_ptr_array_index (paths, i)))
      return;
  g_ptr_array_add (paths, g_strdup (path));
}

static gboolean
same_file (const struct stat *a,
           const struct stat *b)
{
  if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_size != b->st_size
      || a->st_mtime != b->st_mtime || a->st_ctime != b->st_ctime)
    return FALSE;
#ifdef __linux__
  if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec || a->st_ctim.tv_nsec != b->st_ctim.tv_nsec)
    return FALSE;
#endif
  return TRUE;
}

gchar *
terminal_build_monitor_resolve_executable (TerminalBuildMonitor *monitor)
{
  gchar *resolved;
  gchar *path = NULL;

  g_return_val_if_fail (TERMINAL_IS_BUILD_MONITOR (monitor), NULL);
  if (monitor->launch_path == NULL)
    return NULL;
  resolved = realpath (monitor->launch_path, NULL);
  if (monitor->kind == TERMINAL_BUILD_LAUNCH_REPO_WRAPPER)
    {
      /* Only our known wrapper layout is interpreted; no scripts are executed. */
      if (resolved != NULL)
        {
          gchar *base = g_path_get_basename (resolved);
          if (g_str_equal (base, "run-patched-xfce4-terminal.sh"))
            {
              gchar *directory = g_path_get_dirname (resolved);
              path = g_build_filename (directory, "build", "terminal", "xfce4-terminal", NULL);
              g_free (directory);
            }
          else
            {
              /* The launch symlink may have been repointed directly to a binary. */
              path = g_strdup (resolved);
            }
          g_free (base);
        }
    }
  else
    path = g_strdup (monitor->launch_path);
  g_free (resolved);
  return path;
}

const gchar *
terminal_build_monitor_get_launch_path (TerminalBuildMonitor *monitor)
{
  return monitor->launch_path;
}

TerminalBuildLaunchKind
terminal_build_monitor_get_launch_kind (TerminalBuildMonitor *monitor)
{
  return monitor->kind;
}

static void
check_worker (GTask *task,
              gpointer source_object,
              gpointer task_data,
              GCancellable *cancellable)
{
  TerminalBuildMonitor *monitor = source_object;
  BuildCheck *previous = task_data;
  BuildCheck *check = g_new0 (BuildCheck, 1);
  gchar *resolved = monitor->launch_path != NULL ? realpath (monitor->launch_path, NULL) : NULL;
  gchar *canonical;
  struct stat after;

  check->paths = g_ptr_array_new_with_free_func (g_free);
  add_path (check->paths, monitor->launch_path);
  add_path (check->paths, resolved);
  check->path = terminal_build_monitor_resolve_executable (monitor);
  add_path (check->paths, check->path);
  canonical = check->path != NULL ? realpath (check->path, NULL) : NULL;
  add_path (check->paths, canonical);
  g_free (canonical);
  g_free (resolved);

  if (check->path != NULL && g_stat (check->path, &check->info) == 0 && S_ISREG (check->info.st_mode))
    {
      if (previous != NULL && previous->id != NULL
          && g_strcmp0 (check->path, previous->path) == 0 && same_file (&check->info, &previous->info))
        check->id = g_strdup (previous->id);
      else
        check->id = terminal_build_id_read (check->path);
      if (g_stat (check->path, &after) != 0 || !same_file (&check->info, &after))
        g_clear_pointer (&check->id, g_free);
      check->executable = check->id != NULL && g_file_test (check->path, G_FILE_TEST_IS_EXECUTABLE);
    }
  g_task_return_pointer (task, check, check_free);
}

static gboolean
update_watches (TerminalBuildMonitor *monitor,
                BuildCheck *check)
{
  gboolean same = monitor->last_check != NULL
                  && check->paths->len == monitor->last_check->paths->len;
  guint old_count = monitor->watches->len;

  for (guint i = 0; same && i < check->paths->len; i++)
    same = g_str_equal (g_ptr_array_index (check->paths, i),
                        g_ptr_array_index (monitor->last_check->paths, i));
  if (same && monitor->watches->len == check->paths->len)
    return FALSE;
  g_ptr_array_set_size (monitor->watches, 0);
  for (guint i = 0; i < check->paths->len; i++)
    {
      GFile *entry = g_file_new_for_path (g_ptr_array_index (check->paths, i));
      GFile *parent = g_file_get_parent (entry);
      GFileMonitor *file_monitor = parent != NULL
                                     ? g_file_monitor_directory (parent, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL)
                                     : NULL;

      g_clear_object (&parent);
      if (file_monitor != NULL)
        {
          BuildWatch *watch = g_new0 (BuildWatch, 1);
          watch->monitor = file_monitor;
          watch->entry = entry;
          g_ptr_array_add (monitor->watches, watch);
          g_signal_connect (file_monitor, "changed", G_CALLBACK (file_changed), monitor);
        }
      else
        g_object_unref (entry);
    }
  return monitor->watches->len > 0 && (!same || old_count != monitor->watches->len);
}

static void
check_finished (GObject *object,
                GAsyncResult *result,
                gpointer data)
{
  TerminalBuildMonitor *monitor = TERMINAL_BUILD_MONITOR (object);
  BuildCheck *check = g_task_propagate_pointer (G_TASK (result), NULL);
  gboolean stale = check->id != NULL && !g_str_equal (check->id, monitor->running_id);
  gboolean changed = (check->id != NULL && stale != monitor->stale)
                     || terminal_build_monitor_has_update (monitor) != (stale && check->executable);

  /* Close the read-before-watch race, including a newly retargeted launcher. */
  monitor->check_again |= update_watches (monitor, check);
  g_clear_pointer (&monitor->last_check, check_free);
  monitor->last_check = check;
  monitor->checking = FALSE;
  /* A missing/partial candidate is unknown, not evidence that a stale build is fresh. */
  if (check->id != NULL)
    monitor->stale = stale;
  if (changed)
    g_signal_emit_by_name (monitor, "changed");
  g_signal_emit_by_name (monitor, "checked");
  if (monitor->check_again)
    {
      monitor->check_again = FALSE;
      terminal_build_monitor_refresh (monitor);
    }
}

void
terminal_build_monitor_refresh (TerminalBuildMonitor *monitor)
{
  GTask *task;

  if (monitor->running_id == NULL || monitor->launch_path == NULL)
    return;
  if (monitor->checking)
    {
      monitor->check_again = TRUE;
      return;
    }
  monitor->checking = TRUE;
  task = g_task_new (monitor, NULL, check_finished, NULL);
  /* One worker at a time: last_check cannot change until its callback finishes. */
  g_task_set_task_data (task, monitor->last_check, NULL);
  g_task_run_in_thread (task, check_worker);
  g_object_unref (task);
}

static gboolean
fallback_check (gpointer data)
{
  terminal_build_monitor_refresh (data);
  return G_SOURCE_CONTINUE;
}

static TerminalBuildMonitor *
monitor_new (const gchar *id,
             const gchar *launch_path,
             TerminalBuildLaunchKind kind)
{
  TerminalBuildMonitor *monitor = g_object_new (TERMINAL_TYPE_BUILD_MONITOR, NULL);

  monitor->running_id = g_strdup (id);
  monitor->launch_path = launch_path != NULL ? g_canonicalize_filename (launch_path, NULL) : NULL;
  monitor->kind = kind;
  if (monitor->running_id != NULL && monitor->launch_path != NULL)
    {
      monitor->fallback_id = g_timeout_add_seconds (30, fallback_check, monitor);
      terminal_build_monitor_refresh (monitor);
    }
  return monitor;
}

TerminalBuildMonitor *
terminal_build_monitor_new (const gchar *running_executable,
                            const gchar *launch_path,
                            TerminalBuildLaunchKind kind)
{
  gchar *id = terminal_build_id_read (running_executable);
  TerminalBuildMonitor *monitor = monitor_new (id, launch_path, kind);

  g_free (id);
  return monitor;
}

TerminalBuildMonitor *
terminal_build_monitor_new_for_id (const gchar *running_id,
                                   const gchar *launch_path)
{
  return monitor_new (running_id, launch_path, TERMINAL_BUILD_LAUNCH_EXECUTABLE);
}

gboolean
terminal_build_monitor_is_known (TerminalBuildMonitor *monitor)
{
  return monitor->last_check != NULL && monitor->last_check->id != NULL;
}

void
terminal_build_monitor_initialize (const gchar *argv0)
{
  const gchar *path = g_getenv ("XFCE4_TERMINAL_LAUNCH_PATH");
  const gchar *kind = g_getenv ("XFCE4_TERMINAL_LAUNCH_KIND");
  gchar *program;

  g_clear_pointer (&startup_path, g_free);
  g_clear_pointer (&startup_id, g_free);
  startup_kind = TERMINAL_BUILD_LAUNCH_EXECUTABLE;
  if (path != NULL && g_strcmp0 (kind, "repo-wrapper") == 0)
    {
      startup_path = g_canonicalize_filename (path, NULL);
      startup_kind = TERMINAL_BUILD_LAUNCH_REPO_WRAPPER;
    }
  else if (path != NULL && g_strcmp0 (kind, "executable") == 0)
    startup_path = g_canonicalize_filename (path, NULL);
  else
    {
      program = argv0 != NULL ? g_find_program_in_path (argv0) : NULL;
      startup_path = program != NULL ? g_canonicalize_filename (program, NULL) : NULL;
      g_free (program);
    }
  g_unsetenv ("XFCE4_TERMINAL_LAUNCH_PATH");
  g_unsetenv ("XFCE4_TERMINAL_LAUNCH_KIND");
  startup_id = terminal_build_id_read ("/proc/self/exe");
}

TerminalBuildMonitor *
terminal_build_monitor_get (void)
{
  if (shared_monitor == NULL)
    {
      if (startup_id == NULL)
        {
          startup_id = terminal_build_id_read ("/proc/self/exe");
          if (startup_path == NULL)
            startup_path = g_file_read_link ("/proc/self/exe", NULL);
        }
      shared_monitor = monitor_new (startup_id, startup_path, startup_kind);
      g_object_add_weak_pointer (G_OBJECT (shared_monitor), (gpointer *) &shared_monitor);
    }
  else
    g_object_ref (shared_monitor);
  return shared_monitor;
}

gboolean
terminal_build_monitor_is_stale (TerminalBuildMonitor *monitor)
{
  return monitor->stale;
}

gboolean
terminal_build_monitor_has_update (TerminalBuildMonitor *monitor)
{
  g_return_val_if_fail (TERMINAL_IS_BUILD_MONITOR (monitor), FALSE);
  return monitor->stale && monitor->last_check != NULL
         && monitor->last_check->id != NULL && monitor->last_check->executable;
}
