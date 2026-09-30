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
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_LIBUTEMPTER
#include <utempter.h>
#endif

#include <libxfce4ui/libxfce4ui.h>

#include "terminal-enum-types.h"
#include "terminal-marshal.h"
#include "terminal-preferences.h"
#include "terminal-private.h"
#include "terminal-regex.h"
#include "terminal-util.h"
#include "terminal-widget.h"



#define MAILTO "mailto:"



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
  PATTERN_TYPE_FILE
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

static const TerminalRegexPattern regex_patterns[] = {
  { REGEX_URL_AS_IS, PATTERN_TYPE_FULL_HTTP },
  { REGEX_URL_HTTP, PATTERN_TYPE_HTTP },
  { REGEX_URL_FILE, PATTERN_TYPE_FILE },
  { REGEX_EMAIL, PATTERN_TYPE_EMAIL },
  { REGEX_NEWS_MAN, PATTERN_TYPE_FULL_HTTP },
};



static void
terminal_widget_finalize (GObject *object);
static void
terminal_widget_set_property (GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec);
static gboolean
terminal_widget_button_press_event (GtkWidget *widget,
                                    GdkEventButton *event);
static void
terminal_widget_drag_data_received (GtkWidget *widget,
                                    GdkDragContext *context,
                                    gint x,
                                    gint y,
                                    GtkSelectionData *selection_data,
                                    guint info,
                                    guint time);
static gboolean
terminal_widget_key_press_event (GtkWidget *widget,
                                 GdkEventKey *event);
static void
terminal_widget_open_uri (TerminalWidget *widget,
                          const gchar *wlink,
                          PatternType type,
                          guint32 event_time);
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
static void
terminal_widget_hyperlink_hover_uri_changed (TerminalWidget *widget,
                                             const char *uri,
                                             const GdkRectangle *bbox G_GNUC_UNUSED);
static gchar *
terminal_widget_link_to_input (const gchar *uri,
                               PatternType type);
static gboolean
terminal_widget_key_should_scroll_to_bottom (GdkEventKey *event);
static void
terminal_widget_scroll_to_bottom (TerminalWidget *widget);



struct _TerminalWidget
{
  VteTerminal parent_instance;

  /*< private >*/
  TerminalPreferences *preferences;
  GtkAccelGroup *accel_group;
  gint regex_tags[G_N_ELEMENTS (regex_patterns)];
  pcre2_code_8 *regex_pcre[G_N_ELEMENTS (regex_patterns)];
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
  gobject_class->finalize = terminal_widget_finalize;
  gobject_class->set_property = terminal_widget_set_property;

  gtkwidget_class = GTK_WIDGET_CLASS (klass);
  gtkwidget_class->button_press_event = terminal_widget_button_press_event;
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

  /* update tooltip when hovering over a hyperlink */
  g_signal_connect (G_OBJECT (widget), "hyperlink-hover-uri-changed",
                    G_CALLBACK (terminal_widget_hyperlink_hover_uri_changed), NULL);

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

      widget->regex_pcre[i] = pcre2_compile_8 ((PCRE2_SPTR8) regex_patterns[i].pattern, PCRE2_ZERO_TERMINATED, 0, &error_number, &error_offset, NULL);
      if (widget->regex_pcre[i] == NULL)
        g_warning ("Failed to compile regex, error code \"%d\".", error_number);
    }
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
  GdkDisplay *display;
  gchar *modified_wlink = NULL;

  wlink = g_object_get_data (G_OBJECT (item), "terminal-widget-link");
  if (G_LIKELY (wlink != NULL))
    {
      display = gtk_widget_get_display (GTK_WIDGET (widget));

      /* strip mailto from links, bug #7909 */
      if (g_str_has_prefix (wlink, MAILTO))
        {
          modified_wlink = g_strdup (wlink + strlen (MAILTO));
          wlink = modified_wlink;
        }

      // The order of setting the clipboard does matter, see:
      // https://gitlab.xfce.org/apps/xfce4-terminal/-/issues/367

      /* copy the URI to "PRIMARY" */
      clipboard = gtk_clipboard_get_for_display (display, GDK_SELECTION_PRIMARY);
      gtk_clipboard_set_text (clipboard, wlink, -1);

      /* copy the URI to "CLIPBOARD" */
      clipboard = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
      gtk_clipboard_set_text (clipboard, wlink, -1);

      g_free (modified_wlink);
    }
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
      else if (link.type == PATTERN_TYPE_FILE)
        {
          item_copy = gtk_menu_item_new_with_label (_("Copy Link Address"));
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
  gchar *quoted_text = NULL;
  gchar *text;

  text = terminal_widget_link_to_input (uri, type);
  if (G_LIKELY (text != NULL && *text != '\0'))
    {
      quoted_text = terminal_widget_shell_quote_input (text);

      vte_terminal_feed_child (VTE_TERMINAL (widget),
                               quoted_text,
                               strlen (quoted_text));
    }

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
      if (terminal_widget_link_clickable (link.uri, link.type))
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


  return FALSE;
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



  if (!intercept)
    {
      GtkSettings *settings = gtk_settings_get_default ();
      gboolean primary_paste_enabled;
      g_object_get (settings, "gtk-enable-primary-paste", &primary_paste_enabled, NULL);

      /* don't let vte handle primary paste; we want to do it ourselves later, especially
       * to trigger the unsafe paste dialog if necessary */
      g_object_set (settings, "gtk-enable-primary-paste", FALSE, NULL);
      handled = (*GTK_WIDGET_CLASS (terminal_widget_parent_class)->button_press_event) (widget, event);
      g_object_set (settings, "gtk-enable-primary-paste", primary_paste_enabled, NULL);
    }


  if (event->button == 2 && event->type == GDK_BUTTON_PRESS)
    {
      /* if handled is true, it means the VteTerminal's handler either already
       * pasted the selection on its own or passed the middle button click event
       * to the terminal application. In both cases we are done. Otherwise,
       * we need to paste the selection now.
       */
      if (!handled)
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
  gint n;
  GtkWidget *screen;

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
    gtk_drag_finish (context, TRUE, FALSE, time);
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

  if (!gtk_show_uri_on_window (window, uri, event_time, &error))
    {
      /* tell the user that we were unable to open the responsible application */
      xfce_dialog_show_error (window, error, _("Failed to open the URL '%s'"), uri);
      g_error_free (error);
    }

  g_free (uri);
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
  VteRegex *regex;
  const TerminalRegexPattern *pattern;
  GError *error;

  g_object_get (G_OBJECT (widget->preferences),
                "misc-highlight-urls", &highlight_urls, NULL);

  if (!highlight_urls)
    {
      /* remove all our regex tags */
      for (i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
        if (widget->regex_tags[i] != -1)
          {
            vte_terminal_match_remove (VTE_TERMINAL (widget), widget->regex_tags[i]);
            widget->regex_tags[i] = -1;
          }
    }
  else
    {
      /* set all our patterns */
      for (i = 0; i < G_N_ELEMENTS (regex_patterns); i++)
        {
          /* continue if already set */
          if (G_UNLIKELY (widget->regex_tags[i] != -1))
            continue;

          /* get the pattern */
          pattern = &regex_patterns[i];

          /* build the regex */
          error = NULL;
          regex = vte_regex_new_for_match (pattern->pattern, -1,
                                           PCRE2_CASELESS | PCRE2_UTF | PCRE2_NO_UTF_CHECK | PCRE2_MULTILINE,
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
#if VTE_CHECK_VERSION(0, 53, 0)
          vte_terminal_match_set_cursor_name (VTE_TERMINAL (widget), widget->regex_tags[i], "hand2");
#else
          vte_terminal_match_set_cursor_type (VTE_TERMINAL (widget), widget->regex_tags[i], GDK_HAND2);
#endif
          /* release the regex owned by vte now */
          vte_regex_unref (regex);
        }
    }
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
          if (widget->regex_pcre[i] == NULL)
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
  if (!gtk_widget_get_realized (GTK_WIDGET (widget)))
    return;

  gtk_widget_set_tooltip_text (GTK_WIDGET (widget), uri);
}
