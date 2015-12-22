/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2015 Centricular Ltd
 *  Author(s): Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ovg-app.h"
#include "ovg-appwin.h"

#include "onevideo/utils.h"

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

struct _OvgApp
{
  GtkApplication parent;
};

struct _OvgAppClass
{
  GtkApplicationClass parent_class;
};

struct _OvgAppPrivate
{
  OvLocalPeer *ov_local;
};

G_DEFINE_TYPE_WITH_PRIVATE (OvgApp, ovg_app, GTK_TYPE_APPLICATION);

#ifdef __linux__
static gchar *device_path = NULL;
#endif
static gchar *iface_name = NULL;
static guint16 iface_port = 0;

static GOptionEntry app_options[] =
{
#ifdef __linux__
  {"device", 'd', 0, G_OPTION_ARG_STRING, &device_path, "Path to the V4L2"
          " (camera) device; example: /dev/video0", "PATH"},
#endif
  {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
        " to listen on (default: all)", "NAME"},
  {"port", 'p', 0, G_OPTION_ARG_INT, &iface_port, "Override the TCP port to"
        " listen on for incoming connections", "PORT"},
  {NULL}
};

static void
quit_activated (GSimpleAction * action, GVariant * param, gpointer app)
{
  g_application_quit (G_APPLICATION (app));
}

static GActionEntry app_entries[] =
{
  { "quit", quit_activated, NULL, NULL, NULL },
};

#ifdef G_OS_UNIX
static gboolean
on_ovg_app_sigint (GApplication * app)
{
  g_printerr ("SIGINT caught, quitting application...");
  quit_activated (NULL, NULL, app);

  return G_SOURCE_REMOVE;
}
#endif

static void
ovg_app_init (OvgApp * app)
{
  g_set_prgname ("OneVideo");
  g_set_application_name ("OneVideo");
  gtk_window_set_default_icon_name ("OneVideo");
  g_application_add_main_option_entries (G_APPLICATION (app), app_options);
}

static void
ovg_app_activate (GApplication * app)
{
  GList *l;
  GtkWidget *win;

  g_return_if_fail (OVG_IS_APP (app));

  l = gtk_application_get_windows (GTK_APPLICATION (app));
  for (; l; l = l->next)
    if (OVG_IS_APP_WINDOW (l->data))
      return gtk_window_present (GTK_WINDOW (l->data));

  win = ovg_app_window_new (OVG_APP (app));
  gtk_window_present (GTK_WINDOW (win));
}

static void
ovg_app_startup (GApplication * app)
{
  GList *devices;
  GtkBuilder *builder;
  GMenuModel *app_menu;
  GstDevice *device = NULL;
  const gchar *quit_accels[2] = { "<Ctrl>Q", NULL };
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (app));

  G_APPLICATION_CLASS (ovg_app_parent_class)->startup (app);

  /* Setup app menu and accels */
  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   app_entries, G_N_ELEMENTS (app_entries),
                                   app);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "app.quit",
                                         quit_accels);

  builder = gtk_builder_new_from_resource ("/org/gtk/OneVideoGui/ovg-appmenu.ui");
  app_menu = G_MENU_MODEL (gtk_builder_get_object (builder, "appmenu"));
  gtk_application_set_app_menu (GTK_APPLICATION (app), app_menu);
  g_object_unref (builder);

  /* Initiate OneVideo library; listen on all interfaces and default port */
  gst_init (NULL, NULL);

  /* This probes available devices at start, so start-up can be slow */
  priv->ov_local = ov_local_peer_new (iface_name, iface_port);
  if (priv->ov_local == NULL) {
    /* FIXME: Print some GUI message */
    g_application_quit (app);
    return;
  }

  if (!ov_local_peer_start (priv->ov_local)) {
    /* FIXME: Print some GUI message */
    g_application_quit (app);
    return;
  }

  devices = ov_local_peer_get_video_devices (priv->ov_local);
#ifdef __linux__
  device = ov_get_device_from_device_path (devices, device_path);
#else
  if (device_path != NULL)
    GST_WARNING ("The -d/--device option is not supported on this platform;"
        " selecting the first available video device");
#endif
  /* TODO: Just use the first device for now.
   * Need to create GSettings for this. */
  if (device == NULL)
    device = GST_DEVICE (devices->data);

  /* This currently always returns TRUE (aborts on error) */
  ov_local_peer_set_video_device (priv->ov_local, device);
  g_list_free_full (devices, g_object_unref);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, (GSourceFunc) on_ovg_app_sigint, app);
#endif
}

static gint
ovg_app_command_line (GApplication * app, GApplicationCommandLine * cmdline)
{
  /* No command-line options; just raise */
  ovg_app_activate (app);
  return 0;
}

static void
ovg_app_shutdown (GApplication * app)
{
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (app));

  if (priv->ov_local)
    ov_local_peer_stop (priv->ov_local);

  G_APPLICATION_CLASS (ovg_app_parent_class)->shutdown (app);
}

static void
ovg_app_dispose (GObject * object)
{
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (object));

  g_clear_object (&priv->ov_local);

  G_OBJECT_CLASS (ovg_app_parent_class)->dispose (object);
}

static void
ovg_app_class_init (OvgAppClass * class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GApplicationClass *application_class = G_APPLICATION_CLASS (class);

  application_class->activate = ovg_app_activate;
  application_class->startup = ovg_app_startup;
  application_class->command_line = ovg_app_command_line;
  application_class->shutdown = ovg_app_shutdown;

  object_class->dispose = ovg_app_dispose;
}

OvgApp *
ovg_app_new (void)
{
  return g_object_new (OVG_TYPE_APP, "application-id", "org.gtk.OneVideoGui",
      "flags", G_APPLICATION_HANDLES_OPEN, NULL);
}


OvLocalPeer *
ovg_app_get_ov_local_peer (OvgApp * app)
{
  OvgAppPrivate *priv;

  g_return_val_if_fail (OVG_IS_APP (app), NULL);

  priv = ovg_app_get_instance_private (app);

  return priv->ov_local;
}
