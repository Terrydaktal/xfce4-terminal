#ifndef TERMINAL_FRESHNESS_CLIENT_H
#define TERMINAL_FRESHNESS_CLIENT_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define TERMINAL_TYPE_FRESHNESS_CLIENT (terminal_freshness_client_get_type ())
G_DECLARE_FINAL_TYPE (TerminalFreshnessClient, terminal_freshness_client, TERMINAL, FRESHNESS_CLIENT, GObject)

TerminalFreshnessClient *
terminal_freshness_client_new (void);

void
terminal_freshness_client_set_target (TerminalFreshnessClient *client,
                                      GPid pid,
                                      guint64 start_time,
                                      guint64 image_dev,
                                      guint64 image_ino,
                                      const gchar *launch_path);

gboolean
terminal_freshness_client_is_known (TerminalFreshnessClient *client);

gboolean
terminal_freshness_client_is_stale (TerminalFreshnessClient *client);

G_END_DECLS

#endif
