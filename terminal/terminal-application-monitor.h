#ifndef TERMINAL_APPLICATION_MONITOR_H
#define TERMINAL_APPLICATION_MONITOR_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define TERMINAL_TYPE_APPLICATION_MONITOR (terminal_application_monitor_get_type ())
G_DECLARE_FINAL_TYPE (TerminalApplicationMonitor, terminal_application_monitor, TERMINAL, APPLICATION_MONITOR, GObject)

TerminalApplicationMonitor *
terminal_application_monitor_new (void);

/* Borrowed PTY descriptor; the monitor duplicates it and never reads PTY input. */
void
terminal_application_monitor_set_target (TerminalApplicationMonitor *monitor,
                                         gint pty_fd,
                                         GPid child_pid);

void
terminal_application_monitor_refresh (TerminalApplicationMonitor *monitor);

const gchar *
terminal_application_monitor_get_label (TerminalApplicationMonitor *monitor);

gboolean
terminal_application_monitor_is_stale (TerminalApplicationMonitor *monitor);

gchar *
terminal_application_monitor_format_title (TerminalApplicationMonitor *monitor,
                                           const gchar *title);

gchar *
terminal_application_title_format (const gchar *title,
                                   const gchar *label,
                                   gboolean stale);

G_END_DECLS

#endif
