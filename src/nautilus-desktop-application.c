/* nautilus-desktop-application.c
 *
 * Copyright (C) 2016 Carlos Soriano <csoriano@gnome.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "nautilus-desktop-application.h"
#include "nautilus-desktop-window.h"
#include "nautilus-desktop-directory.h"

#include "nautilus-freedesktop-generated.h"

#include <libnautilus-private/nautilus-global-preferences.h>
#include <eel/eel.h>
#include <gdk/gdkx.h>

static NautilusFreedesktopFileManager1 *freedesktop_proxy = NULL;
static NautilusDirectory *desktop_directory = NULL;

struct _NautilusDesktopApplication
{
  NautilusApplication parent_instance;

  GCancellable *freedesktop_cancellable;
  GList *pending_locations;
};

G_DEFINE_TYPE (NautilusDesktopApplication, nautilus_desktop_application, NAUTILUS_TYPE_APPLICATION)

static void
on_show_folders (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  GError *error = NULL;

  nautilus_freedesktop_file_manager1_call_show_items_finish (freedesktop_proxy,
                                                             res,
                                                             &error);
  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Unable to show items with File Manager freedesktop proxy: %s", error->message);
        }
      g_error_free (error);
    }
}

static void
open_location_on_dbus (NautilusDesktopApplication *self,
                       const gchar                *uri)
{
  const gchar *uris[] = { uri, NULL };

  nautilus_freedesktop_file_manager1_call_show_folders (freedesktop_proxy,
                                                        uris,
                                                        "",
                                                        self->freedesktop_cancellable,
                                                        on_show_folders,
                                                        self);
}


static void
on_freedesktop_bus_proxy_created (GObject      *source_object,
                                  GAsyncResult *res,
                                  gpointer      user_data)
{
  GError *error = NULL;

  freedesktop_proxy = nautilus_freedesktop_file_manager1_proxy_new_for_bus_finish (res, &error);

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Unable to create File Manager freedesktop proxy: %s", error->message);
        }
      g_error_free (error);
    }
}

static void
open_location_full (NautilusApplication     *app,
                    GFile                   *location,
                    NautilusWindowOpenFlags  flags,
                    GList                   *selection,
                    NautilusWindow          *target_window,
                    NautilusWindowSlot      *target_slot)
{
  NautilusDesktopApplication *self = NAUTILUS_DESKTOP_APPLICATION (app);
  gchar *uri;

  uri = g_file_get_uri (location);
  g_print ("open location full %s\n", g_file_get_uri (location));
  if (eel_uri_is_desktop (uri) && target_window &&
      NAUTILUS_IS_DESKTOP_WINDOW (target_window))
    {
      nautilus_window_open_location_full (target_window, location, flags, selection, NULL);
    }
  else
    {
      g_print ("other location, use dbus to communicate with nautilus. This process is only for the desktop\n");
      if (freedesktop_proxy)
        {
          open_location_on_dbus (self, uri);
        }
      else
        {
          g_warning ("cannot open folder on desktop, freedesktop bus not ready\n");
        }
    }

  g_free (uri);
}

static void
nautilus_application_set_desktop_visible (NautilusDesktopApplication *self,
                                          gboolean                    visible)
{
  GtkWidget *desktop_window;

  if (visible)
    {
      g_print ("set desktop visible\n");
      nautilus_desktop_window_ensure ();
    }
  else
    {
      desktop_window = nautilus_desktop_window_get ();
      if (desktop_window != NULL)
        {
          gtk_widget_destroy (desktop_window);
        }
    }
}

static void
update_desktop_from_gsettings (NautilusDesktopApplication *self)
{
  GdkDisplay *display;
  gboolean visible;

#ifdef GDK_WINDOWING_X11
  display = gdk_display_get_default ();
  visible = g_settings_get_boolean (gnome_background_preferences,
                                    NAUTILUS_PREFERENCES_SHOW_DESKTOP);
  if (!GDK_IS_X11_DISPLAY (display)) {
    if (visible)
      g_warning ("Desktop icons only supported on X11. Desktop not created");

    return;
    }

  nautilus_application_set_desktop_visible (self, visible);

  return;
#endif

  g_warning ("Desktop icons only supported on X11. Desktop not created");
}

static void
init_desktop (NautilusDesktopApplication *self)
{
  g_signal_connect_swapped (gnome_background_preferences, "changed::" NAUTILUS_PREFERENCES_SHOW_DESKTOP,
                            G_CALLBACK (update_desktop_from_gsettings),
                            self);
  update_desktop_from_gsettings (self);
}

static void
nautilus_desktop_application_activate (GApplication *app)
{
}

static void
nautilus_desktop_application_startup (GApplication *app)
{
  NautilusDesktopApplication *self = NAUTILUS_DESKTOP_APPLICATION (app);

  g_print ("startup desktop\n");

  nautilus_application_startup_common (NAUTILUS_APPLICATION (app));
  self->freedesktop_cancellable = g_cancellable_new ();
  nautilus_freedesktop_file_manager1_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        "org.freedesktop.FileManager1",
                                                        "/org/freedesktop/FileManager1",
                                                        self->freedesktop_cancellable,
                                                        on_freedesktop_bus_proxy_created,
                                                        self);

  init_desktop (self);
}

static void
nautilus_desktop_application_dispose (GObject *object)
{
  NautilusDesktopApplication *self = NAUTILUS_DESKTOP_APPLICATION (object);

  g_clear_object (&self->freedesktop_cancellable);
}

static void
nautilus_desktop_application_class_init (NautilusDesktopApplicationClass *klass)
{
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  NautilusApplicationClass *parent_class = NAUTILUS_APPLICATION_CLASS (klass);

  g_print ("desktop application class init\n");

  parent_class->open_location_full = open_location_full;

  application_class->startup = nautilus_desktop_application_startup;
  application_class->activate = nautilus_desktop_application_activate;

  gobject_class->dispose = nautilus_desktop_application_dispose;
}

static void
nautilus_desktop_application_init (NautilusDesktopApplication *self)
{
  g_autoptr (GFile) desktop_location;

  desktop_location = g_file_new_for_uri (EEL_DESKTOP_URI);
  desktop_directory = g_object_new (NAUTILUS_TYPE_DESKTOP_DIRECTORY, "location", desktop_location, NULL);
  nautilus_directory_add_to_cache (NAUTILUS_DIRECTORY (desktop_directory));
  g_object_unref (desktop_location);
  g_print ("desktop application init\n");
}

NautilusDesktopApplication *
nautilus_desktop_application_new (void)
{
  g_print ("desktop application new\n");
  return g_object_new (NAUTILUS_TYPE_DESKTOP_APPLICATION,
                       "application-id", "org.gnome.NautilusDesktop",
                        NULL);
}

