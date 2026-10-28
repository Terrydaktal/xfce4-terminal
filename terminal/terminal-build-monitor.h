#ifndef TERMINAL_BUILD_MONITOR_H
#define TERMINAL_BUILD_MONITOR_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define TERMINAL_TYPE_BUILD_MONITOR (terminal_build_monitor_get_type ())
G_DECLARE_FINAL_TYPE (TerminalBuildMonitor, terminal_build_monitor, TERMINAL, BUILD_MONITOR, GObject)

typedef enum
{
  TERMINAL_BUILD_LAUNCH_EXECUTABLE,
  TERMINAL_BUILD_LAUNCH_REPO_WRAPPER
} TerminalBuildLaunchKind;

/* Consume private launcher metadata before GTK starts threads or children. */
void
terminal_build_monitor_initialize (const gchar *argv0);

TerminalBuildMonitor *
terminal_build_monitor_get (void);

TerminalBuildMonitor *
terminal_build_monitor_new (const gchar *running_executable,
                            const gchar *launch_path,
                            TerminalBuildLaunchKind kind);

gboolean
terminal_build_monitor_is_stale (TerminalBuildMonitor *monitor);

/* Cached: a different, readable build is currently available and executable. */
gboolean
terminal_build_monitor_has_update (TerminalBuildMonitor *monitor);

/* Resolve the current launch target without executing a wrapper. Caller frees. */
gchar *
terminal_build_monitor_resolve_executable (TerminalBuildMonitor *monitor);

const gchar *
terminal_build_monitor_get_launch_path (TerminalBuildMonitor *monitor);

TerminalBuildLaunchKind
terminal_build_monitor_get_launch_kind (TerminalBuildMonitor *monitor);

void
terminal_build_monitor_refresh (TerminalBuildMonitor *monitor);

G_END_DECLS

#endif
