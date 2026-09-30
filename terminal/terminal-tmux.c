#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "terminal-tmux.h"

#ifdef __linux__
typedef struct
{
  GMainLoop *loop;
  GSubprocess *process;
  GCancellable *cancel;
  gchar *output;
  gboolean success;
} TmuxQuery;

static void
query_finished (GObject *source, GAsyncResult *result, gpointer data)
{
  TmuxQuery *query = data;
  query->success = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result,
                                                         &query->output, NULL, NULL);
  g_main_loop_quit (query->loop);
}

static gboolean
query_expired (gpointer data)
{
  TmuxQuery *query = data;
  g_subprocess_force_exit (query->process);
  g_cancellable_cancel (query->cancel);
  return G_SOURCE_REMOVE;
}

static gchar *
query_clients (const gchar *executable, const gchar *socket)
{
  GMainContext *context = g_main_context_new ();
  GSource *timeout = g_timeout_source_new (250);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                             | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  TmuxQuery query = { 0 };

  /* No shell, server startup, configuration reload, or default socket lookup. */
  g_subprocess_launcher_unsetenv (launcher, "TMUX");
  g_subprocess_launcher_unsetenv (launcher, "TMUX_PANE");
  query.process = g_subprocess_launcher_spawn (launcher, NULL, executable, "-N", "-S", socket,
                                               "list-clients", "-F", "#{client_pid} #{pane_pid}", NULL);
  if (query.process != NULL)
    {
      query.cancel = g_cancellable_new ();
      query.loop = g_main_loop_new (context, FALSE);
      /* A private context bounds a click without dispatching unrelated UI events. */
      g_main_context_push_thread_default (context);
      g_source_set_callback (timeout, query_expired, &query, NULL);
      g_source_attach (timeout, context);
      g_subprocess_communicate_utf8_async (query.process, NULL, query.cancel, query_finished, &query);
      g_main_loop_run (query.loop);
      g_source_destroy (timeout);
      if (!query.success)
        g_subprocess_force_exit (query.process);
      g_subprocess_wait (query.process, NULL, NULL);
      if (!query.success || !g_subprocess_get_successful (query.process))
        g_clear_pointer (&query.output, g_free);
      g_main_context_pop_thread_default (context);
      g_main_loop_unref (query.loop);
      g_object_unref (query.cancel);
      g_object_unref (query.process);
    }
  g_source_unref (timeout);
  g_main_context_unref (context);
  g_object_unref (launcher);
  return query.output;
}

static gchar *
client_socket (pid_t pid)
{
  gchar *path = g_strdup_printf ("/proc/%d/cmdline", (gint) pid);
  gchar *contents = NULL, *socket = NULL;
  gsize length;
  struct stat st;

  if (!g_file_get_contents (path, &contents, &length, NULL)
      || length == 0 || length > 65536)
    goto out;
  for (gsize offset = strnlen (contents, length) + 1; offset < length;)
    {
      const gchar *arg = contents + offset;
      gsize size = strnlen (arg, length - offset);
      offset += size + 1;
      if (arg[0] != '-' || strcmp (arg, "--") == 0)
        break;
      if (strcmp (arg, "-S") == 0 && offset < length)
        {
          g_clear_pointer (&socket, g_free);
          socket = g_strndup (contents + offset, strnlen (contents + offset, length - offset));
          offset += strlen (socket) + 1;
        }
      else if (g_str_has_prefix (arg, "-S") && size > 2)
        {
          g_free (socket);
          socket = g_strdup (arg + 2);
        }
      else if (strcmp (arg, "-f") == 0 || strcmp (arg, "-L") == 0
               || strcmp (arg, "-c") == 0 || strcmp (arg, "-T") == 0)
        {
          if (offset < length)
            offset += strnlen (contents + offset, length - offset) + 1;
        }
    }
  if (socket != NULL
      && (!g_path_is_absolute (socket) || g_stat (socket, &st) != 0
          || !S_ISSOCK (st.st_mode) || st.st_uid != getuid ()))
    g_clear_pointer (&socket, g_free);
out:
  g_free (contents);
  g_free (path);
  return socket;
}
#endif

pid_t
terminal_tmux_pane_foreground_pid (pid_t client_pid)
{
#ifdef __linux__
  gchar *socket = client_socket (client_pid);
  gchar *executable, *output, **lines;
  pid_t result = 0;

  if (socket == NULL)
    return 0;
  /* Use the attached client's binary, not a possibly incompatible PATH tmux. */
  executable = g_strdup_printf ("/proc/%d/exe", (gint) client_pid);
  output = query_clients (executable, socket);
  g_free (executable);
  g_free (socket);
  if (output == NULL)
    return 0;
  lines = g_strsplit (output, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    {
      long client, pane, parent, group, session, tty, foreground;
      char extra, state;
      gchar *path, *stat = NULL, *fields;

      if (sscanf (lines[i], "%ld %ld %c", &client, &pane, &extra) != 2
          || client != client_pid || pane <= 0 || pane > G_MAXINT)
        continue;
      path = g_strdup_printf ("/proc/%ld/stat", pane);
      if (g_file_get_contents (path, &stat, NULL, NULL)
          && (fields = strrchr (stat, ')')) != NULL
          && sscanf (fields + 1, " %c %ld %ld %ld %ld %ld", &state, &parent, &group,
                     &session, &tty, &foreground)
               == 6
          && foreground > 0 && foreground <= G_MAXINT)
        result = (pid_t) foreground;
      g_free (stat);
      g_free (path);
      break;
    }
  g_strfreev (lines);
  g_free (output);
  return result;
#else
  (void) client_pid;
  return 0;
#endif
}
