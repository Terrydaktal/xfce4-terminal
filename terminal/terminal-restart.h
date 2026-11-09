#ifndef TERMINAL_RESTART_H
#define TERMINAL_RESTART_H

#include "terminal-screen.h"

gboolean
terminal_restart_is_pending (TerminalScreen *screen);
gchar *
terminal_restart_prepare (TerminalScreen *screen, const gchar *owner, guint child, gchar **argv, const gchar *cwd, GError **error);
gboolean
terminal_restart_commit (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error);
gboolean
terminal_restart_cancel (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error);
gchar *
terminal_restart_status (TerminalScreen *screen, const gchar *owner, const gchar *token, GError **error);

#endif
