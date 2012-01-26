/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/***************************************************************************
*            player.c
*
*  lun mai 30 08:15:01 2005
*  Copyright  2005  Philippe Rouquier
*  rejilla-app@wanadoo.fr
****************************************************************************/

/*
 *  Rejilla is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Rejilla is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include <gtk/gtk.h>

#include "rejilla-misc.h"
#include "rejilla-metadata.h"
#include "rejilla-io.h"

#include "rejilla-units.h"

#include "rejilla-setting.h"

#include "rejilla-player.h"
#include "rejilla-player-bacon.h"
#include "rejilla-utils.h"


G_DEFINE_TYPE (RejillaPlayer, rejilla_player, GTK_TYPE_ALIGNMENT);

struct RejillaPlayerPrivate {
	GtkWidget *notebook;

	GtkWidget *bacon;

	GtkWidget *spinner;

	GtkWidget *play_image;

	gint image_width;
	gint image_height;
	GdkPixbuf *pixbuf;

	GtkWidget *image_display;

	GtkWidget *zoom_in;
	GtkWidget *zoom_out;

	gint video_width;
	gint video_height;

	RejillaPlayerBaconState state;

	RejillaIOJobBase *meta_task;

	gchar *uri;
	gint64 start;
	gint64 end;
	gint64 length;

	int is_video:1;
};

typedef enum {
	READY_SIGNAL,
	ERROR_SIGNAL,
	EOF_SIGNAL,
	LAST_SIGNAL
} RejillaPlayerSignalType;
static guint rejilla_player_signals [LAST_SIGNAL] = { 0 };

static GObjectClass *parent_class = NULL;

static void
rejilla_player_no_multimedia_stream (RejillaPlayer *player)
{
	gtk_spinner_stop (GTK_SPINNER (player->priv->spinner));
	gtk_widget_set_has_tooltip (GTK_WIDGET (player), FALSE);
	gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 2);

	player->priv->length = 0;
	player->priv->start = 0;
	player->priv->end = 0;
}

static void
rejilla_player_video_zoom_out (RejillaPlayer *player)
{
	player->priv->video_width -= PLAYER_BACON_WIDTH / 3;
	player->priv->video_height -= PLAYER_BACON_HEIGHT / 3;

	if (player->priv->video_width <= PLAYER_BACON_WIDTH ||
	    player->priv->video_height <= PLAYER_BACON_HEIGHT) {
		player->priv->video_width = PLAYER_BACON_WIDTH;
		player->priv->video_height = PLAYER_BACON_HEIGHT;
	}

	gtk_widget_set_size_request (GTK_WIDGET (player->priv->bacon),
				     player->priv->video_width,
				     player->priv->video_height);
}

static void
rejilla_player_video_zoom_in (RejillaPlayer *player)
{
	player->priv->video_width += PLAYER_BACON_WIDTH / 3;
	player->priv->video_height += PLAYER_BACON_HEIGHT / 3;

	if (player->priv->video_width >= PLAYER_BACON_WIDTH * 3 ||
	    player->priv->video_height >= PLAYER_BACON_HEIGHT * 3) {
		player->priv->video_width = PLAYER_BACON_WIDTH * 3;
		player->priv->video_height = PLAYER_BACON_HEIGHT * 3;
	}

	gtk_widget_set_size_request (GTK_WIDGET (player->priv->bacon),
				     player->priv->video_width,
				     player->priv->video_height);
}

static gboolean
rejilla_player_scale_image (RejillaPlayer *player)
{
	gint height, width;
	GdkPixbuf *scaled;
	gdouble ratio;

	height = gdk_pixbuf_get_height (player->priv->pixbuf);
	width = gdk_pixbuf_get_width (player->priv->pixbuf);

	if (player->priv->image_height == height
	&&  player->priv->image_width == width) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (player->priv->image_display),
					   player->priv->pixbuf);
		return TRUE;
	}

	if (width / (gdouble) player->priv->image_width > height / (gdouble) player->priv->image_height)
		ratio = (gdouble) width / (gdouble) player->priv->image_width;
	else
		ratio = (gdouble) height / (gdouble) player->priv->image_height;

	scaled = gdk_pixbuf_scale_simple (player->priv->pixbuf,
					  width / ratio,
					  height / ratio,
					  GDK_INTERP_BILINEAR);

	if (!scaled)
		return FALSE;

	gtk_image_set_from_pixbuf (GTK_IMAGE (player->priv->image_display), scaled);
	g_object_unref (scaled);

	return TRUE;
}

static void
rejilla_player_image_zoom_in (RejillaPlayer *player)
{
	player->priv->image_width += PLAYER_BACON_WIDTH / 3;
	player->priv->image_height += PLAYER_BACON_HEIGHT / 3;

	if (player->priv->image_width >= PLAYER_BACON_WIDTH * 3 ||
	    player->priv->image_height >= PLAYER_BACON_HEIGHT * 3) {
		player->priv->image_width = PLAYER_BACON_WIDTH * 3;
		player->priv->image_height = PLAYER_BACON_HEIGHT * 3;
	}

	if (player->priv->pixbuf)
		rejilla_player_scale_image (player);
}

static void
rejilla_player_image_zoom_out (RejillaPlayer *player)
{
	gint min_height, min_width;

	if (player->priv->pixbuf) {
		min_width = MIN (PLAYER_BACON_WIDTH, gdk_pixbuf_get_width (player->priv->pixbuf));
		min_height = MIN (PLAYER_BACON_HEIGHT, gdk_pixbuf_get_height (player->priv->pixbuf));
	}
	else {
		min_width = PLAYER_BACON_WIDTH;
		min_height = PLAYER_BACON_HEIGHT;
	}

	player->priv->image_width -= PLAYER_BACON_WIDTH / 3;
	player->priv->image_height -= PLAYER_BACON_HEIGHT / 3;

	/* the image itself */
	if (player->priv->image_width <= min_width ||
	    player->priv->image_height <= min_height) {
		player->priv->image_width = min_width;
		player->priv->image_height = min_height;
	}

	if (player->priv->pixbuf)
		rejilla_player_scale_image (player);
}

static void
rejilla_player_zoom_in_cb (GtkButton *button,
                           RejillaPlayer *player)
{
	rejilla_player_image_zoom_in (player);
	rejilla_player_video_zoom_in (player);
}

static void
rejilla_player_zoom_out_cb (GtkButton *button,
                            RejillaPlayer *player)
{
	rejilla_player_image_zoom_out (player);
	rejilla_player_video_zoom_out (player);
}

static gboolean
rejilla_bacon_scroll (RejillaPlayerBacon *bacon,
                      GdkEventScroll *event,
                      RejillaPlayer *player)
{
	switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (player->priv->notebook))) {
	case 1:
	case 4:
		if (event->direction == GDK_SCROLL_UP)
			rejilla_player_bacon_forward (bacon, GST_SECOND);
		else
			rejilla_player_bacon_backward (bacon, GST_SECOND);
		break;

	case 0:
	case 2:
	case 3:
	default:
		if (event->direction == GDK_SCROLL_UP) {
			rejilla_player_image_zoom_in (player);
			rejilla_player_video_zoom_in (player);
		}
		else {
			rejilla_player_image_zoom_out (player);
			rejilla_player_video_zoom_out (player);
		}
		break;
	}

	return TRUE;
}

static gboolean
rejilla_bacon_button_release (RejillaPlayerBacon *bacon,
                              GdkEventButton *event,
                              RejillaPlayer *player)
{
	if (event->button != 1)
		return FALSE;

	if (player->priv->state == BACON_STATE_READY) {
		/* This will probably never happen as we display a play button */
		gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
		gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 1);
		rejilla_player_bacon_set_uri (REJILLA_PLAYER_BACON (player->priv->bacon), player->priv->uri);
		rejilla_player_bacon_play (REJILLA_PLAYER_BACON (player->priv->bacon));
	}
	else if (player->priv->state == BACON_STATE_PAUSED) {
		gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
		gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 1);
		rejilla_player_bacon_play (REJILLA_PLAYER_BACON (player->priv->bacon));
	}
	else if (player->priv->state == BACON_STATE_PLAYING) {
		gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PAUSE, GTK_ICON_SIZE_DIALOG);
		gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 4);
		rejilla_player_bacon_stop (REJILLA_PLAYER_BACON (player->priv->bacon));
	}

	return TRUE;
}

static gboolean
rejilla_player_button_release (GtkWidget *widget,
                               GdkEventButton *event,
                               RejillaPlayer *player)
{
	if (event->button != 1)
		return FALSE;

	if (gtk_notebook_get_current_page (GTK_NOTEBOOK (player->priv->notebook)) == 4) {
		if (player->priv->state == BACON_STATE_READY) {
			gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PAUSE, GTK_ICON_SIZE_DIALOG);
			if (player->priv->is_video)
				gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 1);

			rejilla_player_bacon_set_uri (REJILLA_PLAYER_BACON (player->priv->bacon), player->priv->uri);
			rejilla_player_bacon_play (REJILLA_PLAYER_BACON (player->priv->bacon));
		}
		else if (player->priv->state == BACON_STATE_PAUSED) {
			gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PAUSE, GTK_ICON_SIZE_DIALOG);
			if (player->priv->is_video)
				gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 1);

			rejilla_player_bacon_play (REJILLA_PLAYER_BACON (player->priv->bacon));
		}
		else if (player->priv->state == BACON_STATE_PLAYING) {
			gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
			rejilla_player_bacon_stop (REJILLA_PLAYER_BACON (player->priv->bacon));
		}
	}

	return TRUE;
}

static gboolean
rejilla_player_scroll (GtkWidget *widget,
                       GdkEventScroll *event,
                       RejillaPlayer *player)
{
	switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (player->priv->notebook))) {
	case 1:
	case 4:
		if (event->direction == GDK_SCROLL_UP)
			rejilla_player_bacon_forward (REJILLA_PLAYER_BACON (player->priv->bacon), GST_SECOND);
		else
			rejilla_player_bacon_backward (REJILLA_PLAYER_BACON (player->priv->bacon), GST_SECOND);
		break;

	case 0:
	case 2:
	case 3:
	default:
		if (event->direction == GDK_SCROLL_UP) {
			rejilla_player_image_zoom_in (player);
			rejilla_player_video_zoom_in (player);
		}
		else {
			rejilla_player_image_zoom_out (player);
			rejilla_player_video_zoom_out (player);
		}
		break;
	}

	return TRUE;
}

static void
rejilla_player_image (RejillaPlayer *player)
{
	GError *error = NULL;
	gchar *path;

	if (player->priv->pixbuf) {
		g_object_unref (player->priv->pixbuf);
		player->priv->pixbuf = NULL;
	}

	/* image */
	/* FIXME: this does not allow to preview remote files */
	path = g_filename_from_uri (player->priv->uri, NULL, NULL);
	player->priv->pixbuf = gdk_pixbuf_new_from_file (path, &error);

	if (!player->priv->pixbuf) {
		if (error) {
			g_warning ("Couldn't load image %s\n", error->message);
			g_error_free (error);
		}

		rejilla_player_no_multimedia_stream (player);
		g_free (path);
		return;
	}
	
	rejilla_player_scale_image (player);

	gtk_widget_show (player->priv->notebook);
	gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 0);
	g_signal_emit (player,
		       rejilla_player_signals [READY_SIGNAL],
		       0);
}

static void
rejilla_player_update_tooltip (RejillaPlayer *player,
                               GFileInfo *info)
{
	gchar *string;
	gchar *len_string;
	const gchar *title;
	const gchar *artist;

	/* Update the tooltip */
	len_string = rejilla_units_get_time_string (g_file_info_get_attribute_uint64 (info, REJILLA_IO_LEN), TRUE, FALSE);
	title = g_file_info_get_attribute_string (info, REJILLA_IO_TITLE);
	artist = g_file_info_get_attribute_string (info, REJILLA_IO_ARTIST);
	if (artist) {
		gchar *artist_string;

		/* Translators: %s is the name of the artist */
		artist_string = g_strdup_printf (_("by %s"), artist);
		string = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>"
		                                  "\n<i><span size=\"smaller\">%s</span></i>"
		                                  "\n%s",
		                                  title,
		                                  artist_string,
		                                  len_string);
		g_free (artist_string);
	}
	else if (title)
		string = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>"
		                                  "\n%s",
		                                  title,
		                                  len_string);
	else {
		gchar *name;
		gchar *unescaped_uri;

		unescaped_uri = g_uri_unescape_string (player->priv->uri, NULL);
		name = g_path_get_basename (unescaped_uri);
		g_free (unescaped_uri);
		string = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>"
		                                  "\n%s",
		                                  name,
		                                  len_string);
		g_free (name);
	}

	g_free (len_string);

	gtk_widget_set_tooltip_markup (GTK_WIDGET (player), string);
	g_free (string);
}

static void
rejilla_player_metadata_completed (GObject *obj,
				   GError *error,
				   const gchar *uri,
				   GFileInfo *info,
				   gpointer null_data)
{
	RejillaPlayer *player = REJILLA_PLAYER (obj);
	const gchar *mime;

	gtk_spinner_stop (GTK_SPINNER (player->priv->spinner));

	if (player->priv->pixbuf) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (player->priv->image_display), NULL);
		g_object_unref (player->priv->pixbuf);
		player->priv->pixbuf = NULL;
	}

	if (error) {
		rejilla_player_no_multimedia_stream (player);
		g_signal_emit (player,
			       rejilla_player_signals [ERROR_SIGNAL],
			       0);
		return;
	}

	mime = g_file_info_get_content_type (info);

	/* based on the mime type, we try to determine the type of file */
	if (g_file_info_get_attribute_boolean (info, REJILLA_IO_HAS_VIDEO)) {
		if (g_file_info_get_attribute_uint64 (info, REJILLA_IO_LEN) <= 0) {
			rejilla_player_no_multimedia_stream (player);
			g_signal_emit (player,
				       rejilla_player_signals [ERROR_SIGNAL],
				       0);
			return;
		}

		/* video: display play button first */
		player->priv->is_video = TRUE;
		gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
		gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 4);

		rejilla_player_update_tooltip (player, info);
	}
	else if (g_file_info_get_attribute_boolean (info, REJILLA_IO_HAS_AUDIO)) {
		if (g_file_info_get_attribute_uint64 (info, REJILLA_IO_LEN) <= 0) {
			rejilla_player_no_multimedia_stream (player);
			g_signal_emit (player,
				       rejilla_player_signals [ERROR_SIGNAL],
				       0);
			return;
		}

		/* Audio */
		player->priv->is_video = FALSE;
		gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
		gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 4);

		rejilla_player_update_tooltip (player, info);
	}
	else if (mime && !strncmp ("image/", mime, 6)) {
		gchar *size_string;
		gchar *string;
		gchar *path;
		gchar *name;
		gint height;
		gint width;

		/* Only do that if the image is < 20 M otherwise that's crap
		 * FIXME: maybe a sort of error message here? or use thumbnail? */
		if (g_file_info_get_size (info) > 100000000LL) {
			rejilla_player_no_multimedia_stream (player);
			g_signal_emit (player,
				       rejilla_player_signals [ERROR_SIGNAL],
				       0);
			return;
		}

		rejilla_player_image (player);

		path = g_filename_from_uri (player->priv->uri, NULL, NULL);
		REJILLA_GET_BASENAME_FOR_DISPLAY (path, name);
		g_free (path);

		height = gdk_pixbuf_get_height (player->priv->pixbuf);
		width = gdk_pixbuf_get_width (player->priv->pixbuf);
		size_string = g_strdup_printf (_("%i \303\227 %i pixels"), width, height);

		string = g_strdup_printf ("<span weight=\"bold\">%s</span>\n"
		                          "<i><span size=\"smaller\">%s</span></i>",
		                          name,
		                          size_string);
		g_free (name);
		g_free (size_string);

		gtk_widget_set_tooltip_markup (GTK_WIDGET (player), string);
		g_free (string);
		return;
	}
	else {
		rejilla_player_no_multimedia_stream (player);
		g_signal_emit (player,
			       rejilla_player_signals [ERROR_SIGNAL],
			       0);
	       return;
	}

	if (player->priv->end <= 0)
		player->priv->end = g_file_info_get_attribute_uint64 (info, REJILLA_IO_LEN);

	player->priv->length = g_file_info_get_attribute_uint64 (info, REJILLA_IO_LEN);

	player->priv->state = BACON_STATE_READY;
	g_signal_emit (player,
		       rejilla_player_signals [READY_SIGNAL],
		       0);
}

static void
rejilla_player_retrieve_metadata (RejillaPlayer *player)
{
	if (!player->priv->meta_task)
		player->priv->meta_task = rejilla_io_register (G_OBJECT (player),
							       rejilla_player_metadata_completed,
							       NULL,
							       NULL);

	rejilla_io_get_file_info (player->priv->uri,
				  player->priv->meta_task,
				  REJILLA_IO_INFO_METADATA|
				  REJILLA_IO_INFO_MIME,
				  NULL);
}

void
rejilla_player_set_boundaries (RejillaPlayer *player, 
			       gint64 start,
			       gint64 end)
{
	if (start <= 0)
		player->priv->start = 0;
	else
		player->priv->start = start;

	if (end <= 0)
		player->priv->end = player->priv->length;
	else
		player->priv->end = end;

	if (player->priv->bacon)
		rejilla_player_bacon_set_boundaries (REJILLA_PLAYER_BACON (player->priv->bacon),
						     player->priv->start,
						     player->priv->end);
}

void
rejilla_player_set_uri (RejillaPlayer *player,
			const gchar *uri)
{
	/* avoid reloading everything if it's the same uri */
	if (!g_strcmp0 (uri, player->priv->uri)) {
		/* if it's not loaded yet just return */
		/* the existence of progress is the surest way to know
		 * if that uri was successfully loaded */
		if (uri)
			g_signal_emit (player,
				       rejilla_player_signals [READY_SIGNAL],
				       0);
		else
			g_signal_emit (player,
				       rejilla_player_signals [ERROR_SIGNAL],
				       0);
		return;
	}

	if (player->priv->uri)
		g_free (player->priv->uri);

	player->priv->uri = g_strdup (uri);
	player->priv->length = 0;
	player->priv->start = 0;
	player->priv->end = 0;

	if (player->priv->meta_task)
		rejilla_io_cancel_by_base (player->priv->meta_task);

	/* That stops the pipeline from playing */
	rejilla_player_bacon_set_uri (REJILLA_PLAYER_BACON (player->priv->bacon), NULL);
	if (!uri) {
		rejilla_player_no_multimedia_stream (player);
		return;
	}

	gtk_widget_set_has_tooltip (GTK_WIDGET (player), FALSE);
	gtk_notebook_set_current_page (GTK_NOTEBOOK (player->priv->notebook), 3);
	gtk_spinner_start (GTK_SPINNER (player->priv->spinner));

	rejilla_player_retrieve_metadata (player);
}

static void
rejilla_player_eof_cb (RejillaPlayerBacon *bacon, RejillaPlayer *player)
{
	rejilla_player_bacon_stop (REJILLA_PLAYER_BACON (player->priv->bacon));
	rejilla_player_bacon_set_pos (REJILLA_PLAYER_BACON (player->priv->bacon), player->priv->start);
	player->priv->state = BACON_STATE_PAUSED;
	gtk_image_set_from_stock (GTK_IMAGE (player->priv->play_image), GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
}

static void
rejilla_player_state_changed_cb (RejillaPlayerBacon *bacon,
				 RejillaPlayerBaconState state,
				 RejillaPlayer *player)
{
	if (player->priv->state == state)
		return;

	switch (state) {
	case BACON_STATE_ERROR:
		rejilla_player_no_multimedia_stream (player);
		break;

	default:
		break;
	}

	player->priv->state = state;
}

static void
rejilla_player_destroy (GtkObject *obj)
{
	RejillaPlayer *player;

	player = REJILLA_PLAYER (obj);

	rejilla_setting_set_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_IMAGE_SIZE_WIDTH,
	                           GINT_TO_POINTER (player->priv->image_width));
	rejilla_setting_set_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_IMAGE_SIZE_HEIGHT,
	                           GINT_TO_POINTER (player->priv->image_height));
	rejilla_setting_set_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_VIDEO_SIZE_WIDTH,
	                           GINT_TO_POINTER (player->priv->video_width));
	rejilla_setting_set_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_VIDEO_SIZE_HEIGHT,
	                           GINT_TO_POINTER (player->priv->video_height));

	if (player->priv->pixbuf) {
		g_object_unref (player->priv->pixbuf);
		player->priv->pixbuf = NULL;
	}

	if (player->priv->uri) {
		g_free (player->priv->uri);
		player->priv->uri = NULL;
	}

	if (player->priv->meta_task){
		rejilla_io_cancel_by_base (player->priv->meta_task);
		rejilla_io_job_base_free (player->priv->meta_task);
		player->priv->meta_task = 0;
	}

	if (GTK_OBJECT_CLASS (parent_class)->destroy)
		GTK_OBJECT_CLASS (parent_class)->destroy (obj);
}

static void
rejilla_player_finalize (GObject *object)
{
	RejillaPlayer *cobj;

	cobj = REJILLA_PLAYER (object);

	g_free (cobj->priv);
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
rejilla_player_class_init (RejillaPlayerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkObjectClass *gtk_object_class = GTK_OBJECT_CLASS (klass);

	parent_class = g_type_class_peek_parent (klass);
	object_class->finalize = rejilla_player_finalize;

	gtk_object_class->destroy = rejilla_player_destroy;

	rejilla_player_signals [ERROR_SIGNAL] = 
			g_signal_new ("error",
				      G_TYPE_FROM_CLASS (klass),
				      G_SIGNAL_RUN_LAST,
				      G_STRUCT_OFFSET (RejillaPlayerClass, error),
				      NULL, NULL,
				      g_cclosure_marshal_VOID__VOID,
				      G_TYPE_NONE, 0);
	rejilla_player_signals [READY_SIGNAL] = 
			g_signal_new ("ready",
				      G_TYPE_FROM_CLASS (klass),
				      G_SIGNAL_RUN_LAST,
				      G_STRUCT_OFFSET (RejillaPlayerClass, ready),
				      NULL, NULL,
				      g_cclosure_marshal_VOID__VOID,
				      G_TYPE_NONE, 0);
}

static void
rejilla_player_volume_changed_cb (GtkScaleButton *button,
				  gdouble volume,
				  RejillaPlayer *player)
{
	rejilla_player_bacon_set_volume (REJILLA_PLAYER_BACON (player->priv->bacon), volume);
}

static void
rejilla_player_init (RejillaPlayer *obj)
{
	GtkWidget *volume;
	GtkWidget *image;
	GtkWidget *label;
	GtkWidget *event;
	GtkWidget *zoom;
	GtkWidget *hbox;
	GtkWidget *vbox;
	gpointer value;
	gchar *string;

	obj->priv = g_new0 (RejillaPlayerPrivate, 1);

	gtk_alignment_set (GTK_ALIGNMENT (obj), 0.5, 0.5, 0.0, 0.0);

	vbox = gtk_vbox_new (FALSE, 2);
	gtk_widget_show (vbox);
	gtk_container_add (GTK_CONTAINER (obj), vbox);
	
	/* The notebook and all views */
	event = gtk_event_box_new ();
	gtk_box_pack_start (GTK_BOX (vbox), event, FALSE, FALSE, 0);
	gtk_event_box_set_above_child (GTK_EVENT_BOX (event), TRUE);
	gtk_event_box_set_visible_window (GTK_EVENT_BOX (event), FALSE);
	g_signal_connect (event,
			  "button-release-event",
			  G_CALLBACK (rejilla_player_button_release),
			  obj);
	g_signal_connect (event,
			  "scroll-event",
			  G_CALLBACK (rejilla_player_scroll),
			  obj);
	gtk_widget_show (event);

	obj->priv->notebook = gtk_notebook_new ();
	gtk_widget_show (obj->priv->notebook);
	gtk_container_add (GTK_CONTAINER (event), obj->priv->notebook);
	gtk_notebook_set_show_tabs (GTK_NOTEBOOK (obj->priv->notebook), FALSE);

	/* Images */
	obj->priv->image_display = gtk_image_new ();
	gtk_widget_show (obj->priv->image_display);
	gtk_misc_set_alignment (GTK_MISC (obj->priv->image_display), 0.5, 0.5);
	gtk_notebook_append_page (GTK_NOTEBOOK (obj->priv->notebook),
				  obj->priv->image_display,
				  NULL);

	/* Video */
	obj->priv->bacon = rejilla_player_bacon_new ();
	gtk_widget_show (obj->priv->bacon);
	g_signal_connect (obj->priv->bacon,
			  "button-release-event",
			  G_CALLBACK (rejilla_bacon_button_release),
			  obj);
	g_signal_connect (obj->priv->bacon,
			  "scroll-event",
			  G_CALLBACK (rejilla_bacon_scroll),
			  obj);
	g_signal_connect (obj->priv->bacon,
			  "state-change",
			  G_CALLBACK (rejilla_player_state_changed_cb),
			  obj);
	g_signal_connect (obj->priv->bacon,
			  "eof",
			  G_CALLBACK (rejilla_player_eof_cb),
			  obj);

	gtk_notebook_append_page (GTK_NOTEBOOK (obj->priv->notebook),
				  obj->priv->bacon,
				  NULL);

	/* No Preview view */
	string = g_strdup_printf ("<span color='grey'><big><b>%s</b></big></span>", _("No preview"));
	label = gtk_label_new (string);
	g_free (string);

	gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
	gtk_widget_show (label);
	gtk_notebook_append_page (GTK_NOTEBOOK (obj->priv->notebook),
	                          label,
	                          NULL);
	gtk_notebook_set_current_page (GTK_NOTEBOOK (obj->priv->notebook), 2);

	/* Loading view */
	obj->priv->spinner = gtk_spinner_new ();
	gtk_widget_show (obj->priv->spinner);
	gtk_notebook_append_page (GTK_NOTEBOOK (obj->priv->notebook),
	                          obj->priv->spinner,
	                          NULL);

	/* Music */
	image = gtk_image_new_from_stock (GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_DIALOG);
	gtk_widget_show (image);
	gtk_notebook_append_page (GTK_NOTEBOOK (obj->priv->notebook),
	                          image,
	                          NULL);
	obj->priv->play_image = image;

	/* Set the saved sizes */
	rejilla_setting_get_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_IMAGE_SIZE_WIDTH,
	                           &value);
	obj->priv->image_width = GPOINTER_TO_INT (value);

	if (obj->priv->image_width > PLAYER_BACON_WIDTH * 3
	||  obj->priv->image_width < PLAYER_BACON_WIDTH)
		obj->priv->image_width = PLAYER_BACON_WIDTH;

	rejilla_setting_get_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_IMAGE_SIZE_HEIGHT,
	                           &value);
	obj->priv->image_height = GPOINTER_TO_INT (value);

	if (obj->priv->image_height > PLAYER_BACON_HEIGHT * 3
	||  obj->priv->image_height < PLAYER_BACON_HEIGHT)
		obj->priv->image_height = PLAYER_BACON_HEIGHT;

	rejilla_setting_get_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_VIDEO_SIZE_WIDTH,
	                           &value);
	obj->priv->video_width = GPOINTER_TO_INT (value);

	if (obj->priv->video_width > PLAYER_BACON_WIDTH * 3
	||  obj->priv->video_width < PLAYER_BACON_WIDTH)
		obj->priv->video_width = PLAYER_BACON_WIDTH;

	rejilla_setting_get_value (rejilla_setting_get_default (),
	                           REJILLA_SETTING_VIDEO_SIZE_HEIGHT,
	                           &value);
	obj->priv->video_height = GPOINTER_TO_INT (value);

	if (obj->priv->video_height > PLAYER_BACON_HEIGHT * 3
	||  obj->priv->video_height < PLAYER_BACON_HEIGHT)
		obj->priv->video_height = PLAYER_BACON_HEIGHT;

	gtk_widget_set_size_request (obj->priv->bacon,
				     obj->priv->video_width,
				     obj->priv->video_height);

	/* A few controls */
	hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

	image = gtk_image_new_from_stock (GTK_STOCK_ZOOM_OUT, GTK_ICON_SIZE_BUTTON);
	zoom = gtk_button_new ();
	gtk_widget_show (zoom);
	gtk_widget_show (image);
	gtk_button_set_image (GTK_BUTTON (zoom), image);
	gtk_button_set_relief (GTK_BUTTON (zoom), GTK_RELIEF_NONE);
	gtk_button_set_focus_on_click (GTK_BUTTON (zoom), FALSE);
	g_signal_connect (zoom,
			  "clicked",
			  G_CALLBACK (rejilla_player_zoom_out_cb),
			  obj);
	gtk_box_pack_start (GTK_BOX (hbox),
	                    zoom,
	                    FALSE,
	                    FALSE,
	                    0);
	obj->priv->zoom_out = zoom;

	image = gtk_image_new_from_stock (GTK_STOCK_ZOOM_IN, GTK_ICON_SIZE_BUTTON);
	zoom = gtk_button_new ();
	gtk_widget_show (zoom);
	gtk_widget_show (image);
	gtk_button_set_image (GTK_BUTTON (zoom), image);
	gtk_button_set_relief (GTK_BUTTON (zoom), GTK_RELIEF_NONE);
	gtk_button_set_focus_on_click (GTK_BUTTON (zoom), FALSE);
	g_signal_connect (zoom,
			  "clicked",
			  G_CALLBACK (rejilla_player_zoom_in_cb),
			  obj);
	gtk_box_pack_start (GTK_BOX (hbox),
	                    zoom,
	                    FALSE,
	                    FALSE,
	                    0);
	obj->priv->zoom_in = zoom;

	volume = gtk_volume_button_new ();
	gtk_widget_show (volume);
	gtk_box_pack_end (GTK_BOX (hbox),
			  volume,
			  FALSE,
			  FALSE,
			  0);

	gtk_scale_button_set_value (GTK_SCALE_BUTTON (volume),
				    rejilla_player_bacon_get_volume (REJILLA_PLAYER_BACON (obj->priv->bacon)));

	g_signal_connect (volume,
			  "value-changed",
			  G_CALLBACK (rejilla_player_volume_changed_cb),
			  obj);
}

GtkWidget *
rejilla_player_new ()
{
	RejillaPlayer *obj;

	obj = REJILLA_PLAYER (g_object_new (REJILLA_TYPE_PLAYER, NULL));

	return GTK_WIDGET (obj);
}
