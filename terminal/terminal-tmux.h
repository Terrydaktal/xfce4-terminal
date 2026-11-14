#ifndef TERMINAL_TMUX_H
#define TERMINAL_TMUX_H

#include <glib.h>
#include <sys/types.h>

/* Local explicit-socket clients only. Zero means unavailable, never a guess. */
pid_t
terminal_tmux_pane_foreground_pid (pid_t client_pid);

/* True only for a verified pane without application mouse input, or copy mode. */
gboolean
terminal_tmux_uses_native_context_menu (pid_t client_pid);

/* Copy retained history without forwarding a key or leaving copy mode. */
gboolean
terminal_tmux_copy_selection (pid_t client_pid);

/* Transfer a completed terminal selection; return its verified pane ID. */
gchar *
terminal_tmux_retain_selection (pid_t client_pid, glong sx, glong sy,
                               glong ex, glong ey, gboolean rectangle);

/* Clear only the native selection previously transferred to this pane. */
void
terminal_tmux_clear_native_selection (pid_t client_pid, const gchar *pane);

#endif
