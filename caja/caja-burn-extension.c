/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Copyright (C) 2003 Novell, Inc.
 * Copyright (C) 2003-2004 Red Hat, Inc.
 * Copyright (C) 2005 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2008-2009 Philippe Rouquier <bonfire-app@wanadoo.fr> (modified to work with rejilla)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <libcaja-extension/caja-menu-provider.h>
#include <libcaja-extension/caja-location-widget-provider.h>

#include "rejilla-media.h"
#include "rejilla-medium-monitor.h"
#include "rejilla-drive.h"
#include "rejilla-medium.h"

#include "rejilla-burn-lib.h"
#include "rejilla-track.h"
#include "rejilla-track-data-cfg.h"
#include "rejilla-track-image-cfg.h"
#include "rejilla-track-disc.h"
#include "rejilla-session.h"
#include "rejilla-session-cfg.h"

#include "rejilla-tool-dialog.h"
#include "rejilla-blank-dialog.h"
#include "rejilla-sum-dialog.h"

#include "rejilla-burn-options.h"
#include "rejilla-burn-dialog.h"

#include "caja-burn-bar.h"

#include "rejilla-project-name.h"

#include "rejilla-misc.h"
#include "rejilla-drive-settings.h"

#include "rejilla-media-private.h"
#include "burn-debug.h"

#define BURN_URI "burn:///"

#define CAJA_TYPE_DISC_BURN  (caja_disc_burn_get_type ())
#define CAJA_DISC_BURN(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), CAJA_TYPE_DISC_BURN, CajaDiscBurn))
#define CAJA_IS_DISC_BURN(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), CAJA_TYPE_DISC_BURN))

typedef struct _CajaDiscBurnPrivate CajaDiscBurnPrivate;

typedef struct
{
        GObject              parent_slot;
        CajaDiscBurnPrivate *priv;
} CajaDiscBurn;

typedef struct
{
        GObjectClass parent_slot;
} CajaDiscBurnClass;

#define CAJA_DISC_BURN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CAJA_TYPE_DISC_BURN, CajaDiscBurnPrivate))

struct _CajaDiscBurnPrivate
{
        GFileMonitor *burn_monitor;
        guint         empty : 1;

        guint         start_monitor_id;
        guint         empty_update_id;

        GSList       *widget_list;
};

static GType caja_disc_burn_get_type      (void);
static void  caja_disc_burn_register_type (GTypeModule *module);

static GObjectClass *parent_class;

//#define DEBUG_PRINT(format_MACRO,...)           g_print (format_MACRO, ##__VA_ARGS__);
#define DEBUG_PRINT(format_MACRO,...)             

#define REJILLA_SCHEMA				"org.mate.rejilla"
#define REJILLA_PROPS_CAJA_EXT_DEBUG	"caja-extension-debug"

/* do not call rejilla_*_start() at caja startup, they are very expensive;
 * lazily initialize those instead */
static void
ensure_initialized ()
{
	static gboolean initialized = FALSE;

	if (!initialized) {
		GSettings *settings;

		settings = g_settings_new (REJILLA_SCHEMA);
		if (g_settings_get_boolean (settings, REJILLA_PROPS_CAJA_EXT_DEBUG)) {
			rejilla_media_library_set_debug (TRUE);
			rejilla_burn_library_set_debug (TRUE);
		}
		g_object_unref (settings);

		rejilla_media_library_start ();
		rejilla_burn_library_start (NULL, NULL);

		DEBUG_PRINT ("Librejilla-media started\n");
		initialized = TRUE;
	}
}

static void
launch_rejilla_on_window_session (RejillaSessionCfg	*session,
                                  const gchar		*dialog_title,
				  GtkWidget		*options,
				  GtkWindow		*window)
{
	gboolean		 success;
	GtkResponseType		 result;
	const gchar		*icon_name;
	GtkWidget		*dialog;
	RejillaDriveSettings	*settings;

	/* Set saved temporary directory for the session.
	 * NOTE: RejillaBurnSession can cope with NULL path */
	settings = rejilla_drive_settings_new ();
	rejilla_drive_settings_set_session (settings, REJILLA_BURN_SESSION (session));

	/* Get the icon for the window */
	if (window)
		icon_name = gtk_window_get_icon_name (window);
	else
		icon_name = "rejilla";

	/* run option dialog */
	dialog = rejilla_burn_options_new (session);
	gtk_window_set_icon_name (GTK_WINDOW (dialog), icon_name);

	if (dialog_title)
		gtk_window_set_title (GTK_WINDOW (dialog), dialog_title);

	if (options)
		rejilla_burn_options_add_options (REJILLA_BURN_OPTIONS (dialog), options);

	gtk_widget_show (GTK_WIDGET (dialog));
	result = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);

	if (result != GTK_RESPONSE_OK
	&&  result != GTK_RESPONSE_ACCEPT) {
		g_object_unref (settings);
		return;
	}

	/* now run burn dialog */
	dialog = rejilla_burn_dialog_new ();

	gtk_window_set_icon_name (GTK_WINDOW (dialog), icon_name);

	if (dialog_title)
		gtk_window_set_title (GTK_WINDOW (dialog), dialog_title);

	rejilla_session_cfg_disable (session);

	gtk_widget_show (dialog);
	gtk_window_present (GTK_WINDOW (dialog));

	if (result == GTK_RESPONSE_OK)
		success = rejilla_burn_dialog_run (REJILLA_BURN_DIALOG (dialog),
		                                   REJILLA_BURN_SESSION (session));
	else
		success = rejilla_burn_dialog_run_multi (REJILLA_BURN_DIALOG (dialog),
		                                         REJILLA_BURN_SESSION (session));

	gtk_widget_destroy (dialog);

	g_object_unref (settings);
}

static gboolean
caja_disc_burn_is_empty (GtkWindow *toplevel)
{
	GFileEnumerator *enumerator;
	GFileInfo *info = NULL;
	GError *error = NULL;
	GFile *file;

	file = g_file_new_for_uri (BURN_URI);
	enumerator = g_file_enumerate_children (file,
						G_FILE_ATTRIBUTE_STANDARD_NAME,
						G_FILE_QUERY_INFO_NONE,
						NULL,
						&error);
	if (!enumerator) {
		gchar *string;

		DEBUG_PRINT ("Could not open burn uri %s: %s\n",
                             uri,
                             error->message);

		if (!toplevel) {
			g_error_free (error);
			return TRUE;
		}

		string = g_strdup_printf ("%s.", _("An internal error occurred"));
		rejilla_utils_message_dialog (GTK_WIDGET (toplevel),
					      string,
					      error ? error->message:NULL,
					      GTK_MESSAGE_ERROR);
		g_free (string);

		g_object_unref (file);
		g_error_free (error);
		return TRUE;
	}

	info = g_file_enumerator_next_file (enumerator, NULL, NULL);
	g_object_unref (enumerator);
	g_object_unref (file);

	if (!info) {
		if (!toplevel)
			return TRUE;

		rejilla_utils_message_dialog (GTK_WIDGET (toplevel),
					      _("Please add files."),
					      _("There are no files to write to disc"),
					      GTK_MESSAGE_ERROR);
		return TRUE;
	}

	g_object_unref (info);
	return FALSE;
}

static void
rejilla_session_name_changed (RejillaProjectName *project_name,
                              RejillaBurnSession *session)
{
	const gchar *label;

	label = gtk_entry_get_text (GTK_ENTRY (project_name));
	rejilla_burn_session_set_label (session, label);
}

static void
write_activate (GtkWindow *toplevel)
{
	RejillaTrackDataCfg	*track;
	RejillaSessionCfg	*session;
	GtkWidget 		*name_options;
	GtkWidget		*options;
	gchar			*string;

	if (caja_disc_burn_is_empty (toplevel))
		return;

	ensure_initialized ();

	track = rejilla_track_data_cfg_new ();
	rejilla_track_data_cfg_add (track, BURN_URI, NULL);

	session = rejilla_session_cfg_new ();
	rejilla_burn_session_add_track (REJILLA_BURN_SESSION (session),
					REJILLA_TRACK (track),
					NULL);
	g_object_unref (track);

	/* add name widget here to set the label of the volume */
	name_options = rejilla_project_name_new (NULL);
	rejilla_project_name_set_session (REJILLA_PROJECT_NAME (name_options),
					  REJILLA_BURN_SESSION (session));
	g_signal_connect (name_options,
	                  "name-changed",
	                  G_CALLBACK (rejilla_session_name_changed),
	                  session);

	string = g_strdup_printf ("<b>%s</b>", _("Disc name"));
	options = rejilla_utils_pack_properties (string,
						 name_options,
						 NULL);
	g_free (string);
	gtk_widget_show_all (options);

	/* NOTE: set the disc we're handling */
	launch_rejilla_on_window_session (session,
	                                  _("CD/DVD Creator"),
	                                  options,
	                                  toplevel);

	/* cleanup */
	g_object_unref (session);
}

static void
write_activate_cb (CajaMenuItem *item,
                   gpointer          user_data)
{
	write_activate (GTK_WINDOW (user_data));
}

static void
launch_rejilla_on_window_track (RejillaTrack	*track,
                                const gchar	*dialog_title,
				GtkWidget	*options,
				GtkWindow	*window)
{
	RejillaSessionCfg *session;

	/* create a session and add track */
	session = rejilla_session_cfg_new ();
	rejilla_burn_session_add_track (REJILLA_BURN_SESSION (session),
					REJILLA_TRACK (track),
					NULL);

	launch_rejilla_on_window_session (session,
	                                  dialog_title,
	                                  options,
	                                  window);
	g_object_unref (session);
}

static void
write_iso_activate_cb (CajaMenuItem *item,
                       gpointer          user_data)
{
	RejillaTrackImageCfg	*track;
        CajaFileInfo	*file_info;
        char			*uri;

	ensure_initialized();

        file_info = g_object_get_data (G_OBJECT (item), "file_info");
        uri = caja_file_info_get_uri (file_info);

	track = rejilla_track_image_cfg_new ();
	rejilla_track_image_cfg_set_source (track, uri);

	launch_rejilla_on_window_track (REJILLA_TRACK (track),
	                                _("Write to Disc"),
	                                NULL,
	                                GTK_WINDOW (user_data));
	g_object_unref (track);
}

static void
copy_disc_activate_cb (CajaMenuItem *item,
                       gpointer          user_data)
{
        char            	*device_path;
	RejillaMediumMonitor	*monitor;
	RejillaTrackDisc	*track;
	RejillaDrive		*drive;

	ensure_initialized();

        device_path = g_object_get_data (G_OBJECT (item), "drive_device_path");
	monitor = rejilla_medium_monitor_get_default ();
	drive = rejilla_medium_monitor_get_drive (monitor, device_path);
	g_object_unref (monitor);

	track = rejilla_track_disc_new ();
	rejilla_track_disc_set_drive (track, drive);
	g_object_unref (drive);

	launch_rejilla_on_window_track (REJILLA_TRACK (track),
	                                _("Copy Disc"),
	                                NULL,
	                                GTK_WINDOW (user_data));
	g_object_unref (track);
}

static void
tool_dialog_run (RejillaToolDialog	*dialog,
		 GtkWindow		*toplevel,
		 CajaMenuItem	*item)
{
	char			*device_path;
	RejillaDrive		*drive;
	RejillaMediumMonitor	*monitor;

	device_path = g_object_get_data (G_OBJECT (item), "drive_device_path");
	if (!device_path) {
		g_warning ("Drive device path not specified");
		return;
	}

	monitor = rejilla_medium_monitor_get_default ();
	drive = rejilla_medium_monitor_get_drive (monitor, device_path);
	g_object_unref (monitor);

	if (drive) {
		rejilla_tool_dialog_set_medium (REJILLA_TOOL_DIALOG (dialog),
						rejilla_drive_get_medium (drive));
		g_object_unref (drive);
	}

	/* Get the icon for the window */
	if (toplevel)
		gtk_window_set_icon_name (GTK_WINDOW (dialog), gtk_window_get_icon_name (toplevel));
	else
		gtk_window_set_icon_name (GTK_WINDOW (dialog), "rejilla");

	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
blank_disc_activate_cb (CajaMenuItem *item,
                        gpointer          user_data)
{
	RejillaBlankDialog *dialog;

	ensure_initialized();

	dialog = rejilla_blank_dialog_new ();
	tool_dialog_run (REJILLA_TOOL_DIALOG (dialog),
			 GTK_WINDOW (user_data),
			 item);
}

static void
check_disc_activate_cb (CajaMenuItem *item,
                        gpointer          user_data)
{
	RejillaSumDialog *dialog;

	ensure_initialized ();

	dialog = rejilla_sum_dialog_new ();
	tool_dialog_run (REJILLA_TOOL_DIALOG (dialog),
			 GTK_WINDOW (user_data),
			 item);
}

static gboolean
volume_is_blank (GVolume *volume)
{
        RejillaMediumMonitor *monitor;
        RejillaMedium        *medium;
        RejillaDrive         *drive;
        gchar                *device;
        gboolean              is_blank;

        is_blank = FALSE;

        device = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        if (!device)
                return FALSE;

        DEBUG_PRINT ("Got device: %s\n", device);

        monitor = rejilla_medium_monitor_get_default ();
        drive = rejilla_medium_monitor_get_drive (monitor, device);
        g_object_unref (monitor);
        g_free (device);

        if (drive == NULL)
                return FALSE;

        medium = rejilla_drive_get_medium (drive);
        is_blank = (rejilla_medium_get_status (medium) & REJILLA_MEDIUM_BLANK);
        g_object_unref (drive);

        return is_blank;
}

static GVolume *
drive_get_first_volume (GDrive *drive)
{
        GVolume *volume;
        GList   *volumes;

        volumes = g_drive_get_volumes (drive);

        volume = g_list_nth_data (volumes, 0);

        if (volume != NULL) {
                g_object_ref (volume);
        }

        g_list_foreach (volumes, (GFunc) g_object_unref, NULL);
        g_list_free (volumes);

        return volume;
}

static gboolean
drive_is_cd_device (GDrive *gdrive)
{
        RejillaMediumMonitor *monitor;
        RejillaDrive         *drive;
        gchar                *device;

        device = g_drive_get_identifier (gdrive, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        if (!device)
                return FALSE;

        DEBUG_PRINT ("Got device: %s\n", device);

	/* FIXME: since we call the monitor, the library should be initialized.
	 * To avoid all the initializing we'll be able to use the new GIO API
	 * (#594649 -  Have a way to detect optical drives) */
	ensure_initialized();

	monitor = rejilla_medium_monitor_get_default ();
        drive = rejilla_medium_monitor_get_drive (monitor, device);
        g_object_unref (monitor);
        g_free (device);

        if (drive == NULL)
                return FALSE;
        
        g_object_unref (drive);
        return TRUE;
}

static GList *
caja_disc_burn_get_file_items (CajaMenuProvider *provider,
                                   GtkWidget            *window,
                                   GList                *selection)
{
        GList            *items = NULL;
        CajaMenuItem *item;
        CajaFileInfo *file_info;
        GFile            *file;
        GMount           *mount;
        GVolume          *volume;
        GDrive           *drive;
        char             *mime_type;
        gboolean          is_iso;

        DEBUG_PRINT ("Getting file items\n");

        if (!selection || selection->next != NULL) {
                return NULL;
        }

        file_info = CAJA_FILE_INFO (selection->data);

        if (caja_file_info_is_gone (file_info)) {
                return NULL;
        }

        file = caja_file_info_get_location (file_info);

        if (file == NULL) {
                DEBUG_PRINT ("No file found\n");
                return NULL;
        }

        mime_type = caja_file_info_get_mime_type (file_info);
        DEBUG_PRINT ("Mime type: %s\n", mime_type);
        if (! mime_type) {
                g_object_unref (file);
                return NULL;
        }

        is_iso = (strcmp (mime_type, "application/x-iso-image") == 0)
                || (strcmp (mime_type, "application/x-cd-image") == 0)
                || (strcmp (mime_type, "application/x-cue") == 0)
                || (strcmp (mime_type, "application/x-toc") == 0)
                || (strcmp (mime_type, "application/x-cdrdao-toc") == 0);

        if (is_iso) {
                /* Whether or not this file is local is not a problem */
                item = caja_menu_item_new ("CajaDiscBurn::write_iso",
                                               _("_Write to Disc…"),
                                               _("Write disc image to a CD or DVD"),
                                               "media-optical-data-new");
                g_object_set_data (G_OBJECT (item), "file_info", file_info);
                g_object_set_data (G_OBJECT (item), "window", window);
                g_signal_connect (item, "activate",
                                  G_CALLBACK (write_iso_activate_cb), window);
                items = g_list_append (items, item);
        }

        /*
         * We handle two cases here.  The selection is:
         *  A) a volume
         *  B) a drive
         *
         * This is because there is very little distinction between
         * the two for CD/DVD media
         */

        drive = NULL;
        volume = NULL;

        mount = caja_file_info_get_mount (file_info);
        if (mount != NULL) {
                drive = g_mount_get_drive (mount);
                volume = g_mount_get_volume (mount);
        } else {
                char *uri = g_file_get_uri (file);
                DEBUG_PRINT ("Mount not found: %s\n", uri);
                g_free (uri);
        }

        if (drive == NULL && volume != NULL) {
                /* case A */
                drive = g_volume_get_drive (volume);
        } else if (volume == NULL && drive != NULL) {
                /* case B */
                volume = drive_get_first_volume (drive);
                if (volume == NULL) {
                        DEBUG_PRINT ("Volume not found\n");
                }
        }

        if (drive != NULL
            && volume != NULL
            && drive_is_cd_device (drive)
            && ! volume_is_blank (volume)) {
                char			*device_path;
		RejillaMediumMonitor	*monitor;
		RejillaDrive		*bdrive;
		RejillaMedium		*medium;
		RejillaMedia		 media;
		RejillaTrackType	*type;

		/* Reminder: the following is not needed since it is already 
		 * called in drive_is_cd_device ().
		 * ensure_initialized();
		 */

                device_path = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
		monitor = rejilla_medium_monitor_get_default ();
		bdrive = rejilla_medium_monitor_get_drive (monitor, device_path);
		g_object_unref (monitor);

		medium = rejilla_drive_get_medium (bdrive);
		media = rejilla_medium_get_status (medium);
		g_object_unref (bdrive);

		type = rejilla_track_type_new ();
		rejilla_track_type_set_has_medium (type);
		rejilla_track_type_set_medium_type (type, media);
		if (rejilla_burn_library_input_supported (type) == REJILLA_BURN_OK) {
			/* user may want to copy it ... */
			item = caja_menu_item_new ("CajaDiscBurn::copy_disc",
						       _("_Copy Disc…"),
						       _("Create a copy of this CD or DVD"),
						       "media-optical-copy");
			g_object_set_data (G_OBJECT (item), "file_info", file_info);
			g_object_set_data (G_OBJECT (item), "window", window);
			g_object_set_data_full (G_OBJECT (item), "drive_device_path", g_strdup (device_path), g_free);
			g_signal_connect (item, "activate", G_CALLBACK (copy_disc_activate_cb), window);
			items = g_list_append (items, item);
		}
		rejilla_track_type_free (type);

		if (rejilla_burn_library_get_media_capabilities (media) & REJILLA_MEDIUM_REWRITABLE) {
			/* ... or if it's a rewritable medium to blank it ... */
			item = caja_menu_item_new ("CajaDiscBurn::blank_disc",
						       _("_Blank Disc…"),
						       _("Blank this CD or DVD"),
						       "media-optical-blank");
			g_object_set_data (G_OBJECT (item), "file_info", file_info);
			g_object_set_data (G_OBJECT (item), "window", window);
			g_object_set_data_full (G_OBJECT (item), "drive_device_path", g_strdup (device_path), g_free);
			g_signal_connect (item, "activate",
					  G_CALLBACK (blank_disc_activate_cb), window);
			items = g_list_append (items, item);
		}

		/* - library should be able to checksum
		 * - disc must have a data session */
		if (rejilla_burn_library_can_checksum ()
		&& (media & REJILLA_MEDIUM_HAS_DATA)) {
			/* ... or verify medium. */
			item = caja_menu_item_new ("CajaDiscBurn::check_disc",
						       _("_Check Disc…"),
						       _("Check the data integrity on this CD or DVD"),
						       NULL);
			g_object_set_data (G_OBJECT (item), "file_info", file_info);
			g_object_set_data (G_OBJECT (item), "window", window);
			g_object_set_data_full (G_OBJECT (item), "drive_device_path", g_strdup (device_path), g_free);
			g_signal_connect (item, "activate",
					  G_CALLBACK (check_disc_activate_cb),
			                  window);
			items = g_list_append (items, item);
		}

                g_free (device_path);
        }

        g_object_unref (file);

        if (drive != NULL) {
                g_object_unref (drive);
        }
        if (volume != NULL) {
                g_object_unref (volume);
        }

        g_free (mime_type);

        DEBUG_PRINT ("Items returned\n");
        return items;
}

static GList *
caja_disc_burn_get_background_items (CajaMenuProvider *provider,
                                         GtkWidget            *window,
                                         CajaFileInfo     *current_folder)
{
        GList *items;
        char  *scheme;

        items = NULL;

        scheme = caja_file_info_get_uri_scheme (current_folder);

        if (strcmp (scheme, "burn") == 0) {
                CajaMenuItem *item;

                item = caja_menu_item_new ("CajaDiscBurn::write_menu",
                                               _("_Write to Disc…"),
                                               _("Write contents to a CD or DVD"),
                                               "rejilla");
                g_signal_connect (item, "activate",
                                  G_CALLBACK (write_activate_cb),
                                  window);
                items = g_list_append (items, item);

                g_object_set (item, "sensitive", ! CAJA_DISC_BURN (provider)->priv->empty, NULL);
        }

        g_free (scheme);

        return items;
}

static GList *
caja_disc_burn_get_toolbar_items (CajaMenuProvider *provider,
                                      GtkWidget            *window,
                                      CajaFileInfo     *current_folder)
{
        GList *items;

        items = NULL;

        return items;
}

static void
caja_disc_burn_menu_provider_iface_init (CajaMenuProviderIface *iface)
{
        iface->get_file_items = caja_disc_burn_get_file_items;
        iface->get_background_items = caja_disc_burn_get_background_items;
        iface->get_toolbar_items = caja_disc_burn_get_toolbar_items;
}

static void
bar_activated_cb (CajaDiscBurnBar	*bar,
                  gpointer		 user_data)
{
	write_activate (GTK_WINDOW (user_data));
}

static void
destroyed_callback (GtkWidget    *widget,
                    CajaDiscBurn *burn)
{
        burn->priv->widget_list = g_slist_remove (burn->priv->widget_list, widget);
}

static void
sense_widget (CajaDiscBurn *burn,
              GtkWidget    *widget)
{
        gtk_widget_set_sensitive (widget, !burn->priv->empty);

        burn->priv->widget_list = g_slist_prepend (burn->priv->widget_list, widget);

        g_signal_connect (widget, "destroy",
                          G_CALLBACK (destroyed_callback),
                          burn);
}

static GtkWidget *
caja_disc_burn_get_location_widget (CajaLocationWidgetProvider *iface,
                                        const char                     *uri,
                                        GtkWidget                      *window)
{
        if (g_str_has_prefix (uri, "burn:")) {
                GtkWidget    *bar;
                CajaDiscBurn *burn;

                DEBUG_PRINT ("Get location widget for burn\n");

                burn = CAJA_DISC_BURN (iface);

                bar = caja_disc_burn_bar_new ();

                sense_widget (burn, caja_disc_burn_bar_get_button (CAJA_DISC_BURN_BAR (bar)));

                g_signal_connect (bar, "activate",
                                  G_CALLBACK (bar_activated_cb),
                                  window);

                gtk_widget_show (bar);

                return bar;
        }

        return NULL;
}

static void
caja_disc_burn_location_widget_provider_iface_init (CajaLocationWidgetProviderIface *iface)
{
        iface->get_widget = caja_disc_burn_get_location_widget;
}

static void
update_widget_sensitivity (GtkWidget    *widget,
                           CajaDiscBurn *burn)
{
        gtk_widget_set_sensitive (widget, !burn->priv->empty);
}

static gboolean
update_empty_idle (CajaDiscBurn *burn)
{
        gboolean is_empty;

        burn->priv->empty_update_id = 0;

        is_empty = caja_disc_burn_is_empty (NULL);

        DEBUG_PRINT ("Dir is %s\n", is_empty ? "empty" : "not empty");

        if (burn->priv->empty != is_empty) {
                burn->priv->empty = is_empty;
                /* update bar */
                g_slist_foreach (burn->priv->widget_list, (GFunc)update_widget_sensitivity, burn);

                /* Trigger update for menu items */
                caja_menu_provider_emit_items_updated_signal (CAJA_MENU_PROVIDER (burn));
        }

        return FALSE;
}

static void
queue_update_empty (CajaDiscBurn *burn)
{
        if (burn->priv->empty_update_id != 0) {
                g_source_remove (burn->priv->empty_update_id);
        }

        burn->priv->empty_update_id = g_idle_add ((GSourceFunc)update_empty_idle, burn);
}

static void
burn_monitor_cb (GFileMonitor     *monitor,
                 GFile            *file,
                 GFile            *other_file,
                 GFileMonitorEvent event_type,
                 CajaDiscBurn     *burn)
{
        DEBUG_PRINT ("Monitor callback type %d\n", event_type);

        /* only queue the action if it has a chance of changing the state */
        if (event_type == G_FILE_MONITOR_EVENT_CREATED) {
                if (burn->priv->empty) {
                        queue_update_empty (burn);
                }
        } else if (event_type == G_FILE_MONITOR_EVENT_DELETED) {
                if (! burn->priv->empty) {
                        queue_update_empty (burn);
                }
        }
}

static gboolean
start_monitor (CajaDiscBurn *burn)
{
        GFile  *file;
        GError *error;

        file = g_file_new_for_uri (BURN_URI);

        error = NULL;
        burn->priv->burn_monitor = g_file_monitor_directory (file,
                                                             G_FILE_MONITOR_NONE,
                                                             NULL,
                                                             &error);
        if (burn->priv->burn_monitor == NULL) {
                DEBUG_PRINT ("Unable to add monitor: %s", error->message);
                g_error_free (error);
                goto out;
        }

        DEBUG_PRINT ("Starting monitor for %s\n", BURN_URI);
        g_signal_connect (burn->priv->burn_monitor,
                          "changed",
                          G_CALLBACK (burn_monitor_cb),
                          burn);

        burn->priv->empty = caja_disc_burn_is_empty (NULL);

        DEBUG_PRINT ("Init burn extension, empty: %d\n", burn->priv->empty);

 out:
        g_object_unref (file);

        burn->priv->start_monitor_id = 0;

        return FALSE;
}

static void
caja_disc_burn_instance_init (CajaDiscBurn *burn)
{
        burn->priv = CAJA_DISC_BURN_GET_PRIVATE (burn);
        burn->priv->start_monitor_id = g_timeout_add_seconds (1,
                                                              (GSourceFunc)start_monitor,
                                                              burn);
}

static void
caja_disc_burn_finalize (GObject *object)
{
        CajaDiscBurn *burn;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CAJA_IS_DISC_BURN (object));

        DEBUG_PRINT ("Finalizing burn extension\n");

        burn = CAJA_DISC_BURN (object);

        g_return_if_fail (burn->priv != NULL);

        if (burn->priv->empty_update_id > 0) {
                g_source_remove (burn->priv->empty_update_id);
        }

        if (burn->priv->start_monitor_id > 0) {
                g_source_remove (burn->priv->start_monitor_id);
        }

        if (burn->priv->burn_monitor != NULL) {
                g_file_monitor_cancel (burn->priv->burn_monitor);
        }

        if (burn->priv->widget_list != NULL) {
                g_slist_free (burn->priv->widget_list);
        }

        G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
caja_disc_burn_class_init (CajaDiscBurnClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        parent_class = g_type_class_peek_parent (klass);

        object_class->finalize = caja_disc_burn_finalize;

        g_type_class_add_private (klass, sizeof (CajaDiscBurnPrivate));
}

static GType burn_type = 0;

static GType
caja_disc_burn_get_type (void)
{
        return burn_type;
}

static void
caja_disc_burn_register_type (GTypeModule *module)
{
        static const GTypeInfo info = {
                sizeof (CajaDiscBurnClass),
                (GBaseInitFunc) NULL,
                (GBaseFinalizeFunc) NULL,
                (GClassInitFunc) caja_disc_burn_class_init,
                NULL,
                NULL,
                sizeof (CajaDiscBurn),
                0,
                (GInstanceInitFunc) caja_disc_burn_instance_init,
        };

        static const GInterfaceInfo menu_provider_iface_info = {
                (GInterfaceInitFunc) caja_disc_burn_menu_provider_iface_init,
                NULL,
                NULL
        };
        static const GInterfaceInfo location_widget_provider_iface_info = {
                (GInterfaceInitFunc) caja_disc_burn_location_widget_provider_iface_init,
                NULL,
                NULL
        };

        burn_type = g_type_module_register_type (module,
                                                 G_TYPE_OBJECT,
                                                 "CajaDiscBurn",
                                                 &info, 0);

        g_type_module_add_interface (module,
                                     burn_type,
                                     CAJA_TYPE_MENU_PROVIDER,
                                     &menu_provider_iface_info);
        g_type_module_add_interface (module,
                                     burn_type,
                                     CAJA_TYPE_LOCATION_WIDGET_PROVIDER,
                                     &location_widget_provider_iface_info);
}

void
caja_module_initialize (GTypeModule *module)
{
        DEBUG_PRINT ("Initializing caja-disc-recorder\n");
        caja_disc_burn_register_type (module);
}

void
caja_module_shutdown (void)
{
        DEBUG_PRINT ("Shutting down caja disc recorder\n");

        /* Don't do that in case another module would need the library */
        //rejilla_media_library_stop ();
        //rejilla_burn_library_stop ();
}

void
caja_module_list_types (const GType **types,
                            int          *num_types)
{
        static GType type_list [1];

        type_list [0] = CAJA_TYPE_DISC_BURN;

        *types = type_list;
        *num_types = 1;
}
