/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "dr-window.h"

static void
on_activate (GApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (window == NULL)
    window = GTK_WINDOW (dr_window_new (GTK_APPLICATION (app)));
  gtk_window_present (window);
}

/* "direct-rdp rdp://my-pc" or "direct-rdp vnc://my-mac" connects right away. */
static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint)
{
  on_activate (app);
  for (int i = 0; i < n_files; i++)
    {
      g_autofree char *uri = g_file_get_uri (files[i]);
      dr_window_open_address (DR_WINDOW (gtk_application_get_active_window (GTK_APPLICATION (app))),
                              uri);
    }
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = NULL;

  /* FreeRDP logs every certificate and NLA step to stderr; keep errors only. */
  g_setenv ("WLOG_LEVEL", "ERROR", FALSE);
  g_set_application_name ("Direct RDP & VNC");
  app = adw_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN | G_APPLICATION_CAN_OVERRIDE_APP_ID);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
