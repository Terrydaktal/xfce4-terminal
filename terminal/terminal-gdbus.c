/*-
 * Copyright (c) 2012 Nick Schermer <nick@xfce.org>
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

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "terminal-config.h"
#include "terminal-gdbus.h"
#include "terminal-private.h"
#include "terminal-screen.h"
#include "terminal-window.h"
#include "terminal-update.h"
#include "terminal-restart.h"



// clang-format off
static const gchar terminal_gdbus_introspection_xml[] =
  "<node>"
    "<interface name='" TERMINAL_DBUS_INTERFACE "'>"
      "<method name='" TERMINAL_DBUS_METHOD_LAUNCH "'>"
        "<arg type='u' name='uid' direction='in'/>"
        "<arg type='ay' name='display-name' direction='in'/>"
        "<arg type='aay' name='argv' direction='in'/>"
      "</method>"
      "<method name='" TERMINAL_DBUS_METHOD_LIST_TERMINALS "'>"
        "<arg type='aa{sv}' name='terminals' direction='out'/>"
      "</method>"
      "<method name='" TERMINAL_DBUS_METHOD_SEND_ENTER "'>"
        "<arg type='s' name='tab-uuid' direction='in'/>"
      "</method>"
      "<method name='" TERMINAL_DBUS_METHOD_SEND_YES "'>"
        "<arg type='s' name='tab-uuid' direction='in'/>"
      "</method>"
      "<method name='GetUpdateStatus'><arg type='s' name='window-uuid' direction='in'/>"
        "<arg type='s' name='status-json' direction='out'/></method>"
      "<method name='UpdateInPlace'><arg type='s' name='window-uuid' direction='in'/>"
        "<arg type='s' name='update-service' direction='out'/></method>"
      "<method name='CloseForUpdate'><arg type='s' name='window-uuid' direction='in'/>"
        "<arg type='s' name='update-service' direction='in'/></method>"
      "<method name='FinishUpdate'><arg type='s' name='update-service' direction='in'/></method>"
      "<method name='PrepareTabRestart'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
        "<arg type='as' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
      "<method name='CommitTabRestart'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
      "<method name='CancelTabRestart'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
      "<method name='GetTabRestartStatus'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
        "<arg type='s' direction='out'/></method>"
    "</interface>"
  "</node>";
// clang-format on



typedef struct
{
  GDBusConnection *connection;
  guint registration_id;
  guint launch_owner_id;
  guint instance_owner_id;
  gchar *instance_name;
} TerminalGdbusRegistration;



static void
terminal_gdbus_registration_free (gpointer user_data)
{
  TerminalGdbusRegistration *registration = user_data;

  if (registration->launch_owner_id != 0)
    g_bus_unown_name (registration->launch_owner_id);
  if (registration->instance_owner_id != 0)
    g_bus_unown_name (registration->instance_owner_id);
  if (registration->registration_id != 0)
    g_dbus_connection_unregister_object (registration->connection,
                                         registration->registration_id);

  g_clear_object (&registration->connection);
  g_free (registration->instance_name);
  g_free (registration);
}



static gchar *
terminal_gdbus_display_name (void)
{
  const gchar *display_name;
  gchar *name;
  gchar *period;

  display_name = g_getenv ("DISPLAY");
  if (G_UNLIKELY (display_name == NULL))
    display_name = "";

  name = g_strdup (display_name);
  period = strrchr (name, '.');
  if (period != NULL)
    *period = '\0';

  return name;
}



static void
terminal_gdbus_dict_add_string (GVariantBuilder *dict,
                                 const gchar *key,
                                 const gchar *value)
{
  g_variant_builder_add (dict, "{sv}", key, g_variant_new_string (value != NULL ? value : ""));
}



typedef struct
{
  TerminalApp *app;
  gchar *tab_uuid;
  gchar *text;
  GDBusMethodInvocation *invocation;
} TerminalGdbusSendTextData;



static void
terminal_gdbus_list_terminals (TerminalApp *app,
                               GDBusMethodInvocation *invocation)
{
  GVariantBuilder terminals;
  const GSList *windows;

  g_variant_builder_init (&terminals, G_VARIANT_TYPE ("aa{sv}"));
  windows = terminal_app_get_windows (app);

  for (const GSList *window_link = windows; window_link != NULL; window_link = window_link->next)
    {
      TerminalWindow *window = TERMINAL_WINDOW (window_link->data);
      GtkWidget *notebook = terminal_window_get_notebook (window);
      GList *tabs;

      if (!GTK_IS_CONTAINER (notebook))
        continue;

      tabs = gtk_container_get_children (GTK_CONTAINER (notebook));
      for (GList *tab_link = tabs; tab_link != NULL; tab_link = tab_link->next)
        {
          TerminalScreen *screen = TERMINAL_SCREEN (tab_link->data);
          const gchar *working_directory;
          const gchar *window_title;
          GPid child_pid;
          gint foreground_pgid;
          gchar *pty_name;
          GVariantBuilder terminal;

          working_directory = terminal_screen_get_working_directory (screen);
          window_title = gtk_window_get_title (GTK_WINDOW (window));
          child_pid = terminal_screen_get_child_pid (screen);
          foreground_pgid = terminal_screen_get_foreground_process_group (screen);
          pty_name = terminal_screen_get_pty_name (screen);

          g_variant_builder_init (&terminal, G_VARIANT_TYPE ("a{sv}"));
          terminal_gdbus_dict_add_string (&terminal, "window_uuid", terminal_window_get_uuid (window));
          terminal_gdbus_dict_add_string (&terminal, "tab_uuid", terminal_screen_get_uuid (screen));
          g_variant_builder_add (&terminal, "{sv}", "active",
                                 g_variant_new_boolean (terminal_window_get_active (window) == screen));
          terminal_gdbus_dict_add_string (&terminal, "window_title", window_title);
          terminal_gdbus_dict_add_string (&terminal, "working_directory", working_directory);
          g_variant_builder_add (&terminal, "{sv}", "child_pid",
                                 g_variant_new_uint32 (child_pid > 0 ? (guint32) child_pid : 0));
          g_variant_builder_add (&terminal, "{sv}", "foreground_pid",
                                 g_variant_new_uint32 (foreground_pgid > 0 ? (guint32) foreground_pgid : 0));
          g_variant_builder_add (&terminal, "{sv}", "foreground_pgid",
                                 g_variant_new_uint32 (foreground_pgid > 0 ? (guint32) foreground_pgid : 0));
          terminal_gdbus_dict_add_string (&terminal, "pty", pty_name);
          g_variant_builder_add (&terminal, "{sv}", "input_enabled",
                                 g_variant_new_boolean (terminal_screen_get_input_enabled (screen)));
          g_variant_builder_add_value (&terminals, g_variant_builder_end (&terminal));

          g_free (pty_name);
        }
      g_list_free (tabs);
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@aa{sv})",
                                                        g_variant_builder_end (&terminals)));
}



static TerminalScreen *
terminal_gdbus_find_screen (TerminalApp *app,
                            const gchar *tab_uuid)
{
  const GSList *windows;

  windows = terminal_app_get_windows (app);
  for (const GSList *window_link = windows; window_link != NULL; window_link = window_link->next)
    {
      TerminalWindow *window = TERMINAL_WINDOW (window_link->data);
      GtkWidget *notebook = terminal_window_get_notebook (window);
      GList *tabs;

      if (!GTK_IS_CONTAINER (notebook))
        continue;

      tabs = gtk_container_get_children (GTK_CONTAINER (notebook));
      for (GList *tab_link = tabs; tab_link != NULL; tab_link = tab_link->next)
        {
          TerminalScreen *screen = TERMINAL_SCREEN (tab_link->data);
          const gchar *uuid = terminal_screen_get_uuid (screen);

          if (g_strcmp0 (uuid, tab_uuid) == 0)
            {
              g_list_free (tabs);
              return screen;
            }
        }
      g_list_free (tabs);
    }

  return NULL;
}



static void
terminal_gdbus_send_text (TerminalApp *app,
                          const gchar *tab_uuid,
                          const gchar *text,
                          GDBusMethodInvocation *invocation)
{
  TerminalScreen *screen;

  screen = terminal_gdbus_find_screen (app, tab_uuid);
  if (screen == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             TERMINAL_ERROR, TERMINAL_ERROR_TAB_NOT_FOUND,
                                             _("Unknown terminal tab UUID: %s"), tab_uuid);
      return;
    }

  if (!terminal_screen_get_input_enabled (screen)
      || g_getenv ("XFCE4_TERMINAL_UPDATE_SERVICE") != NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             TERMINAL_ERROR, TERMINAL_ERROR_INPUT_DISABLED,
                                             _("Terminal tab input is disabled"));
      return;
    }

  if (!terminal_screen_can_feed_text (screen))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             TERMINAL_ERROR, TERMINAL_ERROR_TAB_UNAVAILABLE,
                                             _("Terminal tab has no usable child PTY"));
      return;
    }

  terminal_screen_feed_text (screen, text);
  g_dbus_method_invocation_return_value (invocation, NULL);
}



static void
terminal_gdbus_send_text_uid_cb (GObject *source_object,
                                 GAsyncResult *result,
                                 gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);
  TerminalGdbusSendTextData *data = user_data;
  GError *error = NULL;
  GVariant *reply;
  guint32 uid;

  reply = g_dbus_connection_call_finish (connection, result, &error);
  if (reply == NULL)
    {
      g_dbus_method_invocation_return_error (data->invocation,
                                             TERMINAL_ERROR, TERMINAL_ERROR_USER_MISMATCH,
                                             _("Unable to verify caller identity: %s"),
                                             error->message);
      g_error_free (error);
    }
  else
    {
      g_variant_get (reply, "(u)", &uid);
      if (uid != getuid ())
        {
          g_dbus_method_invocation_return_error (data->invocation,
                                                 TERMINAL_ERROR, TERMINAL_ERROR_USER_MISMATCH,
                                                 _("User id mismatch"));
        }
      else
        {
          terminal_gdbus_send_text (data->app, data->tab_uuid,
                                    data->text, data->invocation);
        }

      g_variant_unref (reply);
    }

  g_object_unref (data->invocation);
  g_object_unref (data->app);
  g_free (data->tab_uuid);
  g_free (data->text);
  g_free (data);
}



static void
terminal_gdbus_authorize_send_text (GDBusConnection *connection,
                                    const gchar *sender,
                                    TerminalApp *app,
                                    const gchar *tab_uuid,
                                    const gchar *text,
                                    GDBusMethodInvocation *invocation)
{
  TerminalGdbusSendTextData *data;

  if (sender == NULL || *sender == '\0')
    {
      g_dbus_method_invocation_return_error (invocation,
                                             TERMINAL_ERROR, TERMINAL_ERROR_USER_MISMATCH,
                                             _("Unable to verify caller identity"));
      return;
    }

  data = g_new0 (TerminalGdbusSendTextData, 1);
  data->app = g_object_ref (app);
  data->tab_uuid = g_strdup (tab_uuid);
  data->text = g_strdup (text);
  data->invocation = g_object_ref (invocation);

  g_dbus_connection_call (connection,
                          "org.freedesktop.DBus",
                          "/org/freedesktop/DBus",
                          "org.freedesktop.DBus",
                          "GetConnectionUnixUser",
                          g_variant_new ("(s)", sender),
                          G_VARIANT_TYPE ("(u)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          terminal_gdbus_send_text_uid_cb,
                          data);
}



static gboolean
terminal_gdbus_close_update_window (gpointer data)
{
  gtk_widget_destroy (GTK_WIDGET (data));
  return G_SOURCE_REMOVE;
}

static gboolean
terminal_gdbus_window_restarting (TerminalWindow *window)
{
  GList *tabs = gtk_container_get_children (GTK_CONTAINER (terminal_window_get_notebook (window)));
  gboolean pending = FALSE;
  for (GList *link = tabs; link != NULL; link = link->next)
    pending |= terminal_restart_is_pending (TERMINAL_SCREEN (link->data));
  g_list_free (tabs);
  return pending;
}

static void
terminal_gdbus_tab_restart (TerminalApp *app, GDBusConnection *connection, const gchar *sender,
                            const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation)
{
  guint32 uid = G_MAXUINT32;
  GVariant *reply = sender != NULL ? g_dbus_connection_call_sync (connection,
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetConnectionUnixUser", g_variant_new ("(s)", sender), G_VARIANT_TYPE ("(u)"),
      0, 1000, NULL, NULL) : NULL;
  if (reply != NULL)
    {
      g_variant_get (reply, "(u)", &uid);
      g_variant_unref (reply);
    }
  if (uid != getuid ())
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                             "Tab restart caller must belong to the same user");
      return;
    }
  const gchar *tab, *token = NULL, *cwd = NULL;
  gchar **argv = NULL;
  guint child = 0;
  if (g_str_equal (method, "PrepareTabRestart"))
    g_variant_get (parameters, "(&su^as&s)", &tab, &child, &argv, &cwd);
  else
    g_variant_get (parameters, "(&s&s)", &tab, &token);
  TerminalScreen *screen = terminal_gdbus_find_screen (app, tab);
  GError *error = NULL;
  gchar *result = NULL;
  if (screen == NULL || gtk_widget_in_destruction (GTK_WIDGET (screen)))
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Terminal tab no longer exists");
  else if (g_str_equal (method, "PrepareTabRestart"))
    result = terminal_restart_prepare (screen, sender, child, argv, cwd, &error);
  else if (g_str_equal (method, "CommitTabRestart"))
    terminal_restart_commit (screen, sender, token, &error);
  else if (g_str_equal (method, "CancelTabRestart"))
    terminal_restart_cancel (screen, sender, token, &error);
  else
    result = terminal_restart_status (screen, sender, token, &error);
  if (error != NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    g_dbus_method_invocation_return_value (invocation, result != NULL ? g_variant_new ("(s)", result) : NULL);
  g_free (result);
  g_strfreev (argv);
}

static void
terminal_gdbus_update (TerminalApp *app, GDBusConnection *connection, const gchar *sender,
                      const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation)
{
  const gchar *uuid, *service = NULL;
  TerminalWindow *window = NULL;
  if (g_str_equal (method, "CloseForUpdate"))
    g_variant_get (parameters, "(&s&s)", &uuid, &service);
  else
    g_variant_get (parameters, "(&s)", &uuid);
  for (const GSList *link = terminal_app_get_windows (app); link != NULL; link = link->next)
    if (g_strcmp0 (terminal_window_get_uuid (TERMINAL_WINDOW (link->data)), uuid) == 0)
      window = TERMINAL_WINDOW (link->data);
  if (window == NULL || gtk_widget_in_destruction (GTK_WIDGET (window)))
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                            "Unknown terminal window UUID");
      return;
    }
  GVariant *reply = sender != NULL ? g_dbus_connection_call_sync (connection,
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetConnectionUnixUser", g_variant_new ("(s)", sender), G_VARIANT_TYPE ("(u)"),
      0, 1000, NULL, NULL) : NULL;
  guint32 uid = G_MAXUINT32;
  if (reply != NULL)
    {
      g_variant_get (reply, "(u)", &uid);
      g_variant_unref (reply);
    }
  if (uid != getuid ())
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                            "The update caller must belong to the same user");
      return;
    }
  if (g_str_equal (method, "CloseForUpdate"))
    {
      if (!terminal_update_authorize_close (GTK_WINDOW (window), service, sender))
        g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                              "Only this window's pending update helper can close it");
      else
        {
          g_dbus_method_invocation_return_value (invocation, NULL);
          g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, terminal_gdbus_close_update_window,
                           g_object_ref (window), g_object_unref);
        }
      return;
    }
  TerminalBuildMonitor *monitor = terminal_build_monitor_get ();
  if (g_str_equal (method, "UpdateInPlace"))
    {
      if (terminal_gdbus_window_restarting (window))
        g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_BUSY,
                                              "A program replacement is already using this window");
      else if (!terminal_build_monitor_has_update (monitor) && !terminal_update_is_pending (GTK_WINDOW (window)))
        g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
                                              "No newer terminal executable is available");
      else if (g_strcmp0 (G_OBJECT_TYPE_NAME (window), "TerminalWindowDropdown") == 0)
        g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "Drop-down terminals cannot be updated in place");
      else
        {
          terminal_update_in_place (GTK_WINDOW (window), uuid, monitor);
          g_dbus_method_invocation_return_value (invocation,
              g_variant_new ("(s)", terminal_update_get_service (GTK_WINDOW (window))));
        }
    }
  else
    {
      JsonObject *object = json_object_new ();
      json_object_set_boolean_member (object, "updatable", terminal_build_monitor_has_update (monitor)
          && g_strcmp0 (G_OBJECT_TYPE_NAME (window), "TerminalWindowDropdown") != 0);
      json_object_set_boolean_member (object, "pending", terminal_update_is_pending (GTK_WINDOW (window))
          || terminal_gdbus_window_restarting (window));
      json_object_set_string_member (object, "service", terminal_update_get_service (GTK_WINDOW (window)));
      JsonNode *node = json_node_new (JSON_NODE_OBJECT);
      json_node_take_object (node, object);
      JsonGenerator *generator = json_generator_new ();
      json_generator_set_root (generator, node);
      gchar *json = json_generator_to_data (generator, NULL);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", json));
      g_free (json);
      json_node_free (node);
      g_object_unref (generator);
    }
  g_object_unref (monitor);
}

static void
terminal_gdbus_method_call (GDBusConnection *connection,
                            const gchar *sender,
                            const gchar *object_path,
                            const gchar *interface_name,
                            const gchar *method_name,
                            GVariant *parameters,
                            GDBusMethodInvocation *invocation,
                            gpointer user_data)
{
  TerminalApp *app = TERMINAL_APP (user_data);
  guint32 uid = G_MAXUINT32;
  gchar *display_name = NULL;
  gchar **argv = NULL;
  const gchar *tab_uuid = NULL;
  GError *error = NULL;
  gchar *display_name2;

  g_return_if_fail (TERMINAL_IS_APP (app));
  g_return_if_fail (!g_strcmp0 (object_path, TERMINAL_DBUS_PATH));
  g_return_if_fail (!g_strcmp0 (interface_name, TERMINAL_DBUS_INTERFACE));

  if (g_str_equal (method_name, "FinishUpdate"))
    {
      const gchar *service;
      g_variant_get (parameters, "(&s)", &service);
      GVariant *reply = g_strcmp0 (service, g_getenv ("XFCE4_TERMINAL_UPDATE_SERVICE")) == 0
          ? g_dbus_connection_call_sync (connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
              "org.freedesktop.DBus", "GetNameOwner", g_variant_new ("(s)", service),
              G_VARIANT_TYPE ("(s)"), 0, 1000, NULL, NULL) : NULL;
      const gchar *owner = NULL;
      if (reply != NULL) g_variant_get (reply, "(&s)", &owner);
      if (owner != NULL && g_strcmp0 (owner, sender) == 0)
        {
          g_unsetenv ("XFCE4_TERMINAL_UPDATE_SERVICE");
          g_dbus_method_invocation_return_value (invocation, NULL);
        }
      else
        g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                              "Only the pending update helper can finish startup");
      if (reply != NULL) g_variant_unref (reply);
    }
  else if (g_strcmp0 (method_name, TERMINAL_DBUS_METHOD_LAUNCH) == 0)
    {
      /* get paramenters */
      g_variant_get (parameters, "(u^ay^aay)", &uid, &display_name, &argv);

      display_name2 = terminal_gdbus_display_name ();

      if (uid != getuid ())
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 TERMINAL_ERROR, TERMINAL_ERROR_USER_MISMATCH,
                                                 _("User id mismatch"));
        }
      else if (g_strcmp0 (display_name, display_name2) != 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 TERMINAL_ERROR, TERMINAL_ERROR_DISPLAY_MISMATCH,
                                                 _("Display mismatch"));
        }
      else if (!terminal_app_process (app, argv, g_strv_length (argv), &error))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 TERMINAL_ERROR, TERMINAL_ERROR_OPTIONS,
                                                 "%s", error->message);
          g_error_free (error);
        }
      else
        {
          /* everything went fine */
          g_dbus_method_invocation_return_value (invocation, NULL);
        }

      g_free (display_name);
      g_free (display_name2);
      g_strfreev (argv);
    }
  else if (g_strcmp0 (method_name, TERMINAL_DBUS_METHOD_LIST_TERMINALS) == 0)
    {
      terminal_gdbus_list_terminals (app, invocation);
    }
  else if (g_str_equal (method_name, "PrepareTabRestart") || g_str_equal (method_name, "CommitTabRestart")
           || g_str_equal (method_name, "CancelTabRestart") || g_str_equal (method_name, "GetTabRestartStatus"))
    {
      terminal_gdbus_tab_restart (app, connection, sender, method_name, parameters, invocation);
    }
  else if (g_str_equal (method_name, "GetUpdateStatus") || g_str_equal (method_name, "UpdateInPlace")
           || g_str_equal (method_name, "CloseForUpdate"))
    {
      terminal_gdbus_update (app, connection, sender, method_name, parameters, invocation);
    }
  else if (g_strcmp0 (method_name, TERMINAL_DBUS_METHOD_SEND_ENTER) == 0)
    {
      g_variant_get (parameters, "(&s)", &tab_uuid);
      terminal_gdbus_authorize_send_text (connection, sender, app, tab_uuid,
                                          "\r", invocation);
    }
  else if (g_strcmp0 (method_name, TERMINAL_DBUS_METHOD_SEND_YES) == 0)
    {
      g_variant_get (parameters, "(&s)", &tab_uuid);
      terminal_gdbus_authorize_send_text (connection, sender, app, tab_uuid,
                                          "y", invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method for DBus service " TERMINAL_DBUS_SERVICE);
    }
}



static const GDBusInterfaceVTable terminal_gdbus_vtable = {
  .method_call = terminal_gdbus_method_call,
  .get_property = NULL,
  .set_property = NULL
};



gboolean
terminal_gdbus_register_service (TerminalApp *app,
                                 gboolean register_launch_service,
                                 GError **error)
{
  TerminalGdbusRegistration *registration;
  GDBusNodeInfo *info;
  gchar *uuid;

  g_return_val_if_fail (TERMINAL_IS_APP (app), FALSE);

  registration = g_new0 (TerminalGdbusRegistration, 1);
  registration->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (registration->connection == NULL)
    goto fail;

  info = g_dbus_node_info_new_for_xml (terminal_gdbus_introspection_xml, error);
  if (info == NULL)
    goto fail;

  registration->registration_id =
    g_dbus_connection_register_object (registration->connection,
                                       TERMINAL_DBUS_PATH,
                                       *info->interfaces,
                                       &terminal_gdbus_vtable,
                                       app,
                                       NULL,
                                       error);
  g_dbus_node_info_unref (info);
  if (registration->registration_id == 0)
    goto fail;

  uuid = g_uuid_string_random ();
  g_strdelimit (uuid, "-", '_');
  registration->instance_name = g_strconcat (TERMINAL_DBUS_INSTANCE_PREFIX,
                                             uuid, NULL);
  g_free (uuid);

  registration->instance_owner_id =
    g_bus_own_name_on_connection (registration->connection,
                                  registration->instance_name,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL);

  if (register_launch_service)
    registration->launch_owner_id =
      g_bus_own_name_on_connection (registration->connection,
                                    TERMINAL_DBUS_SERVICE,
                                    G_BUS_NAME_OWNER_FLAGS_NONE,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL);

  g_object_set_data_full (G_OBJECT (app),
                          "terminal-gdbus-registration",
                          registration,
                          terminal_gdbus_registration_free);
  g_debug ("Registered terminal control service %s", registration->instance_name);

  return TRUE;

fail:
  terminal_gdbus_registration_free (registration);
  return FALSE;
}



gboolean
terminal_gdbus_invoke_launch (gint argc,
                              gchar **argv,
                              GError **error)
{
  GVariant *reply;
  GDBusConnection *connection;
  GError *err = NULL;
  gboolean result;
  guint32 uid;
  gchar *display_name;

  g_return_val_if_fail (argc == (gint) g_strv_length (argv), FALSE);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (G_UNLIKELY (connection == NULL))
    return FALSE;

  /* store in an uin32 for gvariant */
  uid = getuid ();
  display_name = terminal_gdbus_display_name ();

  reply = g_dbus_connection_call_sync (connection,
                                       TERMINAL_DBUS_SERVICE,
                                       TERMINAL_DBUS_PATH,
                                       TERMINAL_DBUS_INTERFACE,
                                       TERMINAL_DBUS_METHOD_LAUNCH,
                                       g_variant_new ("(u^ay^aay)",
                                                      uid,
                                                      display_name,
                                                      argv),
                                       NULL,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                       2000,
                                       NULL,
                                       &err);

  g_object_unref (connection);
  g_free (display_name);

  result = (reply != NULL);
  if (G_LIKELY (result))
    g_variant_unref (reply);
  else
    g_propagate_error (error, err);

  return result;
}
