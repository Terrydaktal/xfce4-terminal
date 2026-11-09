#include <json-glib/json-glib.h>
#include <unistd.h>

#include "terminal/terminal-freshness-client.h"

#define SERVICE "com.terrydaktal.ApplicationLauncher"
#define INTERFACE SERVICE ".Freshness1"

typedef enum
{
  NORMAL,
  ABSENT,
  MALFORMED,
  WRONG_IDENTITY,
  HOLD
} Mode;
typedef struct
{
  TerminalFreshnessClient *client;
  GDBusConnection *server;
  GDBusMethodInvocation *held;
  guint registration, calls;
  Mode mode;
  const gchar *status;
  guint64 revision;
} Fixture;

static GTestDBus *bus;

static void
pump (guint milliseconds)
{
  gint64 deadline = g_get_monotonic_time () + milliseconds * 1000;
  do
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);
}

static gchar *
report (GVariant *parameters, const gchar *status, guint64 revision, gboolean wrong)
{
  guint pid;
  guint64 start, dev, ino;
  const gchar *path;
  g_variant_get (parameters, "(uttt&s)", &pid, &start, &dev, &ino, &path);
  JsonObject *target = json_object_new ();
  json_object_set_int_member (target, "pid", pid + wrong);
  json_object_set_int_member (target, "start_time", start);
  json_object_set_int_member (target, "image_dev", dev);
  json_object_set_int_member (target, "image_ino", ino);
  json_object_set_string_member (target, "launch_path", path);
  JsonObject *object = json_object_new ();
  json_object_set_object_member (object, "target", target);
  json_object_set_int_member (object, "schema_version", 1);
  json_object_set_int_member (object, "generation", revision);
  json_object_set_string_member (object, "status", status);
  json_object_set_int_member (object, "start_time", start);
  json_object_set_int_member (object, "image_dev", dev);
  json_object_set_int_member (object, "image_ino", ino);
  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (node, object);
  gchar *json = json_to_string (node, FALSE);
  json_node_free (node);
  return json;
}

static void
method (GDBusConnection *connection, const gchar *sender, const gchar *path, const gchar *interface, const gchar *name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Fixture *fixture = data;
  fixture->calls++;
  if (fixture->mode == HOLD)
    {
      g_assert_null (fixture->held);
      fixture->held = g_object_ref (invocation);
      return;
    }
  gchar *json = fixture->mode == MALFORMED ? g_strdup ("not JSON")
                                           : report (parameters, fixture->status, fixture->revision, fixture->mode == WRONG_IDENTITY);
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", json));
  g_free (json);
}

static void
start_server (Fixture *fixture)
{
  GError *error = NULL;
  fixture->server = g_dbus_connection_new_for_address_sync (g_test_dbus_get_bus_address (bus),
                                                            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                            NULL, NULL, &error);
  g_assert_no_error (error);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (
    "<node><interface name='" INTERFACE "'><method name='GetProcessStatus'>"
    "<arg type='u' direction='in'/><arg type='t' direction='in'/><arg type='t' direction='in'/>"
    "<arg type='t' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/>"
    "</method><signal name='ProcessStatusChanged'><arg type='s'/></signal></interface></node>",
    &error);
  g_assert_no_error (error);
  static const GDBusInterfaceVTable table = { .method_call = method };
  fixture->registration = g_dbus_connection_register_object (fixture->server, "/Freshness",
                                                             node->interfaces[0], &table, fixture, NULL, &error);
  g_assert_no_error (error);
  g_dbus_node_info_unref (node);
  GVariant *reply = g_dbus_connection_call_sync (fixture->server, "org.freedesktop.DBus",
                                                 "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new ("(su)", SERVICE, 4),
                                                 G_VARIANT_TYPE ("(u)"), 0, 1000, NULL, &error);
  g_assert_no_error (error);
  guint result;
  g_variant_get (reply, "(u)", &result);
  g_assert_cmpuint (result, ==, 1);
  g_variant_unref (reply);
}

static void
stop_server (Fixture *fixture)
{
  if (fixture->held != NULL)
    {
      g_dbus_method_invocation_return_error (fixture->held, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Fixture ended");
      g_clear_object (&fixture->held);
    }
  if (fixture->server != NULL)
    {
      g_dbus_connection_unregister_object (fixture->server, fixture->registration);
      g_assert_true (g_dbus_connection_close_sync (fixture->server, NULL, NULL));
      g_clear_object (&fixture->server);
    }
}

static void
setup (Fixture *fixture, gconstpointer data)
{
  fixture->mode = GPOINTER_TO_INT (data);
  fixture->status = "current";
  fixture->revision = 1;
  if (fixture->mode != ABSENT)
    start_server (fixture);
  fixture->client = terminal_freshness_client_new ();
  terminal_freshness_client_set_target (fixture->client, getpid (), 44, 55, 66, "/not-a-local-freshness-probe");
}

static void
teardown (Fixture *fixture, gconstpointer data)
{
  terminal_freshness_client_set_target (fixture->client, 0, 0, 0, 0, NULL);
  g_clear_object (&fixture->client);
  stop_server (fixture);
  pump (30);
}

static void
wait_state (Fixture *fixture, gboolean known, gboolean stale)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  do
    {
      pump (10);
    }
  while ((terminal_freshness_client_is_known (fixture->client) != known
          || terminal_freshness_client_is_stale (fixture->client) != stale)
         && g_get_monotonic_time () < deadline);
  g_assert_cmpint (terminal_freshness_client_is_known (fixture->client), ==, known);
  g_assert_cmpint (terminal_freshness_client_is_stale (fixture->client), ==, stale);
}

static void
send_status (Fixture *fixture, const gchar *status, guint64 revision)
{
  GVariant *parameters = g_variant_ref_sink (g_variant_new ("(uttts)", (guint) getpid (), (guint64) 44,
                                                            (guint64) 55, (guint64) 66, "/not-a-local-freshness-probe"));
  gchar *json = report (parameters, status, revision, FALSE);
  g_assert_true (g_dbus_connection_emit_signal (fixture->server, NULL, "/Freshness", INTERFACE,
                                                "ProcessStatusChanged", g_variant_new ("(s)", json), NULL));
  g_variant_unref (parameters);
  g_free (json);
}

static void
normal (Fixture *fixture, gconstpointer data)
{
  wait_state (fixture, TRUE, FALSE);
  send_status (fixture, "outdated", 2);
  wait_state (fixture, TRUE, TRUE);
  send_status (fixture, "current", 1);
  pump (40);
  g_assert_true (terminal_freshness_client_is_stale (fixture->client));
  send_status (fixture, "current", 3);
  wait_state (fixture, TRUE, FALSE);
}

static void
unknown (Fixture *fixture, gconstpointer data)
{
  pump (150);
  g_assert_false (terminal_freshness_client_is_known (fixture->client));
  g_assert_false (terminal_freshness_client_is_stale (fixture->client));
  g_assert_cmpuint (fixture->calls, ==, fixture->mode == ABSENT ? 0 : 1);
}

static void
restart (Fixture *fixture, gconstpointer data)
{
  wait_state (fixture, TRUE, FALSE);
  send_status (fixture, "outdated", 2);
  wait_state (fixture, TRUE, TRUE);
  stop_server (fixture);
  wait_state (fixture, FALSE, FALSE);
  start_server (fixture);
  wait_state (fixture, TRUE, FALSE);
}

static void
delayed (Fixture *fixture, gconstpointer data)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  while (fixture->held == NULL && g_get_monotonic_time () < deadline)
    pump (10);
  g_assert_nonnull (fixture->held);
  send_status (fixture, "outdated", 2);
  wait_state (fixture, TRUE, TRUE);
  gchar *json = report (g_dbus_method_invocation_get_parameters (fixture->held), "current", 1, FALSE);
  g_dbus_method_invocation_return_value (fixture->held, g_variant_new ("(s)", json));
  g_clear_object (&fixture->held);
  g_free (json);
  pump (50);
  g_assert_true (terminal_freshness_client_is_stale (fixture->client));
}

static void
target_switch (Fixture *fixture, gconstpointer data)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  while (fixture->held == NULL && g_get_monotonic_time () < deadline)
    pump (10);
  g_assert_nonnull (fixture->held);
  fixture->mode = NORMAL;
  terminal_freshness_client_set_target (fixture->client, getpid (), 77, 88, 99, "/different-tab");
  wait_state (fixture, TRUE, FALSE);
  gchar *json = report (g_dbus_method_invocation_get_parameters (fixture->held), "outdated", 100, FALSE);
  g_dbus_method_invocation_return_value (fixture->held, g_variant_new ("(s)", json));
  g_clear_object (&fixture->held);
  g_free (json);
  pump (50);
  g_assert_false (terminal_freshness_client_is_stale (fixture->client));
}

static void
unowned_signal (Fixture *fixture, gconstpointer data)
{
  wait_state (fixture, TRUE, FALSE);
  GDBusConnection *owner = fixture->server;
  GError *error = NULL;
  fixture->server = g_dbus_connection_new_for_address_sync (g_test_dbus_get_bus_address (bus),
                                                            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                            NULL, NULL, &error);
  g_assert_no_error (error);
  send_status (fixture, "outdated", 100);
  g_dbus_connection_flush_sync (fixture->server, NULL, NULL);
  pump (80);
  g_assert_false (terminal_freshness_client_is_stale (fixture->client));
  g_dbus_connection_close_sync (fixture->server, NULL, NULL);
  g_object_unref (fixture->server);
  fixture->server = owner;
  send_status (fixture, "outdated", 2);
  wait_state (fixture, TRUE, TRUE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
  g_test_add ("/freshness/cached-signal-and-ordering", Fixture, GINT_TO_POINTER (NORMAL), setup, normal, teardown);
  g_test_add ("/freshness/no-daemon-no-local-fallback", Fixture, GINT_TO_POINTER (ABSENT), setup, unknown, teardown);
  g_test_add ("/freshness/malformed-unknown", Fixture, GINT_TO_POINTER (MALFORMED), setup, unknown, teardown);
  g_test_add ("/freshness/wrong-identity-unknown", Fixture, GINT_TO_POINTER (WRONG_IDENTITY), setup, unknown, teardown);
  g_test_add ("/freshness/daemon-restart", Fixture, GINT_TO_POINTER (NORMAL), setup, restart, teardown);
  g_test_add ("/freshness/late-reply-after-signal", Fixture, GINT_TO_POINTER (HOLD), setup, delayed, teardown);
  g_test_add ("/freshness/late-reply-after-tab-switch", Fixture, GINT_TO_POINTER (HOLD), setup, target_switch, teardown);
  g_test_add ("/freshness/non-owner-signal-ignored", Fixture, GINT_TO_POINTER (NORMAL), setup, unowned_signal, teardown);
  gint result = g_test_run ();
  g_test_dbus_down (bus);
  g_object_unref (bus);
  return result;
}
