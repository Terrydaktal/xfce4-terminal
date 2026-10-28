/* Local executable identity; never executes a candidate binary. */
#ifndef TERMINAL_BUILD_ID_H
#define TERMINAL_BUILD_ID_H

#include <glib.h>

gchar *
terminal_build_id_read (const gchar *path);

gchar *
terminal_build_title_format (const gchar *title,
                             const gchar *label,
                             gboolean stale);

#endif
