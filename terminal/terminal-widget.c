/*-
 * Copyright (c) 2004-2007 os-cillation e.K.
 *
 * Written by Benedikt Meurer <benny@xfce.org>.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_LIBUTEMPTER
#include <utempter.h>
#endif

#include <glib/gstdio.h>

#include <libxfce4ui/libxfce4ui.h>

#include "terminal-enum-types.h"
#include "terminal-marshal.h"
#include "terminal-preferences.h"
#include "terminal-private.h"
#include "terminal-regex.h"
#include "terminal-tmux.h"
#include "terminal-util.h"
#include "terminal-widget.h"



#define MAILTO "mailto:"

#define IMAGE_DROP_MAX_AGE_SECONDS (60 * 60)
#define IMAGE_DROP_CLEANUP_INTERVAL_SECONDS (15 * 60)



enum
{
  GET_CONTEXT_MENU,
  PASTE_SELECTION_REQUEST,
  PASTE_CLIPBOARD_REQUEST,

  LAST_SIGNAL,
};

typedef enum
{
  PATTERN_TYPE_NONE,
  PATTERN_TYPE_FULL_HTTP,
  PATTERN_TYPE_HTTP,
  PATTERN_TYPE_EMAIL,
  PATTERN_TYPE_FILE,
  PATTERN_TYPE_PATH
} PatternType;

enum
{
  PROP_ACCEL_GROUP = 1,
  N_PROPERTIES
};

typedef struct
{
  const gchar *pattern;
  PatternType type;
} TerminalRegexPattern;

typedef struct
{
  gchar *uri;
  PatternType type;
} TerminalHyperlink;

typedef struct
{
  GWeakRef widget_ref;
  guint32 event_time;
  gboolean select_parent;
  GCancellable *cancellable;
  gchar *key;
  gchar *needle;
} TerminalUnearthSearch;

static const TerminalRegexPattern regex_patterns[] = {
  { REGEX_URL_AS_IS, PATTERN_TYPE_FULL_HTTP },
  { REGEX_URL_HTTP, PATTERN_TYPE_HTTP },
  { REGEX_URL_FILE, PATTERN_TYPE_FILE },
  { REGEX_EMAIL, PATTERN_TYPE_EMAIL },
  { REGEX_NEWS_MAN, PATTERN_TYPE_FULL_HTTP },
  { REGEX_FILE_PATH, PATTERN_TYPE_PATH },
};

static gchar *image_drop_directory;



static void
terminal_widget_finalize (GObject *object);
static void
terminal_widget_dispose (GObject *object);
static void
terminal_widget_set_property (GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec);
static gboolean
terminal_widget_button_press_event (GtkWidget *widget,
                                    GdkEventButton *event);
static gboolean
terminal_widget_button_release_event (GtkWidget *widget,
                                      GdkEventButton *event);
static gboolean
terminal_widget_motion_notify_event (GtkWidget *widget,
                                     GdkEventMotion *event);
static void
terminal_widget_drag_data_received (GtkWidget *widget,
                                    GdkDragContext *context,
                                    gint x,
                                    gint y,
                                    GtkSelectionData *selection_data,
                                    guint info,
                                    guint time);
static gchar *
terminal_widget_save_image_drop (GtkSelectionData *selection_data);
static void
terminal_widget_cleanup_image_drop_directory (gboolean remove_all);
static gboolean
terminal_widget_key_press_event (GtkWidget *widget,
                                 GdkEventKey *event);
static void
terminal_widget_open_uri (TerminalWidget *widget,
                          const gchar *wlink,
                          PatternType type,
                          guint32 event_time);
static gboolean
terminal_widget_is_pcmanfm_selection_uri (const gchar *uri);
static gchar *
terminal_widget_get_pcmanfm_selection_fallback_uri (const gchar *uri);
static gboolean
terminal_widget_open_parent_selection_uri (TerminalWidget *widget,
                                           const gchar *uri);
static gchar *
terminal_widget_candidate_to_path (TerminalWidget *widget,
                                   const gchar *candidate,
                                   gboolean require_allowlist);
static gchar *
terminal_widget_normalize_path_candidate (const gchar *candidate,
                                          gboolean *was_wrapped);
static gboolean
terminal_widget_start_unearth_search_for_path (TerminalWidget *widget,
                                               const gchar *path,
                                               guint32 event_time,
                                               gboolean select_parent);
static gboolean
terminal_widget_open_path_candidate (TerminalWidget *widget,
                                     const gchar *candidate,
                                     guint32 event_time,
                                     gboolean require_allowlist);
static gboolean
terminal_widget_open_parent_path_candidate (TerminalWidget *widget,
                                            const gchar *candidate,
                                            guint32 event_time,
                                            gboolean require_allowlist);
static gboolean
terminal_widget_open_pcmanfm_selection_uri (GtkWindow *window,
                                            const gchar *uri,
                                            TerminalWidget *widget);
static void
terminal_widget_update_highlight_urls (TerminalWidget *widget);
static gboolean
terminal_widget_action_shift_scroll_up (TerminalWidget *widget);
static gboolean
terminal_widget_action_shift_scroll_down (TerminalWidget *widget);
static gboolean
terminal_widget_action_scroll_page_up (TerminalWidget *widget);
static gboolean
terminal_widget_action_scroll_page_down (TerminalWidget *widget);
static void
terminal_widget_connect_accelerators (TerminalWidget *widget);
static void
terminal_widget_disconnect_accelerators (TerminalWidget *widget);
static TerminalHyperlink
terminal_widget_get_link (TerminalWidget *widget,
                          GdkEvent *event);
static gboolean
terminal_widget_link_clickable (const gchar *uri,
                                PatternType type);
static gboolean
terminal_widget_click_insert_link_from_event (TerminalWidget *widget,
                                              GdkEventButton *event);
static gboolean
terminal_widget_click_open_link_from_event (TerminalWidget *widget,
                                            GdkEventButton *event);
static gboolean
terminal_widget_event_is_ctrl_left_click (GdkEventButton *event);
static gboolean
terminal_widget_event_is_ctrl_shift_left_click (GdkEventButton *event);
static gboolean
terminal_widget_click_open_parent_from_event (TerminalWidget *widget,
                                              GdkEventButton *event);
static gchar *
terminal_widget_get_current_directory_path (TerminalWidget *widget);
static gchar *
terminal_widget_selected_existing_path_uri (TerminalWidget *widget);
static void
terminal_widget_hyperlink_hover_uri_changed (TerminalWidget *widget,
                                             const char *uri,
                                             const GdkRectangle *bbox G_GNUC_UNUSED);
static void
terminal_widget_update_hyperlink_tooltip (TerminalWidget *widget);
static gboolean
terminal_widget_foreground_process_allows_path_detection (TerminalWidget *widget);
static gchar *
terminal_widget_link_to_input (const gchar *uri,
                               PatternType type);
static gchar *
terminal_widget_encode_unearth_path (const gchar *path);
static gchar *
terminal_widget_decode_unearth_path (const gchar *path,
                                     gsize length);
static gboolean
terminal_widget_regex_tag_is_path (TerminalWidget *widget,
                                   gint tag);
static gboolean
terminal_widget_key_should_scroll_to_bottom (GdkEventKey *event);
static void
terminal_widget_scroll_to_bottom (TerminalWidget *widget);



typedef enum
{
  TERMINAL_MOUSE_SELECTION_DEFAULT,
  TERMINAL_MOUSE_SELECTION_LOCAL,
  TERMINAL_MOUSE_SELECTION_APPLICATION,
} TerminalMouseSelection;

struct _TerminalWidget
{
  VteTerminal parent_instance;

  /*< private >*/
  TerminalPreferences *preferences;
  GtkAccelGroup *accel_group;
  gint regex_tags[G_N_ELEMENTS (regex_patterns)];
  pcre2_code_8 *regex_pcre[G_N_ELEMENTS (regex_patterns)];
  TerminalUnearthSearch *unearth_search;
  TerminalMouseSelection mouse_selection;
  gboolean link_selection_drag_pending;
  gdouble link_selection_click_x;
  gdouble link_selection_click_y;
};



static guint widget_signals[LAST_SIGNAL];



static const GtkTargetEntry targets[] = {
  { "text/uri-list", 0, TARGET_URI_LIST },
  { "text/x-moz-url", 0, TARGET_MOZ_URL },
  { "UTF8_STRING", 0, TARGET_UTF8_STRING },
  { "TEXT", 0, TARGET_TEXT },
  { "COMPOUND_TEXT", 0, TARGET_COMPOUND_TEXT },
  { "STRING", 0, TARGET_STRING },
  { "text/plain", 0, TARGET_TEXT_PLAIN },
  { "application/x-color", 0, TARGET_APPLICATION_X_COLOR },
  { "image/png", 0, TARGET_IMAGE },
  { "image/jpeg", 0, TARGET_IMAGE },
  { "image/gif", 0, TARGET_IMAGE },
  { "image/webp", 0, TARGET_IMAGE },
  { "image/bmp", 0, TARGET_IMAGE },
  { "image/tiff", 0, TARGET_IMAGE },
  { "GTK_NOTEBOOK_TAB", GTK_TARGET_SAME_APP, TARGET_GTK_NOTEBOOK_TAB },
};



static XfceGtkActionEntry action_entries[] = {
  {
    TERMINAL_WIDGET_ACTION_SCROLL_UP,
    "<Actions>/terminal-widget/shift-up",
    "<Shift>Up",
    XFCE_GTK_MENU_ITEM,
    N_ ("Scroll one line Up"),
    NULL,
    NULL,
    G_CALLBACK (terminal_widget_action_shift_scroll_up),
  },
  {
    TERMINAL_WIDGET_ACTION_SCROLL_DOWN,
    "<Actions>/terminal-widget/shift-down",
    "<Shift>Down",
    XFCE_GTK_MENU_ITEM,
    N_ ("Scroll one line Down"),
    NULL,
    NULL,
    G_CALLBACK (terminal_widget_action_shift_scroll_down),
  },
  {
    TERMINAL_WIDGET_ACTION_SCROLL_PAGE_UP,
    "<Actions>/terminal-widget/shift-pageup",
    "<Shift>Page_Up",
    XFCE_GTK_MENU_ITEM,
    N_ ("Scroll one Page Up"),
    NULL,
    NULL,
    G_CALLBACK (terminal_widget_action_scroll_page_up),
  },
  {
    TERMINAL_WIDGET_ACTION_SCROLL_PAGE_DOWN,
    "<Actions>/terminal-widget/shift-pagedown",
    "<Shift>Page_Down",
    XFCE_GTK_MENU_ITEM,
    N_ ("Scroll one Page Down"),
    NULL,
    NULL,
    G_CALLBACK (terminal_widget_action_scroll_page_down),
  },
};

#define get_action_entry(id) xfce_gtk_get_action_entry_by_id (action_entries, G_N_ELEMENTS (action_entries), id)

static GParamSpec *terminal_widget_props[N_PROPERTIES] = {
  NULL,
};



G_DEFINE_TYPE (TerminalWidget, terminal_widget, VTE_TYPE_TERMINAL)



static void
terminal_widget_class_init (TerminalWidgetClass *klass)
{
  GtkWidgetClass *gtkwidget_class;
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = terminal_widget_dispose;
  gobject_class->finalize = terminal_widget_finalize;
  gobject_class->set_property = terminal_widget_set_property;

  gtkwidget_class = GTK_WIDGET_CLASS (klass);
  gtkwidget_class->button_press_event = terminal_widget_button_press_event;
  gtkwidget_class->button_release_event = terminal_widget_button_release_event;
  gtkwidget_class->motion_notify_event = terminal_widget_motion_notify_event;
  gtkwidget_class->drag_data_received = terminal_widget_drag_data_received;
  gtkwidget_class->key_press_event = terminal_widget_key_press_event;

  xfce_gtk_translate_action_entries (action_entries, G_N_ELEMENTS (action_entries));

  /**
   * TerminalWidget::get-context-menu:
   **/
  widget_signals[GET_CONTEXT_MENU] =
    g_signal_new (I_ ("context-menu"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  _terminal_marshal_OBJECT__VOID,
                  GTK_TYPE_MENU, 0);

  /**
   * TerminalWidget::paste-selection-request:
   **/
  widget_signals[PASTE_SELECTION_REQUEST] =
    g_signal_new (I_ ("paste-selection-request"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  /**
   * TerminalWidget::paste-clipboard-request:
   **/
  widget_signals[PASTE_CLIPBOARD_REQUEST] =
    g_signal_new (I_ ("paste-clipboard-request"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  terminal_widget_props[PROP_ACCEL_GROUP] =
    g_param_spec_object ("accel-group",
                         "accel-group",
                         "accel-group",
                         GTK_TYPE_ACCEL_GROUP,
                         G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, terminal_widget_props);
}



static void
terminal_widget_init (TerminalWidget *widget)
{
  /* query preferences connection */
  widget->preferences = terminal_preferences_get ();

  /* unset tags */
  memset (widget->regex_tags, -1, sizeof (widget->regex_tags));

  /* setup Drag'n'Drop support */
  gtk_drag_dest_set (GTK_WIDGET (widget),
                     GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_HIGHLIGHT | GTK_DEST_DEFAULT_DROP,
                     targets, G_N_ELEMENTS (targets),
                     GDK_ACTION_COPY | GDK_ACTION_LINK | GDK_ACTION_MOVE);

  /* monitor the misc-highlight-urls setting */
  g_signal_connect_swapped (G_OBJECT (widget->preferences), "notify::misc-highlight-urls",
                            G_CALLBACK (terminal_widget_update_highlight_urls), widget);
  g_signal_connect_swapped (G_OBJECT (widget->preferences), "notify::misc-auto-detect-file-paths",
                            G_CALLBACK (terminal_widget_update_highlight_urls), widget);

  /* update tooltip when hovering over a hyperlink */
  g_signal_connect (G_OBJECT (widget), "hyperlink-hover-uri-changed",
                    G_CALLBACK (terminal_widget_hyperlink_hover_uri_changed), NULL);
  g_signal_connect_swapped (G_OBJECT (widget->preferences), "notify::misc-hyperlink-tooltips-enabled",
                            G_CALLBACK (terminal_widget_update_hyperlink_tooltip), widget);

  /* monitor the misc-hyperlinks-enabled setting */
  g_object_bind_property (G_OBJECT (widget->preferences), "misc-hyperlinks-enabled",
                          G_OBJECT (widget), "allow-hyperlink",
                          G_BINDING_SYNC_CREATE);

  /* apply the initial misc-highlight-urls setting */
  terminal_widget_update_highlight_urls (widget);

  widget->accel_group = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
    {
      gint error_number;
      PCRE2_SIZE error_offset;

      widget->regex_pcre[i] = pcre2_compile_8 ((PCRE2_SPTR8) regex_patterns[i].pattern,
                                               PCRE2_ZERO_TERMINATED,
                                               PCRE2_UTF | PCRE2_UCP,
                                               &error_number, &error_offset, NULL);
      if (widget->regex_pcre[i] == NULL)
        g_warning ("Failed to compile regex, error code \"%d\".", error_number);
    }
}



static const gchar *
terminal_widget_image_drop_extension (const gchar *mime_type)
{
  if (g_strcmp0 (mime_type, "image/jpeg") == 0)
    return ".jpg";
  else if (g_strcmp0 (mime_type, "image/gif") == 0)
    return ".gif";
  else if (g_strcmp0 (mime_type, "image/webp") == 0)
    return ".webp";
  else if (g_strcmp0 (mime_type, "image/bmp") == 0)
    return ".bmp";
  else if (g_strcmp0 (mime_type, "image/tiff") == 0)
    return ".tiff";
  else
    return ".png";
}



static void
terminal_widget_cleanup_image_drop_directory (gboolean remove_all)
{
  GDir *directory;
  const gchar *name;
  gint64 now;

  if (image_drop_directory == NULL)
    return;

  directory = g_dir_open (image_drop_directory, 0, NULL);
  if (directory == NULL)
    return;

  now = g_get_real_time () / G_USEC_PER_SEC;
  while ((name = g_dir_read_name (directory)) != NULL)
    {
      gchar *path;
      GStatBuf stat_buf;
      gboolean remove_file = remove_all;

      if (!g_str_has_prefix (name, "image-"))
        continue;

      path = g_build_filename (image_drop_directory, name, NULL);
      if (!remove_file
          && g_stat (path, &stat_buf) == 0
          && now >= (gint64) stat_buf.st_mtime
          && now - (gint64) stat_buf.st_mtime >= IMAGE_DROP_MAX_AGE_SECONDS)
        remove_file = TRUE;

      if (remove_file)
        g_unlink (path);
      g_free (path);
    }

  g_dir_close (directory);

  if (remove_all)
    {
      g_rmdir (image_drop_directory);
      g_clear_pointer (&image_drop_directory, g_free);
    }
}



static gboolean
terminal_widget_cleanup_image_drop_timer (gpointer data G_GNUC_UNUSED)
{
  if (image_drop_directory == NULL)
    return G_SOURCE_REMOVE;

  terminal_widget_cleanup_image_drop_directory (FALSE);
  return G_SOURCE_CONTINUE;
}



static void
terminal_widget_cleanup_image_drop_at_exit (void)
{
  terminal_widget_cleanup_image_drop_directory (TRUE);
}



static const gchar *
terminal_widget_get_image_drop_directory (void)
{
  GError *error = NULL;

  if (image_drop_directory != NULL)
    return image_drop_directory;

  image_drop_directory = g_dir_make_tmp ("xfce4-terminal-image-drop-XXXXXX", &error);
  if (image_drop_directory == NULL)
    {
      g_warning ("Unable to create temporary directory for dropped images: %s",
                 error != NULL ? error->message : "unknown error");
      g_clear_error (&error);
      return NULL;
    }

  if (g_chmod (image_drop_directory, 0700) != 0)
    {
      g_warning ("Unable to secure temporary directory for dropped images: %s",
                 g_strerror (errno));
      g_rmdir (image_drop_directory);
      g_clear_pointer (&image_drop_directory, g_free);
      return NULL;
    }

  atexit (terminal_widget_cleanup_image_drop_at_exit);
  g_timeout_add_seconds (IMAGE_DROP_CLEANUP_INTERVAL_SECONDS,
                         terminal_widget_cleanup_image_drop_timer,
                         NULL);
  return image_drop_directory;
}



static gchar *
terminal_widget_save_image_drop (GtkSelectionData *selection_data)
{
  const gchar *directory;
  const guchar *data;
  gchar *mime_type;
  gchar *template;
  gchar *path;
  const gchar *extension;
  gint fd;
  gint length;
  gsize offset = 0;

  if (gtk_selection_data_get_format (selection_data) != 8
      || (length = gtk_selection_data_get_length (selection_data)) <= 0)
    {
      g_warning ("Unable to drop image: expected non-empty 8-bit image data");
      return NULL;
    }

  directory = terminal_widget_get_image_drop_directory ();
  if (directory == NULL)
    return NULL;

  terminal_widget_cleanup_image_drop_directory (FALSE);

  mime_type = gdk_atom_name (gtk_selection_data_get_data_type (selection_data));
  if (mime_type == NULL)
    mime_type = g_strdup ("image/png");

  template = g_build_filename (directory, "image-XXXXXX", NULL);
  fd = g_mkstemp (template);
  if (fd < 0)
    {
      g_warning ("Unable to create temporary image file: %s", g_strerror (errno));
      g_free (mime_type);
      g_free (template);
      return NULL;
    }

  data = gtk_selection_data_get_data (selection_data);
  while (offset < (gsize) length)
    {
      gssize written = write (fd, data + offset, (gsize) length - offset);
      if (written <= 0)
        {
          g_warning ("Unable to write dropped image: %s", g_strerror (errno));
          close (fd);
          g_unlink (template);
          g_free (mime_type);
          g_free (template);
          return NULL;
        }
      offset += written;
    }

  if (close (fd) != 0)
    {
      g_warning ("Unable to close temporary image file: %s", g_strerror (errno));
      g_unlink (template);
      g_free (mime_type);
      g_free (template);
      return NULL;
    }

  extension = terminal_widget_image_drop_extension (mime_type);
  path = g_strconcat (template, extension, NULL);
  if (g_rename (template, path) != 0)
    {
      g_warning ("Unable to finalize temporary image file: %s", g_strerror (errno));
      g_unlink (template);
      g_free (mime_type);
      g_free (template);
      g_free (path);
      return NULL;
    }

  g_free (mime_type);
  g_free (template);
  return path;
}



static void
terminal_widget_dispose (GObject *object)
{
  TerminalWidget *widget = TERMINAL_WIDGET (object);

  if (widget->unearth_search != NULL)
    g_cancellable_cancel (widget->unearth_search->cancellable);

  (*G_OBJECT_CLASS (terminal_widget_parent_class)->dispose) (object);
}



static void
terminal_widget_finalize (GObject *object)
{
  TerminalWidget *widget = TERMINAL_WIDGET (object);

#ifdef HAVE_LIBUTEMPTER
  VtePty *pty = vte_terminal_get_pty (VTE_TERMINAL (widget));
  if (VTE_IS_PTY (pty))
    utempter_remove_record (vte_pty_get_fd (pty));
#endif

  /* disconnect the misc-highlight-urls watch */
  g_signal_handlers_disconnect_by_func (G_OBJECT (widget->preferences), G_CALLBACK (terminal_widget_update_highlight_urls), widget);
  g_signal_handlers_disconnect_by_func (G_OBJECT (widget->preferences), G_CALLBACK (terminal_widget_update_hyperlink_tooltip), widget);

  /* disconnect from the preferences */
  g_object_unref (G_OBJECT (widget->preferences));

  /* disconnect accelerators */
  terminal_widget_disconnect_accelerators (widget);

  for (guint i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
    {
      if (widget->regex_pcre[i] != NULL)
        {
          pcre2_code_free_8 (widget->regex_pcre[i]);
          widget->regex_pcre[i] = NULL;
        }
    }

  (*G_OBJECT_CLASS (terminal_widget_parent_class)->finalize) (object);
}



static void
terminal_widget_set_property (GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
  TerminalWidget *widget = TERMINAL_WIDGET (object);

  switch (prop_id)
    {
    case PROP_ACCEL_GROUP:
      terminal_widget_disconnect_accelerators (widget);
      widget->accel_group = g_value_dup_object (value);
      terminal_widget_connect_accelerators (widget);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
terminal_widget_context_menu_copy (TerminalWidget *widget,
                                   GtkWidget *item)
{
  GtkClipboard *clipboard;
  const gchar *wlink;
  PatternType *type;
  GdkDisplay *display;
  gchar *clipboard_text = NULL;

  wlink = g_object_get_data (G_OBJECT (item), "terminal-widget-link");
  type = g_object_get_data (G_OBJECT (item), "terminal-widget-link-type");
  if (G_LIKELY (wlink != NULL))
    {
      display = gtk_widget_get_display (GTK_WIDGET (widget));

      if (type != NULL && (*type == PATTERN_TYPE_FILE || *type == PATTERN_TYPE_PATH))
        {
          clipboard_text = terminal_widget_link_to_input (wlink, *type);
        }
      else if (g_str_has_prefix (wlink, MAILTO))
        {
          /* strip mailto from links, bug #7909 */
          clipboard_text = g_strdup (wlink + strlen (MAILTO));
        }
      else
        clipboard_text = g_strdup (wlink);

      // The order of setting the clipboard does matter, see:
      // https://gitlab.xfce.org/apps/xfce4-terminal/-/issues/367

      /* copy the URI to "PRIMARY" */
      clipboard = gtk_clipboard_get_for_display (display, GDK_SELECTION_PRIMARY);
      gtk_clipboard_set_text (clipboard, clipboard_text, -1);

      /* copy the URI to "CLIPBOARD" */
      clipboard = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
      gtk_clipboard_set_text (clipboard, clipboard_text, -1);

      g_free (clipboard_text);
    }
}



static void
terminal_widget_context_menu_copy_all (TerminalWidget *widget)
{
  vte_terminal_select_all (VTE_TERMINAL (widget));
  vte_terminal_copy_clipboard_format (VTE_TERMINAL (widget), VTE_FORMAT_TEXT);
}



static void
terminal_widget_context_menu_open (TerminalWidget *widget,
                                   GtkWidget *item)
{
  const gchar *wlink;
  PatternType *type;

  wlink = g_object_get_data (G_OBJECT (item), "terminal-widget-link");
  type = g_object_get_data (G_OBJECT (item), "terminal-widget-link-type");

  if (wlink != NULL && type != NULL && terminal_widget_link_clickable (wlink, *type))
    {
      guint32 event_time = gtk_get_current_event_time ();

      if (*type == PATTERN_TYPE_PATH)
        terminal_widget_open_path_candidate (widget, wlink, event_time, TRUE);
      else
        terminal_widget_open_uri (widget, wlink, *type, event_time);
    }
}



static void
terminal_widget_context_menu (TerminalWidget *widget,
                              gint button,
                              guint32 event_time,
                              GdkEvent *event)
{
  GMainLoop *loop;
  GtkWidget *menu = NULL;
  GtkWidget *item_copy = NULL;
  GtkWidget *item_copy_all;
  GtkWidget *item_open = NULL;
  GtkWidget *item_separator = NULL;
  GList *children;
  guint id;
  TerminalHyperlink link;

  g_signal_emit (G_OBJECT (widget), widget_signals[GET_CONTEXT_MENU], 0, &menu);
  if (G_UNLIKELY (menu == NULL))
    return;

  /* check if we have a match */
  link = terminal_widget_get_link (widget, (GdkEvent *) event);
  if (G_UNLIKELY (link.uri != NULL))
    {
      /* prepend a separator to the menu if it does not already contain one */
      children = gtk_container_get_children (GTK_CONTAINER (menu));
      item_separator = g_list_nth_data (children, 0);
      if (!GTK_IS_SEPARATOR_MENU_ITEM (item_separator))
        {
          item_separator = gtk_separator_menu_item_new ();
          gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item_separator);
        }
      else
        item_separator = NULL;
      g_list_free (children);

      /* create menu items with appropriate labels */
      if (link.type == PATTERN_TYPE_EMAIL)
        {
          item_copy = gtk_menu_item_new_with_label (_("Copy Email Address"));
          item_open = gtk_menu_item_new_with_label (_("Compose Email"));
        }
      else if (link.type == PATTERN_TYPE_FILE || link.type == PATTERN_TYPE_PATH)
        {
          item_copy = gtk_menu_item_new_with_label (_("Copy Path"));
          if (terminal_widget_link_clickable (link.uri, link.type))
            item_open = gtk_menu_item_new_with_label (_("Open Link"));
        }
      else
        {
          item_copy = gtk_menu_item_new_with_label (_("Copy Link Address"));
          item_open = gtk_menu_item_new_with_label (_("Open Link"));
        }

      /* prepend the "COPY" menu item */
      g_object_set_data_full (G_OBJECT (item_copy), I_ ("terminal-widget-link"), g_strdup (link.uri), g_free);
      g_object_set_data_full (G_OBJECT (item_copy), I_ ("terminal-widget-link-type"), g_memdup (&link.type, sizeof (link.type)), g_free);
      g_signal_connect_swapped (G_OBJECT (item_copy), "activate", G_CALLBACK (terminal_widget_context_menu_copy), widget);
      gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item_copy);

      /* prepend the "OPEN" menu item */
      if (item_open != NULL)
        {
          g_object_set_data_full (G_OBJECT (item_open), I_ ("terminal-widget-link"), g_strdup (link.uri), g_free);
          g_object_set_data_full (G_OBJECT (item_open), I_ ("terminal-widget-link-type"), g_memdup (&link.type, sizeof (link.type)), g_free);
          g_signal_connect_swapped (G_OBJECT (item_open), "activate", G_CALLBACK (terminal_widget_context_menu_open), widget);
          gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item_open);
        }

      g_free (link.uri);
    }

  item_copy_all = gtk_menu_item_new_with_label (_("Copy All Scrollback"));
  g_signal_connect_swapped (G_OBJECT (item_copy_all), "activate",
                            G_CALLBACK (terminal_widget_context_menu_copy_all), widget);
  gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item_copy_all);

  gtk_widget_show_all (menu);

  /* take a reference on the menu */
  g_object_ref_sink (G_OBJECT (menu));

  loop = g_main_loop_new (NULL, FALSE);

  /* connect the deactivate handler */
  id = g_signal_connect_swapped (G_OBJECT (menu), "deactivate", G_CALLBACK (g_main_loop_quit), loop);

  /* make sure the menu is on the proper screen */
  gtk_menu_set_screen (GTK_MENU (menu), gtk_widget_get_screen (GTK_WIDGET (widget)));

  /* run our custom main loop */
  gtk_menu_popup_at_pointer (GTK_MENU (menu), NULL);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  /* remove the additional items (if any) */
  if (item_separator != NULL)
    gtk_widget_destroy (item_separator);
  gtk_widget_destroy (item_copy_all);
  if (item_open != NULL)
    gtk_widget_destroy (item_open);
  if (item_copy != NULL)
    gtk_widget_destroy (item_copy);

  /* unlink this deactivate callback */
  g_signal_handler_disconnect (G_OBJECT (menu), id);

  /* decrease the reference count on the menu */
  g_object_unref (G_OBJECT (menu));
}



static void
terminal_widget_commit (TerminalWidget *widget,
                        gchar *data,
                        guint length,
                        gboolean *committed)
{
  *committed = TRUE;
}



static gboolean
terminal_widget_process_looks_like_codex (pid_t pid)
{
  gchar *path = NULL;
  gchar *content = NULL;
  gsize length = 0;
  gboolean matches = FALSE;

  path = g_strdup_printf ("/proc/%d/cmdline", (gint) pid);
  if (g_file_get_contents (path, &content, &length, NULL) && length > 0)
    {
      for (gsize offset = 0; offset < length;)
        {
          const gchar *arg = content + offset;
          gsize arg_len = strnlen (arg, length - offset);

          if (arg_len == 0)
            {
              offset++;
              continue;
            }

          if (g_strcmp0 (arg, "codex") == 0
              || g_str_has_prefix (arg, "codex-")
              || g_strcmp0 (arg, "gemini") == 0
              || g_str_has_prefix (arg, "gemini-")
              || g_strcmp0 (arg, "agy") == 0
              || g_str_has_prefix (arg, "agy-"))
            {
              matches = TRUE;
              break;
            }

          {
            gchar *basename = g_path_get_basename (arg);
            matches = g_strcmp0 (basename, "codex") == 0
                      || g_str_has_prefix (basename, "codex-")
                      || g_strcmp0 (basename, "gemini") == 0
                      || g_str_has_prefix (basename, "gemini-")
                      || g_strcmp0 (basename, "agy") == 0
                      || g_str_has_prefix (basename, "agy-");
            g_free (basename);
            if (matches)
              break;
          }

          offset += arg_len + 1;
        }
    }
  g_clear_pointer (&content, g_free);
  g_clear_pointer (&path, g_free);
  if (matches)
    return TRUE;

  path = g_strdup_printf ("/proc/%d/comm", (gint) pid);
  if (g_file_get_contents (path, &content, &length, NULL) && length > 0)
    {
      g_strchomp (content);
      matches = g_strcmp0 (content, "codex") == 0
                || g_str_has_prefix (content, "codex-")
                || g_strcmp0 (content, "gemini") == 0
                || g_str_has_prefix (content, "gemini-")
                || g_strcmp0 (content, "agy") == 0
                || g_str_has_prefix (content, "agy-");
    }

  g_clear_pointer (&content, g_free);
  g_clear_pointer (&path, g_free);
  return matches;
}



typedef enum
{
  TERMINAL_FOREGROUND_OTHER,
  TERMINAL_FOREGROUND_CODEX,
  TERMINAL_FOREGROUND_TMUX
} TerminalForegroundApplication;



static TerminalForegroundApplication
terminal_widget_process_application (pid_t pid)
{
  gchar *path = g_strdup_printf ("/proc/%d/exe", (gint) pid);
  gchar *executable = g_file_read_link (path, NULL);
  gchar *basename = executable != NULL ? g_path_get_basename (executable) : NULL;
  gboolean is_tmux = g_strcmp0 (basename, "tmux") == 0
                     || g_strcmp0 (basename, "tmux (deleted)") == 0;

  g_free (basename);
  g_free (executable);
  g_free (path);

  /* Check the executable first: `tmux new-session codex` is still tmux,
   * and its active pane may later be running something other than Codex. */
  if (is_tmux)
    return TERMINAL_FOREGROUND_TMUX;
  if (terminal_widget_process_looks_like_codex (pid))
    return TERMINAL_FOREGROUND_CODEX;
  return TERMINAL_FOREGROUND_OTHER;
}



static TerminalForegroundApplication
terminal_widget_process_group_application (pid_t pgrp)
{
  GDir *proc_dir;
  const gchar *entry;
  TerminalForegroundApplication application = terminal_widget_process_application (pgrp);

  if (application != TERMINAL_FOREGROUND_OTHER)
    return application;

  proc_dir = g_dir_open ("/proc", 0, NULL);
  if (proc_dir == NULL)
    return TERMINAL_FOREGROUND_OTHER;

  while ((entry = g_dir_read_name (proc_dir)) != NULL)
    {
      gchar *end = NULL;
      gint64 value = g_ascii_strtoll (entry, &end, 10);
      pid_t pid;

      if (entry[0] == '\0'
          || end == NULL
          || end[0] != '\0'
          || value <= 0
          || value > G_MAXINT)
        continue;

      pid = (pid_t) value;
      if (pid == pgrp || getpgid (pid) != pgrp)
        continue;

      application = terminal_widget_process_application (pid);
      if (application != TERMINAL_FOREGROUND_OTHER)
        break;
    }

  g_dir_close (proc_dir);
  return application;
}



static TerminalForegroundApplication
terminal_widget_foreground_application (TerminalWidget *widget)
{
  VtePty *pty;
  gint pty_fd;
  pid_t pgrp;

  pty = vte_terminal_get_pty (VTE_TERMINAL (widget));
  if (!VTE_IS_PTY (pty))
    return TERMINAL_FOREGROUND_OTHER;

  pty_fd = vte_pty_get_fd (pty);
  if (pty_fd < 0)
    return TERMINAL_FOREGROUND_OTHER;

  pgrp = tcgetpgrp (pty_fd);
  if (pgrp <= 0)
    return TERMINAL_FOREGROUND_OTHER;

  /* Non-interactive wrapper shells can keep ownership of the foreground
   * process group while Codex runs as another member of the same job. */
  return terminal_widget_process_group_application (pgrp);
}



gboolean
terminal_widget_foreground_process_is_codex (TerminalWidget *widget)
{
  return terminal_widget_foreground_application (widget) == TERMINAL_FOREGROUND_CODEX;
}

gboolean
terminal_widget_copy_tmux_selection (TerminalWidget *widget)
{
  VtePty *pty = vte_terminal_get_pty (VTE_TERMINAL (widget));
  pid_t pgrp;
  gint fd;

  if (!VTE_IS_PTY (pty) || (fd = vte_pty_get_fd (pty)) < 0)
    return FALSE;
  pgrp = tcgetpgrp (fd);
  if (pgrp <= 0 || terminal_widget_process_application (pgrp) != TERMINAL_FOREGROUND_TMUX)
    return FALSE;
  return terminal_tmux_copy_selection (pgrp);
}



static gboolean
terminal_widget_process_matches_rule (const gchar *argument,
                                      const gchar *rule)
{
  gchar *basename;
  gchar *prefix;
  gboolean matches;

  if (rule == NULL || *rule == '\0')
    return FALSE;

  if (g_strcmp0 (rule, "*") == 0)
    return TRUE;

  basename = g_path_get_basename (argument);
  matches = g_strcmp0 (argument, rule) == 0 || g_strcmp0 (basename, rule) == 0;

  if (!matches)
    {
      prefix = g_strconcat (rule, "-", NULL);
      matches = g_str_has_prefix (basename, prefix);
      g_free (prefix);
    }

  g_free (basename);
  return matches;
}



static gboolean
terminal_widget_process_matches_allowlist (pid_t pid,
                                            const gchar *allowlist)
{
  gchar *path = NULL;
  gchar *content = NULL;
  gchar **rules = NULL;
  gsize length = 0;
  gboolean matches = FALSE;

  if (allowlist == NULL || *allowlist == '\0')
    return FALSE;

  rules = g_strsplit_set (allowlist, ";, \t\r\n", -1);
  path = g_strdup_printf ("/proc/%d/cmdline", (gint) pid);
  if (g_file_get_contents (path, &content, &length, NULL) && length > 0)
    {
      gchar *interpreter = NULL;
      gchar *script = NULL;
      const gchar *first_argument = content;
      gsize first_length = strnlen (first_argument, length);

      /* Match the executable, not arbitrary command arguments. This avoids
       * treating output such as `rg codex` as the foreground application. */
      if (first_length > 0)
        {
          for (guint i = 0; rules[i] != NULL; i++)
            {
              if (terminal_widget_process_matches_rule (first_argument, rules[i]))
                {
                  matches = TRUE;
                  break;
                }
            }
        }

      /* Node/Python-style launchers identify the real foreground app in the
       * next argument, e.g. `node /bin/codex`. Only inspect that argument for
       * known interpreters, never arbitrary arguments to native programs. */
      interpreter = g_path_get_basename (first_argument);
      if (!matches
          && (g_strcmp0 (interpreter, "node") == 0
              || g_strcmp0 (interpreter, "nodejs") == 0
              || g_strcmp0 (interpreter, "python") == 0
              || g_strcmp0 (interpreter, "python3") == 0
              || g_strcmp0 (interpreter, "ruby") == 0
              || g_strcmp0 (interpreter, "perl") == 0
              || g_strcmp0 (interpreter, "bun") == 0
              || g_strcmp0 (interpreter, "deno") == 0))
        {
          const gchar *script_start = first_argument + first_length + 1;
          if ((gsize) (script_start - content) < length)
            {
              gsize script_length = strnlen (script_start,
                                              length - (gsize) (script_start - content));
              if (script_length > 0)
                {
                  script = g_strndup (script_start, script_length);
                  for (guint i = 0; rules[i] != NULL; i++)
                    {
                      if (terminal_widget_process_matches_rule (script, rules[i]))
                        {
                          matches = TRUE;
                          break;
                        }
                    }
                }
            }
        }

      g_free (script);
      g_free (interpreter);
    }

  g_clear_pointer (&content, g_free);
  g_clear_pointer (&path, g_free);

  if (!matches)
    {
      path = g_strdup_printf ("/proc/%d/comm", (gint) pid);
      if (g_file_get_contents (path, &content, &length, NULL) && length > 0)
        {
          g_strchomp (content);
          for (guint i = 0; rules[i] != NULL; i++)
            {
              if (terminal_widget_process_matches_rule (content, rules[i]))
                {
                  matches = TRUE;
                  break;
                }
            }
        }
    }

  g_clear_pointer (&content, g_free);
  g_clear_pointer (&path, g_free);
  g_strfreev (rules);
  return matches;
}



static gboolean
terminal_widget_foreground_process_allows_path_detection (TerminalWidget *widget)
{
  VtePty *pty;
  gint pty_fd;
  pid_t pgrp;
  gboolean enabled;
  gchar *allowlist = NULL;
  gboolean matches;

  /* VTE stores screen text, not the PID that produced each cell. The only
   * reliable provenance available here is the current PTY foreground group. */
  g_object_get (G_OBJECT (widget->preferences),
                "misc-auto-detect-file-paths", &enabled,
                "misc-auto-detect-file-path-apps", &allowlist,
                NULL);
  if (!enabled)
    {
      g_free (allowlist);
      return FALSE;
    }

  pty = vte_terminal_get_pty (VTE_TERMINAL (widget));
  if (!VTE_IS_PTY (pty))
    {
      g_free (allowlist);
      return FALSE;
    }

  pty_fd = vte_pty_get_fd (pty);
  pgrp = pty_fd >= 0 ? tcgetpgrp (pty_fd) : -1;
  if (pgrp > 0 && terminal_widget_process_application (pgrp) == TERMINAL_FOREGROUND_TMUX)
    pgrp = terminal_tmux_pane_foreground_pid (pgrp);
  matches = pgrp > 0 && terminal_widget_process_matches_allowlist (pgrp, allowlist);
  g_free (allowlist);
  return matches;
}



static gchar *
terminal_widget_link_to_input (const gchar *uri,
                               PatternType type)
{
  gchar *filename;

  if (type != PATTERN_TYPE_FILE)
    return g_strdup (uri);

  filename = g_filename_from_uri (uri, NULL, NULL);
  if (filename != NULL)
    return filename;

  return g_strdup (uri);
}



static gchar *
terminal_widget_shell_quote_input (const gchar *text)
{
  gboolean needs_quote = FALSE;

  for (const gchar *p = text; *p != '\0'; p++)
    {
      if (g_ascii_isalnum ((guchar) *p)
          || strchr ("_+-./:@%=~", *p) != NULL)
        continue;

      needs_quote = TRUE;
      break;
    }

  if (!needs_quote)
    return g_strdup (text);

  /* Keep tilde expansion active while quoting unsafe characters in its
   * remainder, e.g. ~/'folder with spaces'. */
  if (g_str_has_prefix (text, "~/"))
    {
      gchar *quoted_tail = g_shell_quote (text + 2);
      gchar *quoted = g_strdup_printf ("~/%s", quoted_tail);

      g_free (quoted_tail);
      return quoted;
    }

  return g_shell_quote (text);
}



static gboolean
terminal_widget_link_matches_event (VteTerminal *terminal,
                                    GdkEvent *event,
                                    const gchar *link,
                                    gint tag,
                                    gboolean osc8)
{
  gint candidate_tag = -1;
  gchar *candidate = vte_terminal_hyperlink_check_event (terminal, event);
  gboolean matches;

  if (!osc8 && candidate == NULL)
    candidate = vte_terminal_match_check_event (terminal, event, &candidate_tag);
  matches = (osc8 || candidate_tag == tag) && g_strcmp0 (candidate, link) == 0;
  g_free (candidate);
  return matches;
}



static void
terminal_widget_select_link (TerminalWidget *widget,
                              GdkEventButton *event)
{
  VteTerminal *terminal = VTE_TERMINAL (widget);
  GtkWidget *gtk_widget = GTK_WIDGET (widget);
  GtkWidgetClass *parent = GTK_WIDGET_CLASS (terminal_widget_parent_class);
  GtkBorder padding;
  GdkEvent *probe, *motion;
  gchar *link;
  gboolean osc8;
  gint tag = -1, threshold;
  glong columns, cell_width, cell_height, height, row_offset;
  gint64 clicked, first, last, cells;
  gdouble scroll, first_x, first_y, last_x, last_y;

  if (event->type != GDK_2BUTTON_PRESS || event->button != 1
      || widget->mouse_selection == TERMINAL_MOUSE_SELECTION_APPLICATION
      || (event->state & gtk_accelerator_get_default_mod_mask ()) != 0)
    return;

  /* Use the displayed span, not word separators or the decoded target URI.
   * OSC 8 labels may contain spaces and have no resemblance to their target. */
  link = vte_terminal_hyperlink_check_event (terminal, (GdkEvent *) event);
  osc8 = link != NULL;
  if (!osc8)
    link = vte_terminal_match_check_event (terminal, (GdkEvent *) event, &tag);
  if (link == NULL)
    return;

  gtk_style_context_get_padding (gtk_widget_get_style_context (gtk_widget), GTK_STATE_FLAG_NORMAL, &padding);
  columns = vte_terminal_get_column_count (terminal);
  cell_width = vte_terminal_get_char_width (terminal);
  cell_height = vte_terminal_get_char_height (terminal);
  height = gtk_widget_get_allocated_height (gtk_widget) - padding.top - padding.bottom;
  if (columns <= 0 || cell_width <= 0 || cell_height <= 0 || height <= 0
      || event->x < padding.left || event->x >= padding.left + columns * cell_width
      || event->y < padding.top || event->y >= padding.top + height)
    {
      g_free (link);
      return;
    }

  scroll = gtk_adjustment_get_value (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (widget)));
#if VTE_CHECK_VERSION(0, 66, 0)
  if (!vte_terminal_get_scroll_unit_is_pixels (terminal))
#endif
    scroll *= cell_height;
  row_offset = (gint64) (MAX (scroll, 0) + 0.5) % cell_height;
  clicked = (gint64) ((event->y - padding.top + row_offset) / cell_height) * columns
            + (gint64) ((event->x - padding.left) / cell_width);
  cells = ((height + row_offset + cell_height - 1) / cell_height) * columns;
  first = last = clicked;
  probe = gdk_event_copy ((GdkEvent *) event);

  /* Only inspect neighbouring cells on a double click. No scrollback copying,
   * filesystem lookups, or new work in the output/hover hot paths. */
  for (gint direction = -1; direction <= 1; direction += 2)
    for (gint64 cell = clicked + direction; cell >= 0 && cell < cells; cell += direction)
      {
        probe->button.x = padding.left + (cell % columns + 0.5) * cell_width;
        probe->button.y = CLAMP (padding.top + (cell / columns + 0.5) * cell_height - row_offset,
                                padding.top, padding.top + height - 1);
        if (!terminal_widget_link_matches_event (terminal, probe, link, tag, osc8))
          break;
        if (direction < 0)
          first = cell;
        else
          last = cell;
      }

  first_x = padding.left + (first % columns + 0.1) * cell_width;
  last_x = padding.left + (last % columns + 0.9) * cell_width;
  first_y = CLAMP (padding.top + (first / columns + 0.5) * cell_height - row_offset,
                   padding.top, padding.top + height - 1);
  last_y = CLAMP (padding.top + (last / columns + 0.5) * cell_height - row_offset,
                  padding.top, padding.top + height - 1);

  /* VTE's GTK3 API has no range-selection setter. Drive its own character
   * selection, with Shift forcing local handling even under mouse tracking.
   * The real button release completes the gesture and publishes PRIMARY. */
  vte_terminal_unselect_all (terminal);
  probe->button.type = GDK_BUTTON_PRESS;
  probe->button.state = GDK_SHIFT_MASK;
  probe->button.x = first_x;
  probe->button.y = first_y;
  parent->button_press_event (gtk_widget, &probe->button);

  motion = gdk_event_new (GDK_MOTION_NOTIFY);
  motion->motion.window = g_object_ref (event->window);
  motion->motion.time = event->time;
  motion->motion.state = GDK_SHIFT_MASK | GDK_BUTTON1_MASK;
  gdk_event_set_device (motion, gdk_event_get_device ((GdkEvent *) event));
  g_object_get (gtk_widget_get_settings (gtk_widget), "gtk-dnd-drag-threshold", &threshold, NULL);
  /* Cross the threshold even for a one-character label, then select its end. */
  motion->motion.x = first_x + threshold + cell_width;
  motion->motion.y = first_y;
  parent->motion_notify_event (gtk_widget, &motion->motion);
  motion->motion.x = last_x;
  motion->motion.y = last_y;
  parent->motion_notify_event (gtk_widget, &motion->motion);

  widget->mouse_selection = TERMINAL_MOUSE_SELECTION_LOCAL;
  widget->link_selection_drag_pending = TRUE;
  widget->link_selection_click_x = event->x;
  widget->link_selection_click_y = event->y;
  gdk_event_free (motion);
  gdk_event_free (probe);
  g_free (link);
}



static gboolean
terminal_widget_event_matches_button_trigger (GdkEventButton *event,
                                              guint button,
                                              guint modifier)
{
  const GdkModifierType mask = gtk_accelerator_get_default_mod_mask ();

  if (button == 0)
    return FALSE;

  return event->button == button
         && (event->state & mask) == (modifier & mask);
}



static TerminalHyperlink
terminal_widget_get_link_with_ctrl_fallback (TerminalWidget *widget,
                                             GdkEventButton *event)
{
  TerminalHyperlink link = terminal_widget_get_link (widget, (GdkEvent *) event);

  if (link.uri == NULL)
    {
      GdkEvent *event_with_ctrl;

      event_with_ctrl = gdk_event_copy ((GdkEvent *) event);
      ((GdkEventButton *) event_with_ctrl)->state |= GDK_CONTROL_MASK;
      link = terminal_widget_get_link (widget, event_with_ctrl);
      gdk_event_free (event_with_ctrl);
    }

  return link;
}



static void
terminal_widget_feed_link_to_child (TerminalWidget *widget,
                                    const gchar *uri,
                                    PatternType type)
{
  gchar *directory_prefix = NULL;
  gchar *directory_suffix = NULL;
  gchar *payload = NULL;
  gchar *quoted_text = NULL;
  gchar *text;

  g_object_get (G_OBJECT (widget->preferences),
                "misc-hyperlink-directory-prefix", &directory_prefix,
                "misc-hyperlink-directory-suffix", &directory_suffix,
                NULL);

  text = terminal_widget_link_to_input (uri, type);
  if (G_LIKELY (text != NULL && *text != '\0'))
    {
      quoted_text = terminal_widget_shell_quote_input (text);

      if (type == PATTERN_TYPE_FILE
          && terminal_widget_link_clickable (uri, type)
          && g_file_test (text, G_FILE_TEST_IS_DIR))
        payload = g_strconcat (directory_prefix != NULL ? directory_prefix : "",
                               quoted_text,
                               directory_suffix != NULL ? directory_suffix : "",
                               NULL);

      vte_terminal_feed_child (VTE_TERMINAL (widget),
                               payload != NULL ? payload : quoted_text,
                               payload != NULL ? strlen (payload) : strlen (quoted_text));
    }

  g_free (directory_prefix);
  g_free (directory_suffix);
  g_free (payload);
  g_free (quoted_text);
  g_free (text);
}



static gboolean
terminal_widget_click_insert_link_from_event (TerminalWidget *widget,
                                              GdkEventButton *event)
{
  TerminalHyperlink link = terminal_widget_get_link_with_ctrl_fallback (widget, event);

  if (G_UNLIKELY (link.uri != NULL))
    {
      terminal_widget_feed_link_to_child (widget, link.uri, link.type);
      g_free (link.uri);
      return TRUE;
    }

  /* Fallback for URIs that VTE can detect but don't match our pattern table. */
  {
    gchar *uri = vte_terminal_hyperlink_check_event (VTE_TERMINAL (widget), (GdkEvent *) event);

    if (uri == NULL)
      {
        gint tag = -1;

        uri = vte_terminal_match_check_event (VTE_TERMINAL (widget), (GdkEvent *) event, &tag);

        if (terminal_widget_regex_tag_is_path (widget, tag))
          {
            g_free (uri);
            uri = NULL;
          }
      }

    if (uri != NULL)
      {
        PatternType type = g_str_has_prefix (uri, "file://") ? PATTERN_TYPE_FILE : PATTERN_TYPE_NONE;

        if (type == PATTERN_TYPE_FILE
            && !terminal_widget_link_clickable (uri, type))
          {
            g_free (uri);
            return FALSE;
          }

        terminal_widget_feed_link_to_child (widget, uri, type);
        g_free (uri);
        return TRUE;
      }
  }

  return FALSE;
}



static gboolean
terminal_widget_click_open_link_from_event (TerminalWidget *widget,
                                            GdkEventButton *event)
{
  TerminalHyperlink link = terminal_widget_get_link_with_ctrl_fallback (widget, event);

  if (G_UNLIKELY (link.uri != NULL))
    {
      if (link.type == PATTERN_TYPE_PATH)
        {
          gboolean opened = terminal_widget_open_path_candidate (widget, link.uri,
                                                                   event->time, TRUE);
          g_free (link.uri);
          return opened;
        }
      else if (terminal_widget_link_clickable (link.uri, link.type))
        {
          terminal_widget_open_uri (widget, link.uri, link.type, event->time);
          g_free (link.uri);
          return TRUE;
        }

      g_free (link.uri);
    }

  /* Fallback for URIs that VTE can detect but don't match our pattern table. */
  {
    gchar *uri = vte_terminal_hyperlink_check_event (VTE_TERMINAL (widget), (GdkEvent *) event);

    if (uri == NULL)
      {
        gint tag = -1;

        uri = vte_terminal_match_check_event (VTE_TERMINAL (widget), (GdkEvent *) event, &tag);

        if (terminal_widget_regex_tag_is_path (widget, tag))
          {
            if (uri != NULL
                && terminal_widget_open_path_candidate (widget, uri, event->time, TRUE))
              {
                g_free (uri);
                return TRUE;
              }

            g_free (uri);
            uri = NULL;
          }
      }

    if (uri != NULL)
      {
        PatternType type = g_str_has_prefix (uri, "file://") ? PATTERN_TYPE_FILE : PATTERN_TYPE_NONE;

        if (terminal_widget_link_clickable (uri, type))
          {
            terminal_widget_open_uri (widget, uri, type, event->time);
            g_free (uri);
            return TRUE;
          }

        g_free (uri);
      }
  }

  if (terminal_widget_event_is_ctrl_left_click (event))
    {
      gchar *selected_uri = terminal_widget_selected_existing_path_uri (widget);

      if (selected_uri != NULL)
        {
          terminal_widget_open_uri (widget, selected_uri, PATTERN_TYPE_FILE, event->time);
          g_free (selected_uri);
          return TRUE;
        }

      {
        gchar *selection = vte_terminal_get_text_selected (VTE_TERMINAL (widget), VTE_FORMAT_TEXT);

        if (selection != NULL
            && terminal_widget_open_path_candidate (widget, selection, event->time, FALSE))
          {
            g_free (selection);
            return TRUE;
          }

        g_free (selection);
      }
    }

  return FALSE;
}



static gboolean
terminal_widget_event_is_ctrl_shift_left_click (GdkEventButton *event)
{
  const GdkModifierType mask = gtk_accelerator_get_default_mod_mask ();

  return event->button == 1
         && (event->state & mask) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}



static gboolean
terminal_widget_click_open_parent_from_event (TerminalWidget *widget,
                                              GdkEventButton *event)
{
  TerminalHyperlink link = terminal_widget_get_link_with_ctrl_fallback (widget, event);

  if (link.uri != NULL)
    {
      if (link.type == PATTERN_TYPE_PATH)
        {
          gboolean opened = terminal_widget_open_parent_path_candidate (widget,
                                                                          link.uri,
                                                                          event->time,
                                                                          FALSE);
          g_free (link.uri);
          return opened;
        }

      if (link.type == PATTERN_TYPE_FILE
          && terminal_widget_link_clickable (link.uri, link.type)
          && terminal_widget_open_parent_path_candidate (widget, link.uri, event->time, FALSE))
        {
          g_free (link.uri);
          return TRUE;
        }

      g_free (link.uri);
    }

  {
    gchar *uri = vte_terminal_hyperlink_check_event (VTE_TERMINAL (widget), (GdkEvent *) event);

    if (uri == NULL)
      {
        gint tag = -1;

        uri = vte_terminal_match_check_event (VTE_TERMINAL (widget), (GdkEvent *) event, &tag);
        if (terminal_widget_regex_tag_is_path (widget, tag)
            && uri != NULL
            && terminal_widget_open_parent_path_candidate (widget, uri, event->time, TRUE))
          {
            g_free (uri);
            return TRUE;
          }
      }

    if (uri != NULL)
      {
        PatternType type = g_str_has_prefix (uri, "file://") ? PATTERN_TYPE_FILE : PATTERN_TYPE_NONE;

        if (type == PATTERN_TYPE_FILE
            && terminal_widget_link_clickable (uri, type)
            && terminal_widget_open_parent_path_candidate (widget, uri, event->time, FALSE))
          {
            g_free (uri);
            return TRUE;
          }

        g_free (uri);
      }
  }

  {
    gchar *selection = vte_terminal_get_text_selected (VTE_TERMINAL (widget), VTE_FORMAT_TEXT);

    if (selection != NULL
        && terminal_widget_open_parent_path_candidate (widget, selection, event->time, FALSE))
      {
        g_free (selection);
        return TRUE;
      }

    g_free (selection);
  }

  return FALSE;
}



static gboolean
terminal_widget_event_is_ctrl_left_click (GdkEventButton *event)
{
  const GdkModifierType mask = gtk_accelerator_get_default_mod_mask ();

  return event->button == 1
         && (event->state & mask) == GDK_CONTROL_MASK;
}



static gchar *
terminal_widget_get_current_directory_path (TerminalWidget *widget)
{
  const gchar *cwd_uri;
  gchar *cwd_path = NULL;
  gboolean tmux_pane = FALSE;
  VtePty *pty;
  gint pty_fd;
  pid_t pgrp;

  pty = vte_terminal_get_pty (VTE_TERMINAL (widget));
  if (VTE_IS_PTY (pty))
    {
      pty_fd = vte_pty_get_fd (pty);
      if (pty_fd >= 0)
        {
          pgrp = tcgetpgrp (pty_fd);
          if (pgrp > 0 && terminal_widget_process_application (pgrp) == TERMINAL_FOREGROUND_TMUX)
            {
              tmux_pane = TRUE;
              pgrp = terminal_tmux_pane_foreground_pid (pgrp);
              if (pgrp <= 0)
                return NULL;
            }
          if (pgrp > 0)
            {
              gchar *proc_cwd = g_strdup_printf ("/proc/%d/cwd", (gint) pgrp);
              cwd_path = g_file_read_link (proc_cwd, NULL);
              g_free (proc_cwd);

              if (cwd_path != NULL && !g_file_test (cwd_path, G_FILE_TEST_IS_DIR))
                g_clear_pointer (&cwd_path, g_free);
            }
        }
    }

  if (cwd_path != NULL || tmux_pane)
    return cwd_path;

  /* VTE's reported URI is useful during shell startup, but must not override
   * a valid foreground process CWD with stale shell metadata. */
  cwd_uri = vte_terminal_get_current_directory_uri (VTE_TERMINAL (widget));
  if (cwd_uri != NULL)
    {
      cwd_path = g_filename_from_uri (cwd_uri, NULL, NULL);
      if (cwd_path != NULL && g_file_test (cwd_path, G_FILE_TEST_IS_DIR))
        return cwd_path;
      g_clear_pointer (&cwd_path, g_free);
    }

  /* Relative path resolution must fail closed when the PTY no longer has a
   * trustworthy foreground working directory. */
  return NULL;
}



static gchar *
terminal_widget_selected_existing_path_uri (TerminalWidget *widget)
{
  gchar *selection = NULL;
  gchar *candidate = NULL;
  gchar *path = NULL;
  gchar *uri = NULL;
  gsize len;

  if (!vte_terminal_get_has_selection (VTE_TERMINAL (widget)))
    return NULL;

  selection = vte_terminal_get_text_selected (VTE_TERMINAL (widget), VTE_FORMAT_TEXT);
  if (selection == NULL)
    return NULL;

  candidate = g_strstrip (selection);
  if (*candidate == '\0' || strchr (candidate, '\n') != NULL || strchr (candidate, '\r') != NULL)
    goto out;

  len = strlen (candidate);
  if (len > 4096)
    goto out;
  while (len >= 2
         && ((candidate[0] == '"' && candidate[len - 1] == '"')
             || (candidate[0] == '\'' && candidate[len - 1] == '\'')
             || (candidate[0] == '<' && candidate[len - 1] == '>')
             || (candidate[0] == '(' && candidate[len - 1] == ')')))
    {
      candidate[len - 1] = '\0';
      candidate++;
      len -= 2;
    }

  if (*candidate == '\0')
    goto out;

  if (g_str_has_prefix (candidate, "file://"))
    {
      gchar *filename = NULL;
      gchar *hostname = NULL;

      filename = g_filename_from_uri (candidate, &hostname, NULL);
      if (filename != NULL
          && terminal_widget_link_clickable (candidate, PATTERN_TYPE_FILE)
          && g_file_test (filename, G_FILE_TEST_EXISTS))
        uri = g_filename_to_uri (filename, NULL, NULL);

      g_free (filename);
      g_free (hostname);
      goto out;
    }

  if (g_strcmp0 (candidate, "~") == 0)
    {
      path = g_strdup (g_get_home_dir ());
    }
  else if (g_str_has_prefix (candidate, "~/"))
    {
      path = g_build_filename (g_get_home_dir (), candidate + 2, NULL);
    }
  else if (g_path_is_absolute (candidate))
    {
      path = g_strdup (candidate);
    }
  else
    {
      gchar *cwd_path = NULL;

      cwd_path = terminal_widget_get_current_directory_path (widget);
      if (cwd_path != NULL)
        path = g_build_filename (cwd_path, candidate, NULL);

      g_free (cwd_path);
    }

  if (path != NULL)
    {
      if (g_file_test (path, G_FILE_TEST_EXISTS))
        uri = g_filename_to_uri (path, NULL, NULL);
    }

out:
  g_free (path);
  g_free (selection);
  return uri;
}



static gchar *
terminal_widget_normalize_path_candidate (const gchar *candidate,
                                          gboolean *was_wrapped)
{
  GString *normalized;
  gboolean previous_was_wrap = FALSE;

  normalized = g_string_sized_new (strlen (candidate));
  *was_wrapped = FALSE;

  for (const gchar *p = candidate; *p != '\0'; p++)
    {
      if (previous_was_wrap && (*p == ' ' || *p == '\t'))
        continue;

      if (*p == '\r' || *p == '\n')
        {
          if (*p == '\r')
            {
              if (p[1] != '\n')
                {
                  g_string_free (normalized, TRUE);
                  return NULL;
                }
              p++;
            }

          if (normalized->len == 0
              || normalized->str[normalized->len - 1] != '/'
              || previous_was_wrap)
            {
              g_string_free (normalized, TRUE);
              return NULL;
            }

          *was_wrapped = TRUE;
          previous_was_wrap = TRUE;
          continue;
        }

      if ((guchar) *p < 0x20 || *p == 0x7f)
        {
          g_string_free (normalized, TRUE);
          return NULL;
        }

      g_string_append_c (normalized, *p);
      previous_was_wrap = FALSE;
    }

  return g_string_free (normalized, FALSE);
}



static gchar *
terminal_widget_candidate_to_path (TerminalWidget *widget,
                                   const gchar *candidate,
                                   gboolean require_allowlist)
{
  gchar *cwd_path = NULL;
  gchar *raw_candidate = NULL;
  gchar *result = NULL;
  gssize end;
  gboolean was_wrapped;

  if ((require_allowlist
       && !terminal_widget_foreground_process_allows_path_detection (widget))
      || candidate == NULL
      || *candidate == '\0'
      || strlen (candidate) > 4096)
    return NULL;

  raw_candidate = terminal_widget_normalize_path_candidate (candidate, &was_wrapped);
  if (raw_candidate == NULL)
    return NULL;

  if (g_str_has_prefix (raw_candidate, "file://"))
    {
      if (terminal_widget_link_clickable (raw_candidate, PATTERN_TYPE_FILE))
        result = g_filename_from_uri (raw_candidate, NULL, NULL);
      goto out;
    }

  cwd_path = terminal_widget_get_current_directory_path (widget);

  /* A selection can contain a path followed by a command argument, for
   * example `foo.cpp --line 20`. Keep quoted paths intact, but remove an
   * obvious option suffix before starting an Unearth search. */
  if (raw_candidate[0] != '"' && raw_candidate[0] != '\''
      && raw_candidate[0] != '<' && raw_candidate[0] != '(')
    {
      gchar *separator = strpbrk (raw_candidate, " \t");

      if (separator != NULL
          && separator[1] == '-'
          && !g_file_test (raw_candidate, G_FILE_TEST_EXISTS))
        *separator = '\0';
    }

  end = (gssize) strlen (raw_candidate);
  while (end > 0 && result == NULL)
    {
      gchar *piece;
      gchar *trimmed;
      gchar *path = NULL;
      gsize length;

      piece = g_strndup (raw_candidate, (gsize) end);
      trimmed = g_strstrip (piece);
      length = strlen (trimmed);

      while (length >= 2
             && ((trimmed[0] == '"' && trimmed[length - 1] == '"')
                 || (trimmed[0] == '\'' && trimmed[length - 1] == '\'')
                 || (trimmed[0] == '<' && trimmed[length - 1] == '>')
                 || (trimmed[0] == '(' && trimmed[length - 1] == ')')))
        {
          trimmed[length - 1] = '\0';
          trimmed++;
          length -= 2;
        }

      while (length > 0
             && strchr (".,;:!?]}", trimmed[length - 1]) != NULL)
        trimmed[--length] = '\0';

      if (g_strcmp0 (trimmed, "~") == 0)
        {
          path = g_strdup (g_get_home_dir ());
        }
      else if (g_str_has_prefix (trimmed, "~/"))
        {
          path = g_build_filename (g_get_home_dir (), trimmed + 2, NULL);
        }
      else if (g_path_is_absolute (trimmed))
        {
          path = g_strdup (trimmed);
        }
      else if (*trimmed != '\0' && cwd_path != NULL)
        {
          path = g_build_filename (cwd_path, trimmed, NULL);
        }

      if (path != NULL)
        {
          result = path;
          path = NULL;
        }

      g_free (path);
      g_free (piece);

      while (result == NULL && end > 0 && g_ascii_isspace (raw_candidate[end - 1]))
        end--;
      while (result == NULL && end > 0 && !g_ascii_isspace (raw_candidate[end - 1]))
        end--;
    }

out:
  /* A hard newline after slash is indistinguishable from a visual wrap in
   * VTE's regex input. Do not turn such text into an Unearth query. */
  if (was_wrapped
      && result != NULL
      && !g_file_test (result, G_FILE_TEST_EXISTS))
    g_clear_pointer (&result, g_free);

  g_free (cwd_path);
  g_free (raw_candidate);
  return result;
}



static gboolean
terminal_widget_open_parent_selection_uri (TerminalWidget *widget,
                                           const gchar *uri)
{
  GtkWidget *toplevel;
  GtkWindow *window = NULL;
  GFile *file = NULL;
  GFile *parent = NULL;
  gchar *path = NULL;
  gchar *parent_uri = NULL;
  gchar *escaped_path = NULL;
  gchar *selection_uri = NULL;
  gboolean result = FALSE;

  if (!g_str_has_prefix (uri, "file://")
      || !terminal_widget_link_clickable (uri, PATTERN_TYPE_FILE))
    return FALSE;

  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  if (path == NULL || !g_file_test (path, G_FILE_TEST_EXISTS))
    goto out;

  parent = g_file_get_parent (file);
  if (parent == NULL)
    goto out;

  parent_uri = g_file_get_uri (parent);
  escaped_path = g_uri_escape_string (path, "/", FALSE);
  selection_uri = g_strdup_printf (g_str_has_suffix (parent_uri, "/")
                                     ? "%s?select=%s"
                                     : "%s/?select=%s",
                                   parent_uri, escaped_path);

  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (widget));
  if (GTK_IS_WINDOW (toplevel))
    window = GTK_WINDOW (toplevel);
  result = terminal_widget_open_pcmanfm_selection_uri (window, selection_uri, widget);

out:
  g_clear_object (&parent);
  g_clear_object (&file);
  g_free (selection_uri);
  g_free (escaped_path);
  g_free (parent_uri);
  g_free (path);
  return result;
}



static void
terminal_widget_unearth_search_finished (GObject *source_object,
                                         GAsyncResult *result,
                                         gpointer user_data)
{
  TerminalUnearthSearch *search = user_data;
  TerminalWidget *widget;
  GBytes *stdout_bytes = NULL;
  GBytes *stderr_bytes = NULL;
  const guint8 *data = NULL;
  gsize data_length = 0;
  guint match_count = 0;
  gchar *match_path = NULL;
  GError *error = NULL;
  gboolean current_search;

  g_subprocess_communicate_finish (G_SUBPROCESS (source_object), result,
                                   &stdout_bytes, &stderr_bytes, &error);
  widget = g_weak_ref_get (&search->widget_ref);
  current_search = widget != NULL && widget->unearth_search == search;

  if (current_search
      && !g_cancellable_is_cancelled (search->cancellable)
      && error == NULL
      && stdout_bytes != NULL
      && g_subprocess_get_successful (G_SUBPROCESS (source_object)))
    {
      data = g_bytes_get_data (stdout_bytes, &data_length);
      for (gsize offset = 0; offset < data_length && match_count < 2;)
        {
          const guint8 *newline = memchr (data + offset, '\n', data_length - offset);
          gsize line_length = newline != NULL
                                ? (gsize) (newline - (data + offset))
                                : data_length - offset;

          if (line_length > 0 && data[offset + line_length - 1] == '\r')
            line_length--;

          /* Unearth's textual protocol is newline-delimited, but filenames
           * themselves may contain non-UTF-8 bytes. The lossless output mode
           * encodes those bytes as %XX, so compare encoded records first and
           * decode only after an exact candidate has been selected. */
          if (line_length > 0
              && g_strstr_len ((const gchar *) (data + offset), line_length,
                               search->needle) != NULL)
            {
              gchar *encoded_match = g_strndup ((const gchar *) (data + offset), line_length);
              gchar *match = terminal_widget_decode_unearth_path (encoded_match,
                                                                   line_length);

              if (g_path_is_absolute (match)
                  && g_file_test (match, G_FILE_TEST_EXISTS))
                {
                  match_count++;
                  if (match_count == 1)
                    match_path = g_strdup (match);
                }

              g_free (encoded_match);
              g_free (match);
            }

          if (newline == NULL)
            break;
          offset += line_length + (data[offset + line_length] == '\r' ? 2 : 1);
        }

      if (match_count == 1 && match_path != NULL)
        {
          gchar *uri = g_filename_to_uri (match_path, NULL, NULL);

          if (uri != NULL)
            {
              if (search->select_parent)
                terminal_widget_open_parent_selection_uri (widget, uri);
              else
                terminal_widget_open_uri (widget, uri,
                                          PATTERN_TYPE_FILE,
                                          search->event_time);
              g_free (uri);
            }
        }
    }

  if (error != NULL)
    g_error_free (error);
  g_clear_pointer (&stdout_bytes, g_bytes_unref);
  g_clear_pointer (&stderr_bytes, g_bytes_unref);
  g_free (match_path);
  if (current_search)
    widget->unearth_search = NULL;
  g_clear_object (&search->cancellable);
  g_free (search->key);
  g_free (search->needle);
  g_clear_object (&widget);
  g_weak_ref_clear (&search->widget_ref);
  g_free (search);
}



static gchar *
terminal_widget_resolve_python_test_id (const gchar *path)
{
  gchar *directory = NULL;
  gchar *basename = NULL;
  gchar **components = NULL;
  gchar *module_path = NULL;
  guint component_count = 0;
  gint class_index = -1;
  GString *candidate = NULL;

  if (!g_path_is_absolute (path))
    return NULL;

  directory = g_path_get_dirname (path);
  basename = g_path_get_basename (path);
  components = g_strsplit (basename, ".", -1);
  component_count = g_strv_length (components);
  if (component_count < 3
      || !g_str_has_prefix (components[component_count - 1], "test_"))
    goto out;

  /* Resolve only the conventional module.Class.test_method shape. */
  for (guint i = 1; i + 1 < component_count; i++)
    {
      if (components[i][0] != '\0' && g_ascii_isupper (components[i][0]))
        {
          class_index = (gint) i;
          break;
        }
    }
  if (class_index < 1)
    goto out;

  candidate = g_string_new (directory);
  for (gint i = 0; i < class_index; i++)
    {
      g_string_append_c (candidate, G_DIR_SEPARATOR);
      g_string_append (candidate, components[i]);
    }
  g_string_append (candidate, ".py");

  if (g_file_test (candidate->str, G_FILE_TEST_IS_REGULAR))
    module_path = g_string_free (g_steal_pointer (&candidate), FALSE);

out:
  if (candidate != NULL)
    g_string_free (candidate, TRUE);
  g_strfreev (components);
  g_free (basename);
  g_free (directory);
  return module_path;
}



static gchar *
terminal_widget_encode_unearth_path (const gchar *path)
{
  const guint8 *bytes = (const guint8 *) path;
  const guint8 *end = bytes + strlen (path);
  GString *encoded = g_string_sized_new ((gsize) (end - bytes));

  while (bytes < end)
    {
      if (*bytes == '/' || (*bytes >= 0x20 && *bytes < 0x7f && *bytes != '%'))
        {
          g_string_append_c (encoded, (gchar) *bytes++);
        }
      else if (*bytes == '%')
        {
          g_string_append (encoded, "%25");
          bytes++;
        }
      else if (*bytes >= 0x80)
        {
          gunichar character;
          gchar utf8[6];
          gint utf8_length;

          character = g_utf8_get_char_validated ((const gchar *) bytes,
                                                 (gssize) (end - bytes));
          if (character != (gunichar) -1 && character != (gunichar) -2)
            {
              utf8_length = g_unichar_to_utf8 (character, utf8);
              g_string_append_len (encoded, utf8, utf8_length);
              bytes += utf8_length;
            }
          else
            {
              g_string_append_printf (encoded, "%%%02X", *bytes++);
            }
        }
      else
        {
          g_string_append_printf (encoded, "%%%02X", *bytes++);
        }
    }

  return g_string_free (encoded, FALSE);
}



static gchar *
terminal_widget_decode_unearth_path (const gchar *path,
                                     gsize length)
{
  const guint8 *bytes = (const guint8 *) path;
  GString *decoded = g_string_sized_new (length);

  for (gsize i = 0; i < length; i++)
    {
      if (bytes[i] == '%' && i + 2 < length
          && g_ascii_isxdigit (bytes[i + 1])
          && g_ascii_isxdigit (bytes[i + 2]))
        {
          guint8 value = (guint8) ((g_ascii_xdigit_value (bytes[i + 1]) << 4)
                                   | g_ascii_xdigit_value (bytes[i + 2]));
          g_string_append_c (decoded, (gchar) value);
          i += 2;
        }
      else
        {
          g_string_append_c (decoded, (gchar) bytes[i]);
        }
    }

  return g_string_free (decoded, FALSE);
}



static gboolean
terminal_widget_start_unearth_search_for_path (TerminalWidget *widget,
                                               const gchar *path,
                                               guint32 event_time,
                                               gboolean select_parent)
{
  GSubprocessLauncher *launcher = NULL;
  GSubprocess *subprocess = NULL;
  TerminalUnearthSearch *search = NULL;
  gchar *unearth = NULL;
  gchar *cwd = NULL;
  gchar *canonical_cwd = NULL;
  gchar *canonical_path = NULL;
  gchar *basename = NULL;
  gchar *escaped = NULL;
  gchar *pattern = NULL;
  gchar *search_part = NULL;
  gchar *encoded_search_part = NULL;
  gchar *key = NULL;
  gchar *python_module = NULL;
  const gchar *suffix = NULL;
  GError *error = NULL;
  gboolean started = FALSE;

  if (path == NULL || *path == '\0')
    return FALSE;

  cwd = terminal_widget_get_current_directory_path (widget);
  if (cwd == NULL)
    goto out;

  canonical_cwd = g_canonicalize_filename (cwd, NULL);
  canonical_path = g_canonicalize_filename (path, NULL);

  python_module = terminal_widget_resolve_python_test_id (canonical_path);
  if (python_module != NULL)
    {
      gchar *uri = g_filename_to_uri (python_module, NULL, NULL);

      if (widget->unearth_search != NULL)
        g_cancellable_cancel (widget->unearth_search->cancellable);

      if (uri != NULL)
        {
          if (select_parent)
            terminal_widget_open_parent_selection_uri (widget, uri);
          else
            terminal_widget_open_uri (widget, uri, PATTERN_TYPE_FILE, event_time);
          g_free (uri);
          started = TRUE;
        }
      goto out;
    }

  key = g_strdup_printf ("%d:%s", select_parent ? 1 : 0, canonical_path);
  if (widget->unearth_search != NULL
      && !g_cancellable_is_cancelled (widget->unearth_search->cancellable)
      && g_strcmp0 (widget->unearth_search->key, key) == 0)
    {
      started = TRUE;
      goto out;
    }
  if (widget->unearth_search != NULL)
    g_cancellable_cancel (widget->unearth_search->cancellable);

  if (g_str_has_prefix (canonical_path, canonical_cwd)
      && (canonical_path[strlen (canonical_cwd)] == '\0'
          || canonical_path[strlen (canonical_cwd)] == G_DIR_SEPARATOR))
    {
      suffix = canonical_path + strlen (canonical_cwd);
      /* Match the same literal substring that the fish `f` helper searches
       * for, rather than requiring an exact path suffix. */
      search_part = g_strdup (suffix + (suffix[0] == G_DIR_SEPARATOR));
    }
  else
    {
      basename = g_path_get_basename (canonical_path);
      search_part = g_strdup (basename);
    }

  encoded_search_part = terminal_widget_encode_unearth_path (search_part);
  escaped = g_regex_escape_string (encoded_search_part, -1);
  pattern = g_strdup (escaped);

  unearth = g_find_program_in_path ("unearth");
  if (unearth == NULL)
    {
      gchar *local_unearth = g_build_filename (g_get_home_dir (), ".local", "bin", "unearth", NULL);

      if (g_file_test (local_unearth, G_FILE_TEST_IS_EXECUTABLE))
        unearth = local_unearth;
      else
        g_free (local_unearth);
    }
  if (unearth == NULL)
    goto out;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                        | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  subprocess = g_subprocess_launcher_spawn (launcher, &error,
                                             unearth,
                                             "--index-if-watched",
                                             "--hidden",
                                             "--full",
                                             "--regex", pattern,
                                             "--absolute-paths",
                                             "--case-sensitive",
                                             "--lossless-paths",
                                             "--color=never",
                                             "--timeout", "2",
                                             cwd,
                                             NULL);
  if (subprocess == NULL)
    goto out;

  search = g_new0 (TerminalUnearthSearch, 1);
  g_weak_ref_init (&search->widget_ref, widget);
  search->event_time = event_time;
  search->select_parent = select_parent;
  search->cancellable = g_cancellable_new ();
  search->key = g_steal_pointer (&key);
  search->needle = g_strdup (encoded_search_part);
  widget->unearth_search = search;
  g_subprocess_communicate_async (subprocess, NULL, search->cancellable,
                                  terminal_widget_unearth_search_finished,
                                  search);
  search = NULL;
  started = TRUE;

out:
  if (error != NULL)
    g_error_free (error);
  g_clear_object (&subprocess);
  g_clear_object (&launcher);
  g_free (unearth);
  g_free (pattern);
  g_free (search_part);
  g_free (encoded_search_part);
  g_free (escaped);
  g_free (basename);
  g_free (canonical_path);
  g_free (canonical_cwd);
  g_free (cwd);
  g_free (python_module);
  g_free (key);
  if (search != NULL)
    {
      g_clear_object (&search->cancellable);
      g_free (search->key);
      g_free (search->needle);
      g_weak_ref_clear (&search->widget_ref);
      g_free (search);
    }
  return started;
}



static gboolean
terminal_widget_open_path_candidate (TerminalWidget *widget,
                                     const gchar *candidate,
                                     guint32 event_time,
                                     gboolean require_allowlist)
{
  gchar *path;
  gchar *uri;
  gboolean opened;

  path = terminal_widget_candidate_to_path (widget, candidate, require_allowlist);
  if (path == NULL)
    return FALSE;

  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      uri = g_filename_to_uri (path, NULL, NULL);
      if (uri == NULL)
        {
          g_free (path);
          return FALSE;
        }

      terminal_widget_open_uri (widget, uri, PATTERN_TYPE_FILE, event_time);
      g_free (uri);
      g_free (path);
      return TRUE;
    }

  opened = terminal_widget_start_unearth_search_for_path (widget, path, event_time, FALSE);
  g_free (path);
  return opened;
}



static gboolean
terminal_widget_open_parent_path_candidate (TerminalWidget *widget,
                                            const gchar *candidate,
                                            guint32 event_time,
                                            gboolean require_allowlist)
{
  gchar *path;
  gchar *uri;
  gboolean opened;

  if (g_str_has_prefix (candidate, "file://")
      && !terminal_widget_link_clickable (candidate, PATTERN_TYPE_FILE))
    return FALSE;

  path = terminal_widget_candidate_to_path (widget, candidate, require_allowlist);
  if (path == NULL)
    return FALSE;

  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      uri = g_filename_to_uri (path, NULL, NULL);
      if (uri == NULL)
        {
          g_free (path);
          return FALSE;
        }

      opened = terminal_widget_open_parent_selection_uri (widget, uri);
      g_free (uri);
      g_free (path);
      return opened;
    }

  opened = terminal_widget_start_unearth_search_for_path (widget, path, event_time, TRUE);
  g_free (path);
  return opened;
}



static guint
terminal_widget_selection_modifiers (TerminalWidget *widget,
                                      guint state)
{
  if (widget->mouse_selection == TERMINAL_MOUSE_SELECTION_LOCAL)
    return state | GDK_SHIFT_MASK;
  if (widget->mouse_selection == TERMINAL_MOUSE_SELECTION_APPLICATION)
    return state & ~GDK_SHIFT_MASK;
  return state;
}



static gboolean
terminal_widget_button_release_event (GtkWidget *widget,
                                      GdkEventButton *event)
{
  TerminalWidget *terminal_widget = TERMINAL_WIDGET (widget);
  GdkEvent *forwarded;
  gboolean handled;

  if (event->button == 1)
    terminal_widget->link_selection_drag_pending = FALSE;

  if (event->button != 1 || terminal_widget->mouse_selection == TERMINAL_MOUSE_SELECTION_DEFAULT)
    return GTK_WIDGET_CLASS (terminal_widget_parent_class)->button_release_event (widget, event);

  forwarded = gdk_event_copy ((GdkEvent *) event);
  forwarded->button.state = terminal_widget_selection_modifiers (terminal_widget, event->state);
  terminal_widget->mouse_selection = TERMINAL_MOUSE_SELECTION_DEFAULT;
  handled = GTK_WIDGET_CLASS (terminal_widget_parent_class)->button_release_event (widget, &forwarded->button);
  gdk_event_free (forwarded);
  return handled;
}



static gboolean
terminal_widget_motion_notify_event (GtkWidget *widget,
                                     GdkEventMotion *event)
{
  TerminalWidget *terminal_widget = TERMINAL_WIDGET (widget);
  GdkEvent *forwarded;
  gboolean handled;

  if ((event->state & GDK_BUTTON1_MASK) && terminal_widget->link_selection_drag_pending)
    {
      if (!gtk_drag_check_threshold (widget,
                                     terminal_widget->link_selection_click_x,
                                     terminal_widget->link_selection_click_y,
                                     event->x, event->y))
        return TRUE;
      terminal_widget->link_selection_drag_pending = FALSE;
    }

  if (!(event->state & GDK_BUTTON1_MASK)
      || terminal_widget->mouse_selection == TERMINAL_MOUSE_SELECTION_DEFAULT)
    return GTK_WIDGET_CLASS (terminal_widget_parent_class)->motion_notify_event (widget, event);

  forwarded = gdk_event_copy ((GdkEvent *) event);
  forwarded->motion.state = terminal_widget_selection_modifiers (terminal_widget, event->state);
  handled = GTK_WIDGET_CLASS (terminal_widget_parent_class)->motion_notify_event (widget, &forwarded->motion);
  gdk_event_free (forwarded);
  return handled;
}



static gboolean
terminal_widget_button_press_event (GtkWidget *widget,
                                    GdkEventButton *event)
{
  TerminalWidget *terminal_widget = TERMINAL_WIDGET (widget);
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask ();
  gboolean committed = FALSE;
  gboolean intercept = FALSE;
  gboolean handled = FALSE;
  gboolean open_trigger_matched = FALSE;
  gboolean insert_trigger_matched = FALSE;
  gboolean insert_middle_click = FALSE;
  gboolean middle_click_opens_uri;
  guint insert_button = 0;
  guint insert_modifier = 0;
  guint open_button = 0;
  guint open_modifier = 0;
  guint signal_id = 0;
  GdkEvent *forwarded = NULL;

  if (event->type == GDK_BUTTON_PRESS && event->button == 1)
    {
      terminal_widget->mouse_selection = TERMINAL_MOUSE_SELECTION_DEFAULT;
      terminal_widget->link_selection_drag_pending = FALSE;
    }

  if (event->type == GDK_BUTTON_PRESS)
    {
      g_object_get (G_OBJECT (terminal_widget->preferences),
                    "misc-hyperlink-insert-button", &insert_button,
                    "misc-hyperlink-insert-modifier", &insert_modifier,
                    "misc-hyperlink-insert-middle-click", &insert_middle_click,
                    "misc-hyperlink-open-button", &open_button,
                    "misc-hyperlink-open-modifier", &open_modifier,
                    "misc-middle-click-opens-uri", &middle_click_opens_uri,
                    NULL);

      if (terminal_widget_event_is_ctrl_shift_left_click (event)
          && terminal_widget_click_open_parent_from_event (terminal_widget, event))
        return TRUE;

      if (open_button == 0)
        {
          open_trigger_matched = middle_click_opens_uri
                                 ? event->button == 2
                                 : terminal_widget_event_matches_button_trigger (event, 1, GDK_CONTROL_MASK);
        }
      else
        {
          open_trigger_matched = terminal_widget_event_matches_button_trigger (event, open_button, open_modifier);
        }

      insert_trigger_matched = terminal_widget_event_matches_button_trigger (event, insert_button, insert_modifier);
      if (insert_middle_click && event->button == 2)
        insert_trigger_matched = TRUE;

      if (open_trigger_matched)
        {
          if (terminal_widget_click_open_link_from_event (terminal_widget, event))
            return TRUE;
        }

      if (insert_trigger_matched)
        {
          if (terminal_widget_click_insert_link_from_event (terminal_widget, event))
            return TRUE;
        }

      if ((event->state & modifiers) == GDK_SHIFT_MASK
          && (event->button == 2 || event->button == 3))
        {
          intercept = TRUE;
        }
      else if (event->button == 3)
        {
          signal_id = g_signal_connect (G_OBJECT (widget), "commit",
                                        G_CALLBACK (terminal_widget_commit), &committed);
        }
    }

  if (event->type == GDK_BUTTON_PRESS && event->button == 1)
    {
      gboolean prefer_selection;

      g_object_get (G_OBJECT (terminal_widget->preferences),
                    "misc-prefer-mouse-selection", &prefer_selection, NULL);
      if (prefer_selection)
        {
          if ((event->state & modifiers) == 0)
            {
              terminal_widget->mouse_selection = TERMINAL_MOUSE_SELECTION_LOCAL;
              /* Synthetic Shift must start a new selection, not extend the old one. */
              vte_terminal_unselect_all (VTE_TERMINAL (widget));
            }
          else if ((event->state & modifiers) == GDK_SHIFT_MASK)
            terminal_widget->mouse_selection = TERMINAL_MOUSE_SELECTION_APPLICATION;
        }
    }

  /* Keep the route chosen at press time through double clicks, motion and release.
   * Only VTE sees these modifiers; hyperlink shortcuts use the original event. */
  if (event->button == 1 && terminal_widget->mouse_selection != TERMINAL_MOUSE_SELECTION_DEFAULT)
    {
      forwarded = gdk_event_copy ((GdkEvent *) event);
      forwarded->button.state = terminal_widget_selection_modifiers (terminal_widget, event->state);
    }

  if (!intercept)
    {
      GtkSettings *settings = gtk_settings_get_default ();
      gboolean primary_paste_enabled;
      g_object_get (settings, "gtk-enable-primary-paste", &primary_paste_enabled, NULL);

      /* don't let vte handle primary paste; we want to do it ourselves later, especially
       * to trigger the unsafe paste dialog if necessary */
      g_object_set (settings, "gtk-enable-primary-paste", FALSE, NULL);
      handled = (*GTK_WIDGET_CLASS (terminal_widget_parent_class)->button_press_event) (widget,
                                                                                      forwarded != NULL ? &forwarded->button : event);
      g_object_set (settings, "gtk-enable-primary-paste", primary_paste_enabled, NULL);
    }
  g_clear_pointer (&forwarded, gdk_event_free);

  if (handled)
    terminal_widget_select_link (terminal_widget, event);

  if (event->button == 2 && event->type == GDK_BUTTON_PRESS)
    {
      /* if handled is true, it means the VteTerminal's handler either already
       * pasted the selection on its own or passed the middle button click event
       * to the terminal application. In both cases we are done. Otherwise,
       * we need to paste the selection now.
       */
      if (!handled && vte_terminal_get_has_selection (VTE_TERMINAL (widget)))
        {
          g_signal_emit (G_OBJECT (widget), widget_signals[PASTE_SELECTION_REQUEST], 0, NULL);
        }
    }
  else if (event->button == 3 && event->type == GDK_BUTTON_PRESS)
    {
      if (signal_id != 0)
        g_signal_handler_disconnect (G_OBJECT (widget), signal_id);

      /* no data (mouse actions) was committed to the terminal application
       * which means, we can safely popup a context menu now.
       */
      if (!committed)
        {
          TerminalRightClickAction action;

          g_object_get (G_OBJECT (terminal_widget->preferences), "misc-right-click-action", &action, NULL);

          if (action == TERMINAL_RIGHT_CLICK_ACTION_CONTEXT_MENU)
            terminal_widget_context_menu (TERMINAL_WIDGET (widget),
                                          event->button, event->time,
                                          (GdkEvent *) event);
          else if (action == TERMINAL_RIGHT_CLICK_ACTION_PASTE_CLIPBOARD)
            g_signal_emit (G_OBJECT (widget), widget_signals[PASTE_CLIPBOARD_REQUEST], 0, NULL);
          else if (action == TERMINAL_RIGHT_CLICK_ACTION_PASTE_SELECTION)
            g_signal_emit (G_OBJECT (widget), widget_signals[PASTE_SELECTION_REQUEST], 0, NULL);
        }
    }

  return TRUE;
}



static void
terminal_widget_drag_data_received (GtkWidget *widget,
                                    GdkDragContext *context,
                                    gint x,
                                    gint y,
                                    GtkSelectionData *selection_data,
                                    guint info,
                                    guint time)
{
  const gunichar2 *ucs;
  GdkRGBA color;
  GString *str;
  GValue value = G_VALUE_INIT;
  gchar **uris;
  gchar *filename;
  gchar *text;
  gchar *image_path;
  gchar *quoted_image_path;
  gint n;
  GtkWidget *screen;
  gboolean succeed = TRUE;

  switch (info)
    {
    case TARGET_STRING:
    case TARGET_UTF8_STRING:
    case TARGET_COMPOUND_TEXT:
    case TARGET_TEXT:
      text = (gchar *) gtk_selection_data_get_text (selection_data);
      if (G_LIKELY (text != NULL))
        {
          if (G_LIKELY (*text != '\0'))
            vte_terminal_feed_child (VTE_TERMINAL (widget), text, strlen (text));
          g_free (text);
        }
      break;

    case TARGET_IMAGE:
      image_path = terminal_widget_save_image_drop (selection_data);
      if (image_path != NULL)
        {
          quoted_image_path = g_shell_quote (image_path);
          vte_terminal_feed_child (VTE_TERMINAL (widget),
                                   quoted_image_path,
                                   strlen (quoted_image_path));
          vte_terminal_feed_child (VTE_TERMINAL (widget), " ", 1);
          g_free (quoted_image_path);
          g_free (image_path);
        }
      else
        {
          succeed = FALSE;
        }
      break;

    case TARGET_TEXT_PLAIN:
      if (gtk_selection_data_get_format (selection_data) != 8 || gtk_selection_data_get_length (selection_data) == 0)
        {
          g_warning ("Unable to drop selection of type text/plain to terminal: Wrong format (%d) or length (%d)",
                     gtk_selection_data_get_format (selection_data), gtk_selection_data_get_length (selection_data));
        }
      else
        {
          vte_terminal_feed_child (VTE_TERMINAL (widget),
                                   (const gchar *) gtk_selection_data_get_data (selection_data),
                                   gtk_selection_data_get_length (selection_data));
        }
      break;

    case TARGET_MOZ_URL:
      if (gtk_selection_data_get_format (selection_data) != 8
          || gtk_selection_data_get_length (selection_data) == 0
          || (gtk_selection_data_get_length (selection_data) % 2) != 0)
        {
          g_warning ("Unable to drop Mozilla URL on terminal: Wrong format (%d) or length (%d)",
                     gtk_selection_data_get_format (selection_data), gtk_selection_data_get_length (selection_data));
        }
      else
        {
          str = g_string_new (NULL);
          ucs = (const gunichar2 *) (gpointer) gtk_selection_data_get_data (selection_data);
          for (n = 0; n < gtk_selection_data_get_length (selection_data) / 2 && ucs[n] != '\n'; ++n)
            g_string_append_unichar (str, ucs[n]);
          filename = g_filename_from_uri (str->str, NULL, NULL);
          if (filename != NULL)
            {
              vte_terminal_feed_child (VTE_TERMINAL (widget), filename, strlen (filename));
              g_free (filename);
            }
          else
            {
              vte_terminal_feed_child (VTE_TERMINAL (widget), str->str, str->len);
            }
          vte_terminal_feed_child (VTE_TERMINAL (widget), " ", 1);
          g_string_free (str, TRUE);
        }
      break;

    case TARGET_URI_LIST:
      if (gtk_selection_data_get_format (selection_data) != 8 || gtk_selection_data_get_length (selection_data) == 0)
        {
          g_warning ("Unable to drop URI list on terminal: Wrong format (%d) or length (%d)",
                     gtk_selection_data_get_format (selection_data), gtk_selection_data_get_length (selection_data));
        }
      else
        {
          /* split the text/uri-list */
          text = g_strndup ((const gchar *) gtk_selection_data_get_data (selection_data), gtk_selection_data_get_length (selection_data));
          uris = g_uri_list_extract_uris (text);
          g_free (text);

          /* translate all file:-URIs to quoted file names */
          for (n = 0; uris[n] != NULL; ++n)
            {
              /* check if we have a local file here */
              filename = g_filename_from_uri (uris[n], NULL, NULL);
              if (G_LIKELY (filename != NULL))
                {
                  /* exclude non-ASCII characters from escaping below */
                  const gchar *excluded =
                    "\"\\"
                    "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f"
                    "\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f"
                    "\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf"
                    "\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf"
                    "\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf"
                    "\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf"
                    "\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef"
                    "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff";

                  /* release the file:-URI */
                  g_free (uris[n]);

                  /* quote the file name (for the shell) */
                  uris[n] = g_shell_quote (filename);
                  g_free (filename);
                  filename = g_strescape (uris[n], excluded);
                  g_free (uris[n]);
                  uris[n] = filename;
                }
            }

          text = g_strjoinv (" ", uris);
          vte_terminal_feed_child (VTE_TERMINAL (widget), text, strlen (text));
          vte_terminal_feed_child (VTE_TERMINAL (widget), " ", 1);
          g_strfreev (uris);
          g_free (text);
        }
      break;

    case TARGET_APPLICATION_X_COLOR:
      if (gtk_selection_data_get_format (selection_data) != 16 || gtk_selection_data_get_length (selection_data) != 8)
        {
          g_warning ("Received invalid color data: Wrong format (%d) or length (%d)",
                     gtk_selection_data_get_format (selection_data), gtk_selection_data_get_length (selection_data));
        }
      else
        {
          /* get the color from the selection data (ignoring the alpha setting) */
          const guchar *data = gtk_selection_data_get_data (selection_data);
          color.red = (gdouble) data[0] / 65535.;
          color.green = (gdouble) data[1] / 65535.;
          color.blue = (gdouble) data[2] / 65535.;
          color.alpha = 1.;

          /* prepare the value */
          g_value_init (&value, GDK_TYPE_RGBA);
          g_value_set_boxed (&value, &color);

          /* change the background to the specified color */
          g_object_set_property (G_OBJECT (TERMINAL_WIDGET (widget)->preferences), "color-background", &value);

          /* release the value */
          g_value_unset (&value);
        }
      break;

    case TARGET_GTK_NOTEBOOK_TAB:
      /* 'send' the drag to the parent's parent widget (TerminalWidget -> GtkBox -> TerminalScreen) */
      screen = gtk_widget_get_parent (gtk_widget_get_parent (widget));
      if (G_LIKELY (screen))
        {
          g_signal_emit_by_name (G_OBJECT (screen), "drag-data-received", context,
                                 x, y, selection_data, info, time);
        }
      break;

    default:
      /* never finish the drag */
      return;
    }

  if (info != TARGET_GTK_NOTEBOOK_TAB)
    gtk_drag_finish (context, succeed, FALSE, time);
}



static gboolean
terminal_widget_key_press_event (GtkWidget *widget,
                                 GdkEventKey *event)
{
  const GdkModifierType mask = gtk_accelerator_get_default_mod_mask ();
  gboolean scroll_on_keystroke;
  gboolean shortcuts_no_menukey;

  if (event->keyval == GDK_KEY_BackSpace
      && (event->state & mask) == GDK_CONTROL_MASK)
    {
      /* Match xfce4-terminal's working Alt+Backspace behavior globally. */
      vte_terminal_feed_child (VTE_TERMINAL (widget), "\033\177", 2);
      return TRUE;
    }

  if ((event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
      && ((event->state & mask) == GDK_SHIFT_MASK
          || (event->state & mask) == GDK_CONTROL_MASK
          || (event->state & mask) == GDK_MOD1_MASK))
    {
      TerminalForegroundApplication application = terminal_widget_foreground_application (TERMINAL_WIDGET (widget));

      if (application == TERMINAL_FOREGROUND_CODEX)
        {
          /* Map modified Enter newline shortcuts to Ctrl+J (LF). */
          vte_terminal_feed_child (VTE_TERMINAL (widget), "\n", 1);
          return TRUE;
        }
      if (application == TERMINAL_FOREGROUND_TMUX)
        {
          /* Preserve the modifier so tmux can choose the action for its active
           * pane. VTE otherwise collapses Shift/Ctrl+Enter to ordinary Enter. */
          const gchar *sequence = (event->state & mask) == GDK_SHIFT_MASK ? "\033[13;2u"
                                  : (event->state & mask) == GDK_CONTROL_MASK ? "\033[13;5u"
                                                                           : "\033[13;3u";
          vte_terminal_feed_child (VTE_TERMINAL (widget), sequence, strlen (sequence));
          return TRUE;
        }
    }

  g_object_get (G_OBJECT (TERMINAL_WIDGET (widget)->preferences),
                "scrolling-on-keystroke", &scroll_on_keystroke,
                NULL);
  if (scroll_on_keystroke && terminal_widget_key_should_scroll_to_bottom (event))
    terminal_widget_scroll_to_bottom (TERMINAL_WIDGET (widget));

  /* determine current settings */
  g_object_get (G_OBJECT (TERMINAL_WIDGET (widget)->preferences),
                "shortcuts-no-menukey", &shortcuts_no_menukey,
                NULL);

  /* popup context menu if "Menu" or "<Shift>F10" is pressed */
  if (event->keyval == GDK_KEY_Menu
      || (!shortcuts_no_menukey
          && (event->state & GDK_SHIFT_MASK) != 0
          && event->keyval == GDK_KEY_F10))
    {
      terminal_widget_context_menu (TERMINAL_WIDGET (widget), 0, event->time, (GdkEvent *) event);
      return TRUE;
    }

  return (*GTK_WIDGET_CLASS (terminal_widget_parent_class)->key_press_event) (widget, event);
}



static void
terminal_widget_open_uri (TerminalWidget *widget,
                          const gchar *wlink,
                          PatternType type,
                          guint32 event_time)
{
  GtkWidget *toplevel;
  GtkWindow *window = NULL;
  GError *error = NULL;
  gchar *uri;
  gchar *missing_path = NULL;
  gchar *selection_uri = NULL;

  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (widget));
  if (GTK_IS_WINDOW (toplevel))
    window = GTK_WINDOW (toplevel);

  /* handle the pattern type */
  switch (type)
    {
    case PATTERN_TYPE_FULL_HTTP:
    case PATTERN_TYPE_FILE:
      uri = g_strdup (wlink);
      break;

    case PATTERN_TYPE_HTTP:
      uri = g_strconcat ("http://", wlink, NULL);
      break;

    case PATTERN_TYPE_EMAIL:
      uri = strncmp (wlink, MAILTO, strlen (MAILTO)) == 0
              ? g_strdup (wlink)
              : g_strconcat (MAILTO, wlink, NULL);
      break;

    default:
      g_warning ("Invalid tag specified while trying to open link \"%s\".", wlink);
      return;
    }

  if (event_time == 0)
    event_time = gtk_get_current_event_time ();

  if (type == PATTERN_TYPE_FILE && terminal_widget_is_pcmanfm_selection_uri (uri))
    {
      terminal_widget_open_pcmanfm_selection_uri (window, uri, widget);
    }
  else if (type == PATTERN_TYPE_FILE
           && (selection_uri = terminal_widget_get_pcmanfm_selection_fallback_uri (uri)) != NULL)
    {
      terminal_widget_open_pcmanfm_selection_uri (window, selection_uri, widget);
    }
  else if (type == PATTERN_TYPE_FILE
           && (missing_path = g_filename_from_uri (uri, NULL, NULL)) != NULL
           && !g_file_test (missing_path, G_FILE_TEST_EXISTS)
           && terminal_widget_start_unearth_search_for_path (widget, missing_path, event_time, FALSE))
    {
      /* Unearth will reopen the resolved match from its asynchronous callback. */
    }
  else if (!gtk_show_uri_on_window (window, uri, event_time, &error))
    {
      /* tell the user that we were unable to open the responsible application */
      xfce_dialog_show_error (window, error, _("Failed to open the URL '%s'"), uri);
      g_error_free (error);
    }

  g_free (missing_path);
  g_free (selection_uri);
  g_free (uri);
}



static gboolean
terminal_widget_is_pcmanfm_selection_uri (const gchar *uri)
{
  const gchar *query;
  const gchar *parameter;

  if (!g_str_has_prefix (uri, "file://"))
    return FALSE;

  query = strchr (uri, '?');
  if (query == NULL)
    return FALSE;

  parameter = query + 1;
  while (*parameter != '\0' && *parameter != '#')
    {
      if (g_str_has_prefix (parameter, "select=")
          && (parameter == query + 1 || parameter[-1] == '&'))
        return TRUE;

      parameter = strchr (parameter, '&');
      if (parameter == NULL)
        break;
      parameter++;
    }

  return FALSE;
}



static gboolean
terminal_widget_content_type_is_binary (const gchar *content_type)
{
  gchar *mime_type;
  gboolean is_binary;

  if (content_type == NULL)
    return FALSE;

  /* Do not use g_content_type_is_a() here.  The MIME database can report
   * text types such as application/json as descendants of broad executable
   * types, which would incorrectly send them to the file manager. */
  mime_type = g_content_type_get_mime_type (content_type);
  is_binary = g_strcmp0 (mime_type != NULL ? mime_type : content_type,
                         "application/x-executable") == 0
              || g_strcmp0 (mime_type != NULL ? mime_type : content_type,
                            "application/x-pie-executable") == 0
              || g_strcmp0 (mime_type != NULL ? mime_type : content_type,
                            "application/x-sharedlib") == 0
              || g_strcmp0 (mime_type != NULL ? mime_type : content_type,
                            "application/x-object") == 0;
  g_free (mime_type);
  return is_binary;
}



static gchar *
terminal_widget_get_pcmanfm_selection_fallback_uri (const gchar *uri)
{
  GFile *file = NULL;
  GFile *parent = NULL;
  GFileInfo *info = NULL;
  GAppInfo *handler = NULL;
  const gchar *content_type;
  gchar *path = NULL;
  gchar *parent_uri = NULL;
  gchar *escaped_path = NULL;
  gchar *selection_uri = NULL;
  gboolean is_binary = FALSE;

  if (!g_str_has_prefix (uri, "file://"))
    return NULL;

  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  if (path == NULL)
    goto out;

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info == NULL)
    goto out;

  content_type = g_file_info_get_content_type (info);
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
    is_binary = terminal_widget_content_type_is_binary (content_type);

  if (!is_binary)
    handler = g_file_query_default_handler (file, NULL, NULL);
  if (!is_binary && handler != NULL)
    goto out;

  parent = g_file_get_parent (file);
  if (parent == NULL)
    goto out;

  parent_uri = g_file_get_uri (parent);
  escaped_path = g_uri_escape_string (path, "/", FALSE);
  selection_uri = g_strdup_printf (g_str_has_suffix (parent_uri, "/")
                                     ? "%s?select=%s"
                                     : "%s/?select=%s",
                                   parent_uri, escaped_path);

out:
  g_clear_object (&handler);
  g_clear_object (&info);
  g_clear_object (&parent);
  g_clear_object (&file);
  g_free (escaped_path);
  g_free (parent_uri);
  g_free (path);
  return selection_uri;
}



static gboolean
terminal_widget_open_pcmanfm_selection_uri (GtkWindow *window,
                                            const gchar *uri,
                                            TerminalWidget *widget)
{
  gchar *configured_command = NULL;
  gchar *file_manager = NULL;
  gchar *argv[3];
  GError *error = NULL;
  gboolean result;

  g_object_get (G_OBJECT (widget->preferences),
                "misc-hyperlink-file-manager", &configured_command,
                NULL);
  if (configured_command == NULL || *configured_command == '\0')
    {
      g_free (configured_command);
      configured_command = g_strdup ("pcmanfm");
    }

  file_manager = g_path_is_absolute (configured_command)
                   ? g_strdup (configured_command)
                   : g_find_program_in_path (configured_command);
  if (file_manager == NULL)
    {
      g_set_error (&error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT,
                   _("The configured file-manager executable '%s' was not found"),
                   configured_command);
      result = FALSE;
    }
  else
    {
      argv[0] = file_manager;
      argv[1] = (gchar *) uri;
      argv[2] = NULL;
      result = g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, &error);
    }

  if (!result)
    {
      xfce_dialog_show_error (window, error,
                              _("Failed to open the file-manager selection URI '%s'"), uri);
      g_clear_error (&error);
    }

  g_free (file_manager);
  g_free (configured_command);
  return result;
}



static gboolean
terminal_widget_key_should_scroll_to_bottom (GdkEventKey *event)
{
  switch (event->keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_Up:
    case GDK_KEY_Down:
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_KP_Up:
    case GDK_KEY_KP_Down:
    case GDK_KEY_KP_Left:
    case GDK_KEY_KP_Right:
      return TRUE;

    default:
      return FALSE;
    }
}



static void
terminal_widget_scroll_to_bottom (TerminalWidget *widget)
{
  GtkAdjustment *adjustment;
  gdouble value;

  adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (widget));
  value = MAX (0.0, gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment));
  gtk_adjustment_set_value (adjustment, value);
}



static void
terminal_widget_update_highlight_urls (TerminalWidget *widget)
{
  guint i;
  gboolean highlight_urls;
  gboolean auto_detect_file_paths;
  VteRegex *regex;
  const TerminalRegexPattern *pattern;
  GError *error;

  g_object_get (G_OBJECT (widget->preferences),
                "misc-highlight-urls", &highlight_urls,
                "misc-auto-detect-file-paths", &auto_detect_file_paths,
                NULL);

  for (i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
    {
      const TerminalRegexPattern *current_pattern = &regex_patterns[i];
      gboolean should_be_enabled = highlight_urls
                                   && (current_pattern->type != PATTERN_TYPE_PATH
                                       || auto_detect_file_paths);

      if (!should_be_enabled)
        {
          if (widget->regex_tags[i] != -1)
            {
              vte_terminal_match_remove (VTE_TERMINAL (widget), widget->regex_tags[i]);
              widget->regex_tags[i] = -1;
            }
          continue;
        }

      /* continue if already set */
      if (G_UNLIKELY (widget->regex_tags[i] != -1))
        continue;

      /* get the pattern */
      pattern = current_pattern;

      /* build the regex */
      error = NULL;
      regex = vte_regex_new_for_match (pattern->pattern, -1,
                                       PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP
                                       | PCRE2_NO_UTF_CHECK | PCRE2_MULTILINE,
                                       &error);

      if (error == NULL
          && (!vte_regex_jit (regex, PCRE2_JIT_COMPLETE, &error)
              || !vte_regex_jit (regex, PCRE2_JIT_PARTIAL_SOFT, &error)))
        {
          g_critical ("Failed to JIT regular expression '%s': %s\n", pattern->pattern, error->message);
          g_clear_error (&error);
        }
      if (G_UNLIKELY (error != NULL))
        {
          g_critical ("Failed to parse regular expression pattern %u: %s", i, error->message);
          g_error_free (error);
          continue;
        }

      /* set the new regular expression */
      widget->regex_tags[i] = vte_terminal_match_add_regex (VTE_TERMINAL (widget), regex, 0);
      if (pattern->type != PATTERN_TYPE_PATH)
        {
#if VTE_CHECK_VERSION(0, 53, 0)
          vte_terminal_match_set_cursor_name (VTE_TERMINAL (widget), widget->regex_tags[i], "hand2");
#else
          vte_terminal_match_set_cursor_type (VTE_TERMINAL (widget), widget->regex_tags[i], GDK_HAND2);
#endif
        }
      /* release the regex owned by vte now */
      vte_regex_unref (regex);
    }
}



static gboolean
terminal_widget_regex_tag_is_path (TerminalWidget *widget,
                                   gint tag)
{
  for (guint i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
    {
      if (widget->regex_tags[i] == tag)
        return regex_patterns[i].type == PATTERN_TYPE_PATH;
    }

  return FALSE;
}



static gboolean
terminal_widget_action_scroll_page_up (TerminalWidget *widget)
{
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (widget));

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_value (adjustment) - gtk_adjustment_get_page_size (adjustment));
  return TRUE;
}



static gboolean
terminal_widget_action_scroll_page_down (TerminalWidget *widget)
{
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (widget));

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_value (adjustment) + gtk_adjustment_get_page_size (adjustment));
  return TRUE;
}



static gboolean
terminal_widget_action_shift_scroll_up (TerminalWidget *widget)
{
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (widget));

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_value (adjustment) - 1);
  return TRUE;
}



static gboolean
terminal_widget_action_shift_scroll_down (TerminalWidget *widget)
{
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (widget));
  gdouble value;

  value = MIN (gtk_adjustment_get_value (adjustment) + 1, gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment));
  gtk_adjustment_set_value (adjustment, value);
  return TRUE;
}



static void
terminal_widget_connect_accelerators (TerminalWidget *widget)
{
  g_return_if_fail (TERMINAL_IS_WIDGET (widget));

  if (widget->accel_group == NULL)
    return;

  xfce_gtk_accel_map_add_entries (action_entries, G_N_ELEMENTS (action_entries));
  xfce_gtk_accel_group_connect_action_entries (widget->accel_group,
                                               action_entries,
                                               G_N_ELEMENTS (action_entries),
                                               widget);
}



static void
terminal_widget_disconnect_accelerators (TerminalWidget *widget)
{
  g_return_if_fail (TERMINAL_IS_WIDGET (widget));

  if (widget->accel_group == NULL)
    return;

  /* Don't listen to the accel keys defined by the action entries any more */
  xfce_gtk_accel_group_disconnect_action_entries (widget->accel_group,
                                                  action_entries,
                                                  G_N_ELEMENTS (action_entries));

  /* and release the accel group */
  g_clear_object (&widget->accel_group);
}



XfceGtkActionEntry *
terminal_widget_get_action_entries (void)
{
  return action_entries;
}



static TerminalHyperlink
terminal_widget_get_link (TerminalWidget *widget,
                          GdkEvent *event)
{
  guint i;
  gint tag;
  gchar *uri;
  pcre2_match_data_8 *match_data;
  TerminalHyperlink result = { NULL, PATTERN_TYPE_NONE };
  gboolean hyperlinks_enabled;

  g_object_get (G_OBJECT (TERMINAL_WIDGET (widget)->preferences), "misc-hyperlinks-enabled", &hyperlinks_enabled, NULL);

  /* check if we have an OSC 8 uri */
  if (hyperlinks_enabled && (uri = vte_terminal_hyperlink_check_event (VTE_TERMINAL (widget), (GdkEvent *) event)) != NULL)
    {
      if (g_str_has_prefix (uri, "file://"))
        {
          gchar *filename = g_filename_from_uri (uri, NULL, NULL);

          if (filename != NULL
              && terminal_widget_link_clickable (uri, PATTERN_TYPE_FILE))
            {
              g_free (filename);
              result.uri = uri;
              result.type = PATTERN_TYPE_FILE;
              return result;
            }

          g_free (filename);
        }

      gint rc;

      for (i = 0; i < G_N_ELEMENTS (widget->regex_pcre); i++)
        {
          if (widget->regex_pcre[i] == NULL
              || regex_patterns[i].type == PATTERN_TYPE_PATH)
            continue;

          match_data = pcre2_match_data_create_from_pattern_8 (widget->regex_pcre[i], NULL);
          rc = pcre2_match_8 (widget->regex_pcre[i], (PCRE2_SPTR8) uri, strlen (uri), 0, 0, match_data, NULL);
          pcre2_match_data_free_8 (match_data);

          if (rc >= 0)
            {
              if (regex_patterns[i].type == PATTERN_TYPE_FILE
                  && !terminal_widget_link_clickable (uri, PATTERN_TYPE_FILE))
                continue;

              result.uri = uri;
              result.type = regex_patterns[i].type;
              return result;
            }
          else if (rc != PCRE2_ERROR_NOMATCH)
            g_warning ("pcre2_match returned error code \"%d\".", rc);
        }
      g_free (uri);
    }

  /* check if we have a regex match */
  if ((uri = vte_terminal_match_check_event (VTE_TERMINAL (widget), event, &tag)) != NULL)
    {
      for (i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
        {
          /* lookup the tag in our tags */
          if (widget->regex_tags[i] == tag)
            {
              if (regex_patterns[i].type == PATTERN_TYPE_PATH)
                {
                  gchar *path = terminal_widget_candidate_to_path (widget, uri, TRUE);
                  gchar *path_uri = NULL;
                  gboolean path_available = path != NULL;

                  if (path != NULL && g_file_test (path, G_FILE_TEST_EXISTS))
                    path_uri = g_filename_to_uri (path, NULL, NULL);
                  g_free (path);
                  if (path_uri != NULL)
                    {
                      g_free (uri);
                      result.uri = path_uri;
                      result.type = PATTERN_TYPE_FILE;
                      return result;
                    }

                  if (!path_available)
                    {
                      g_free (uri);
                      return result;
                    }

                  result.uri = uri;
                  result.type = PATTERN_TYPE_PATH;
                  return result;
                }

              if (regex_patterns[i].type == PATTERN_TYPE_FILE
                  && !terminal_widget_link_clickable (uri, PATTERN_TYPE_FILE))
                {
                  g_free (uri);
                  return result;
                }

              result.uri = uri;
              result.type = regex_patterns[i].type;
              return result;
            }
        }
      g_free (uri);
    }

  return result;
}



/*
 * Ensures that hostname component of a link with a file:// schema
 * matches the current hostname or is "localhost".
 *
 * See: https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
 */
static gboolean
terminal_widget_link_clickable (const gchar *uri,
                                PatternType type)
{
  gboolean result = FALSE;
  gchar *filename;
  gchar *hostname;

  if (type != PATTERN_TYPE_FILE)
    return TRUE;

  if (uri == NULL || strlen (uri) > 8192)
    return FALSE;
  for (const gchar *p = uri; *p != '\0'; p++)
    if ((guchar) *p < 0x20 || *p == 0x7f)
      return FALSE;

  filename = g_filename_from_uri (uri, &hostname, NULL);

  result = filename != NULL
           && (hostname == NULL
               || g_ascii_strcasecmp (hostname, "localhost") == 0
               || g_ascii_strcasecmp (hostname, g_get_host_name ()) == 0);

  g_free (filename);
  g_free (hostname);

  return result;
}



static void
terminal_widget_hyperlink_hover_uri_changed (TerminalWidget *widget,
                                             const char *uri,
                                             const GdkRectangle *bbox G_GNUC_UNUSED)
{
  gboolean enabled;

  if (!gtk_widget_get_realized (GTK_WIDGET (widget)))
    return;

  g_object_get (G_OBJECT (widget->preferences),
                "misc-hyperlink-tooltips-enabled", &enabled,
                NULL);
  gtk_widget_set_tooltip_text (GTK_WIDGET (widget), enabled ? uri : NULL);
}



static void
terminal_widget_update_hyperlink_tooltip (TerminalWidget *widget)
{
  gboolean enabled;

  g_object_get (G_OBJECT (widget->preferences),
                "misc-hyperlink-tooltips-enabled", &enabled,
                NULL);
  if (!enabled)
    gtk_widget_set_tooltip_text (GTK_WIDGET (widget), NULL);
}
