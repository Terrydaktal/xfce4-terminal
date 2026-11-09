#ifndef FRESHNESS_TEST_SERVICE_H
#define FRESHNESS_TEST_SERVICE_H

#include <gio/gio.h>

static GSubprocess *
freshness_test_service_start (void)
{
  GError *error = NULL;
  const gchar *program = g_getenv ("TERMINAL_TEST_FRESHNESS_SERVICE");
  GSubprocess *service = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDIN_PIPE,
                                           &error, program != NULL ? program : TEST_FRESHNESS_SERVICE, NULL);
  g_assert_no_error (error);
  GDataInputStream *stream = g_data_input_stream_new (g_subprocess_get_stdout_pipe (service));
  gchar *ready = g_data_input_stream_read_line (stream, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (ready, ==, "READY");
  g_free (ready);
  g_object_unref (stream);
  return service;
}

static void
freshness_test_service_stop (GSubprocess *service)
{
  g_subprocess_force_exit (service);
  g_assert_true (g_subprocess_wait (service, NULL, NULL));
  g_object_unref (service);
}

#endif
