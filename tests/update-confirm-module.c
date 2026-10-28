/* Test-only GTK module: accept update confirmation in the private compositor.
 */
#include <gmodule.h>
#include <gtk/gtk.h>

G_MODULE_EXPORT void
gtk_module_init (gint *argc, gchar ***argv);

static gboolean
accept_update (gpointer data)
{
  const gchar *log = g_getenv ("XFCE_UPDATE_TEST_CONFIRM_LOG");
  if (log != NULL)
    g_file_set_contents (log, "confirmed\n", -1, NULL);
  gtk_dialog_response (GTK_DIALOG (data), GTK_RESPONSE_ACCEPT);
  return G_SOURCE_REMOVE;
}

static gboolean
dialog_shown (GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
  GtkWidget *widget = g_value_get_object (&values[0]);
  if (GTK_IS_MESSAGE_DIALOG (widget)
      && g_strcmp0 (gtk_window_get_title (GTK_WINDOW (widget)),
                    "Update in Place")
           == 0
      && gtk_dialog_get_widget_for_response (GTK_DIALOG (widget),
                                             GTK_RESPONSE_ACCEPT)
           != NULL)
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, accept_update,
                     g_object_ref (widget), g_object_unref);
  return TRUE;
}

G_MODULE_EXPORT void
gtk_module_init (gint *argc, gchar ***argv)
{
  g_type_class_ref (GTK_TYPE_WIDGET);
  g_signal_add_emission_hook (g_signal_lookup ("show", GTK_TYPE_WIDGET), 0,
                              dialog_shown, NULL, NULL);
}
