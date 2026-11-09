#include <json-glib/json-glib.h>
#include <unistd.h>

#include "terminal-freshness-client.h"

#define SERVICE "com.terrydaktal.ApplicationLauncher"
#define PATH "/Freshness"
#define INTERFACE "com.terrydaktal.ApplicationLauncher.Freshness1"

struct _TerminalFreshnessClient
{
  GObject parent;
  GDBusConnection *connection;
  GCancellable *cancel;
  gchar *owner, *launch_path;
  GPid pid;
  guint64 start, dev, ino, revision;
  guint generation, watch, subscription, heartbeat;
  gboolean known, stale, checking;
};

typedef struct
{
  TerminalFreshnessClient *client;
  guint generation;
} Request;

G_DEFINE_TYPE (TerminalFreshnessClient, terminal_freshness_client, G_TYPE_OBJECT)

static void
refresh (TerminalFreshnessClient *client);

static void
set_status (TerminalFreshnessClient *client, gboolean known, gboolean stale)
{
  if (client->known != known || client->stale != stale)
    {
      client->known = known;
      client->stale = stale;
      g_signal_emit_by_name (client, "changed");
    }
}

static const gchar *
string_member (JsonObject *object, const gchar *name)
{
  JsonNode *node = json_object_get_member (object, name);
  return node != NULL && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
           ? json_node_get_string (node)
           : NULL;
}

static gboolean
integer_matches (JsonObject *object, const gchar *name, guint64 expected)
{
  JsonNode *node = json_object_get_member (object, name);
  return node != NULL && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_INT64
         && json_node_get_int (node) >= 0 && (guint64) json_node_get_int (node) == expected;
}

static gboolean
apply_report (TerminalFreshnessClient *client, const gchar *json)
{
  if (strlen (json) > 16384)
    return FALSE;
  JsonParser *parser = json_parser_new ();
  gboolean valid = FALSE;
  if (json_parser_load_from_data (parser, json, -1, NULL))
    {
      JsonNode *node = json_parser_get_root (parser);
      JsonObject *report = node != NULL && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
      node = report != NULL ? json_object_get_member (report, "target") : NULL;
      JsonObject *target = node != NULL && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
      const gchar *status = report != NULL ? string_member (report, "status") : NULL;
      JsonNode *revision = report != NULL ? json_object_get_member (report, "generation") : NULL;
      if (target != NULL && integer_matches (report, "schema_version", 1)
          && integer_matches (target, "pid", client->pid)
          && integer_matches (target, "start_time", client->start)
          && integer_matches (target, "image_dev", client->dev)
          && integer_matches (target, "image_ino", client->ino)
          && g_strcmp0 (string_member (target, "launch_path"), client->launch_path) == 0
          && revision != NULL && JSON_NODE_HOLDS_VALUE (revision)
          && json_node_get_value_type (revision) == G_TYPE_INT64 && json_node_get_int (revision) >= 0
          && (g_strcmp0 (status, "unknown") == 0 || g_strcmp0 (status, "current") == 0 || g_strcmp0 (status, "outdated") == 0))
        {
          gboolean unknown = g_str_equal (status, "unknown");
          valid = unknown || ((client->start == 0 || integer_matches (report, "start_time", client->start)) && (client->dev == 0 || integer_matches (report, "image_dev", client->dev)) && (client->ino == 0 || integer_matches (report, "image_ino", client->ino)));
          if (valid)
            {
              if ((guint64) json_node_get_int (revision) >= client->revision)
                {
                  client->revision = json_node_get_int (revision);
                  set_status (client, !unknown, g_str_equal (status, "outdated"));
                }
            }
        }
    }
  g_object_unref (parser);
  return valid;
}

static void
reply_received (GObject *source, GAsyncResult *result, gpointer data)
{
  Request *request = data;
  TerminalFreshnessClient *client = request->client;
  GVariant *reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  if (request->generation == client->generation)
    {
      client->checking = FALSE;
      const gchar *json = NULL;
      if (reply != NULL)
        g_variant_get (reply, "(&s)", &json);
      if (json == NULL || !apply_report (client, json))
        set_status (client, FALSE, FALSE);
    }
  g_clear_pointer (&reply, g_variant_unref);
  g_object_unref (client);
  g_free (request);
}

static void
refresh (TerminalFreshnessClient *client)
{
  if (client->checking || client->pid <= 1 || client->owner == NULL || client->connection == NULL)
    return;
  Request *request = g_new0 (Request, 1);
  request->client = g_object_ref (client);
  request->generation = client->generation;
  client->checking = TRUE;
  g_dbus_connection_call (client->connection, client->owner, PATH, INTERFACE, "GetProcessStatus",
                          g_variant_new ("(uttts)", (guint) client->pid, client->start, client->dev, client->ino, client->launch_path),
                          G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, client->cancel,
                          reply_received, request);
}

static void
status_changed (GDBusConnection *connection, const gchar *sender, const gchar *path, const gchar *interface, const gchar *signal, GVariant *parameters, gpointer data)
{
  TerminalFreshnessClient *client = data;
  if (g_strcmp0 (sender, client->owner) != 0)
    return;
  if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(s)")))
    {
      const gchar *json;
      g_variant_get (parameters, "(&s)", &json);
      apply_report (client, json);
    }
}

static void
reset (TerminalFreshnessClient *client)
{
  client->generation++;
  client->revision = 0;
  g_cancellable_cancel (client->cancel);
  g_clear_object (&client->cancel);
  client->cancel = g_cancellable_new ();
  client->checking = FALSE;
  set_status (client, FALSE, FALSE);
}

static void
disconnect_service (TerminalFreshnessClient *client)
{
  if (client->subscription != 0)
    g_dbus_connection_signal_unsubscribe (client->connection, client->subscription);
  client->subscription = 0;
  g_clear_object (&client->connection);
  g_clear_pointer (&client->owner, g_free);
  reset (client);
}

static void
appeared (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer data)
{
  TerminalFreshnessClient *client = data;
  disconnect_service (client);
  client->connection = g_object_ref (connection);
  client->owner = g_strdup (owner);
  client->subscription = g_dbus_connection_signal_subscribe (
    connection, owner, INTERFACE, "ProcessStatusChanged", PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, status_changed, client, NULL);
  refresh (client);
}

static void
vanished (GDBusConnection *connection, const gchar *name, gpointer data)
{
  disconnect_service (data);
}

static gboolean
heartbeat (gpointer data)
{
  refresh (data);
  return G_SOURCE_CONTINUE;
}

static void
finalize (GObject *object)
{
  TerminalFreshnessClient *client = TERMINAL_FRESHNESS_CLIENT (object);
  g_bus_unwatch_name (client->watch);
  g_source_remove (client->heartbeat);
  disconnect_service (client);
  g_clear_object (&client->cancel);
  g_free (client->launch_path);
  G_OBJECT_CLASS (terminal_freshness_client_parent_class)->finalize (object);
}

static void
terminal_freshness_client_class_init (TerminalFreshnessClientClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = finalize;
  g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
terminal_freshness_client_init (TerminalFreshnessClient *client)
{
  client->cancel = g_cancellable_new ();
  client->launch_path = g_strdup ("");
  client->watch = g_bus_watch_name (G_BUS_TYPE_SESSION, SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, client, NULL);
  client->heartbeat = g_timeout_add_seconds (20, heartbeat, client);
}

TerminalFreshnessClient *
terminal_freshness_client_new (void)
{
  return g_object_new (TERMINAL_TYPE_FRESHNESS_CLIENT, NULL);
}

void
terminal_freshness_client_set_target (TerminalFreshnessClient *client, GPid pid, guint64 start_time, guint64 image_dev, guint64 image_ino, const gchar *launch_path)
{
  const gchar *path = launch_path != NULL ? launch_path : "";
  if (client->pid == pid && client->start == start_time && client->dev == image_dev && client->ino == image_ino
      && g_str_equal (client->launch_path, path))
    return;
  reset (client);
  client->pid = pid;
  client->start = start_time;
  client->dev = image_dev;
  client->ino = image_ino;
  g_free (client->launch_path);
  client->launch_path = g_strdup (path);
  refresh (client);
}

gboolean
terminal_freshness_client_is_known (TerminalFreshnessClient *client)
{
  return client->known;
}

gboolean
terminal_freshness_client_is_stale (TerminalFreshnessClient *client)
{
  return client->known && client->stale;
}
