#ifndef TERMINAL_UPDATE_H
#define TERMINAL_UPDATE_H

#include <gtk/gtk.h>

#include "terminal-build-monitor.h"

G_BEGIN_DECLS

gboolean
terminal_update_is_pending (GtkWindow *window);

const gchar *
terminal_update_get_service (GtkWindow *window);

gboolean
terminal_update_authorize_close (GtkWindow *window, const gchar *service, const gchar *sender);

gboolean
terminal_update_hold (GtkWindow *window, const gchar *service, const gchar *sender, gboolean hold);

void
terminal_update_in_place (GtkWindow *window,
                          const gchar *window_uuid,
                          TerminalBuildMonitor *monitor);

void
terminal_update_in_place_confirmed (GtkWindow *window,
                                    const gchar *window_uuid,
                                    TerminalBuildMonitor *monitor);

G_END_DECLS

#endif
