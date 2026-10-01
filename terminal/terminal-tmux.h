#ifndef TERMINAL_TMUX_H
#define TERMINAL_TMUX_H

#include <glib.h>
#include <sys/types.h>

/* Local explicit-socket clients only. Zero means unavailable, never a guess. */
pid_t
terminal_tmux_pane_foreground_pid (pid_t client_pid);

/* Copy retained history without forwarding a key or leaving copy mode. */
gboolean
terminal_tmux_copy_selection (pid_t client_pid);

#endif
