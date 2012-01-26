/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Librejilla-burn
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Librejilla-burn is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-burn authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-burn. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-burn is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-burn is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <gmodule.h>

#include <libisofs/libisofs.h>
#include <libburn/libburn.h>

#include "burn-libburnia.h"
#include "burn-job.h"
#include "rejilla-units.h"
#include "rejilla-plugin-registration.h"
#include "burn-libburn-common.h"
#include "rejilla-track-data.h"
#include "rejilla-track-image.h"


#define REJILLA_TYPE_LIBISOFS         (rejilla_libisofs_get_type ())
#define REJILLA_LIBISOFS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), REJILLA_TYPE_LIBISOFS, RejillaLibisofs))
#define REJILLA_LIBISOFS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), REJILLA_TYPE_LIBISOFS, RejillaLibisofsClass))
#define REJILLA_IS_LIBISOFS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), REJILLA_TYPE_LIBISOFS))
#define REJILLA_IS_LIBISOFS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), REJILLA_TYPE_LIBISOFS))
#define REJILLA_LIBISOFS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), REJILLA_TYPE_LIBISOFS, RejillaLibisofsClass))

REJILLA_PLUGIN_BOILERPLATE (RejillaLibisofs, rejilla_libisofs, REJILLA_TYPE_JOB, RejillaJob);

struct _RejillaLibisofsPrivate {
	struct burn_source *libburn_src;

	/* that's for multisession */
	RejillaLibburnCtx *ctx;

	GError *error;
	GThread *thread;
	GMutex *mutex;
	GCond *cond;
	guint thread_id;

	guint cancel:1;
};
typedef struct _RejillaLibisofsPrivate RejillaLibisofsPrivate;

#define REJILLA_LIBISOFS_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), REJILLA_TYPE_LIBISOFS, RejillaLibisofsPrivate))

static GObjectClass *parent_class = NULL;

static gboolean
rejilla_libisofs_thread_finished (gpointer data)
{
	RejillaLibisofs *self = data;
	RejillaLibisofsPrivate *priv;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	priv->thread_id = 0;
	if (priv->error) {
		GError *error;

		error = priv->error;
		priv->error = NULL;
		rejilla_job_error (REJILLA_JOB (self), error);
		return FALSE;
	}

	if (rejilla_job_get_fd_out (REJILLA_JOB (self), NULL) != REJILLA_BURN_OK) {
		RejillaTrackImage *track = NULL;
		gchar *output = NULL;
		goffset blocks = 0;

		/* Let's make a track */
		track = rejilla_track_image_new ();
		rejilla_job_get_image_output (REJILLA_JOB (self),
					      &output,
					      NULL);
		rejilla_track_image_set_source (track,
						output,
						NULL,
						REJILLA_IMAGE_FORMAT_BIN);

		rejilla_job_get_session_output_size (REJILLA_JOB (self), &blocks, NULL);
		rejilla_track_image_set_block_num (track, blocks);

		rejilla_job_add_track (REJILLA_JOB (self), REJILLA_TRACK (track));
		g_object_unref (track);
	}

	rejilla_job_finished_track (REJILLA_JOB (self));
	return FALSE;
}

static RejillaBurnResult
rejilla_libisofs_write_sector_to_fd (RejillaLibisofs *self,
				     int fd,
				     gpointer buffer,
				     gint bytes_remaining)
{
	gint bytes_written = 0;
	RejillaLibisofsPrivate *priv;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	while (bytes_remaining) {
		gint written;

		written = write (fd,
				 ((gchar *) buffer) + bytes_written,
				 bytes_remaining);

		if (priv->cancel)
			break;

		if (written != bytes_remaining) {
			if (errno != EINTR && errno != EAGAIN) {
                                int errsv = errno;

				/* unrecoverable error */
				priv->error = g_error_new (REJILLA_BURN_ERROR,
							   REJILLA_BURN_ERROR_GENERAL,
							   _("Data could not be written (%s)"),
							   g_strerror (errsv));
				return REJILLA_BURN_ERR;
			}

			g_thread_yield ();
		}

		if (written > 0) {
			bytes_remaining -= written;
			bytes_written += written;
		}
	}

	return REJILLA_BURN_OK;
}

static void
rejilla_libisofs_write_image_to_fd_thread (RejillaLibisofs *self)
{
	const gint sector_size = 2048;
	RejillaLibisofsPrivate *priv;
	gint64 written_sectors = 0;
	RejillaBurnResult result;
	guchar buf [sector_size];
	int read_bytes;
	int fd = -1;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	rejilla_job_set_nonblocking (REJILLA_JOB (self), NULL);

	rejilla_job_set_current_action (REJILLA_JOB (self),
					REJILLA_BURN_ACTION_CREATING_IMAGE,
					NULL,
					FALSE);

	rejilla_job_start_progress (REJILLA_JOB (self), FALSE);
	rejilla_job_get_fd_out (REJILLA_JOB (self), &fd);

	REJILLA_JOB_LOG (self, "Writing to pipe");
	read_bytes = priv->libburn_src->read_xt (priv->libburn_src, buf, sector_size);
	while (priv->libburn_src->read_xt (priv->libburn_src, buf, sector_size) == sector_size) {
		if (priv->cancel)
			break;

		result = rejilla_libisofs_write_sector_to_fd (self,
							      fd,
							      buf,
							      sector_size);
		if (result != REJILLA_BURN_OK)
			break;

		written_sectors ++;
		rejilla_job_set_written_track (REJILLA_JOB (self), written_sectors << 11);

		read_bytes = priv->libburn_src->read_xt (priv->libburn_src, buf, sector_size);
	}

	if (read_bytes == -1 && !priv->error)
		priv->error = g_error_new (REJILLA_BURN_ERROR,
					   REJILLA_BURN_ERROR_GENERAL,
					   _("Volume could not be created"));
}

static void
rejilla_libisofs_write_image_to_file_thread (RejillaLibisofs *self)
{
	const gint sector_size = 2048;
	RejillaLibisofsPrivate *priv;
	gint64 written_sectors = 0;
	guchar buf [sector_size];
	int read_bytes;
	gchar *output;
	FILE *file;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	rejilla_job_get_image_output (REJILLA_JOB (self), &output, NULL);
	file = fopen (output, "w");
	if (!file) {
		int errnum = errno;

		if (errno == EACCES)
			priv->error = g_error_new_literal (REJILLA_BURN_ERROR,
							   REJILLA_BURN_ERROR_PERMISSION,
							   _("You do not have the required permission to write at this location"));
		else
			priv->error = g_error_new_literal (REJILLA_BURN_ERROR,
							   REJILLA_BURN_ERROR_GENERAL,
							   g_strerror (errnum));
		return;
	}

	REJILLA_JOB_LOG (self, "writing to file %s", output);

	rejilla_job_set_current_action (REJILLA_JOB (self),
					REJILLA_BURN_ACTION_CREATING_IMAGE,
					NULL,
					FALSE);

	priv = REJILLA_LIBISOFS_PRIVATE (self);
	rejilla_job_start_progress (REJILLA_JOB (self), FALSE);

	read_bytes = priv->libburn_src->read_xt (priv->libburn_src, buf, sector_size);
	while (read_bytes == sector_size) {
		if (priv->cancel)
			break;

		if (fwrite (buf, 1, sector_size, file) != sector_size) {
                        int errsv = errno;

			priv->error = g_error_new (REJILLA_BURN_ERROR,
						   REJILLA_BURN_ERROR_GENERAL,
						   _("Data could not be written (%s)"),
						   g_strerror (errsv));
			break;
		}

		if (priv->cancel)
			break;

		written_sectors ++;
		rejilla_job_set_written_track (REJILLA_JOB (self), written_sectors << 11);

		read_bytes = priv->libburn_src->read_xt (priv->libburn_src, buf, sector_size);
	}

	if (read_bytes == -1 && !priv->error)
		priv->error = g_error_new (REJILLA_BURN_ERROR,
					   REJILLA_BURN_ERROR_GENERAL,
					   _("Volume could not be created"));

	fclose (file);
	file = NULL;
}

static gpointer
rejilla_libisofs_thread_started (gpointer data)
{
	RejillaLibisofsPrivate *priv;
	RejillaLibisofs *self;

	self = REJILLA_LIBISOFS (data);
	priv = REJILLA_LIBISOFS_PRIVATE (self);

	REJILLA_JOB_LOG (self, "Entering thread");
	if (rejilla_job_get_fd_out (REJILLA_JOB (self), NULL) == REJILLA_BURN_OK)
		rejilla_libisofs_write_image_to_fd_thread (self);
	else
		rejilla_libisofs_write_image_to_file_thread (self);

	REJILLA_JOB_LOG (self, "Getting out thread");

	/* End thread */
	g_mutex_lock (priv->mutex);

	if (!priv->cancel)
		priv->thread_id = g_idle_add (rejilla_libisofs_thread_finished, self);

	priv->thread = NULL;
	g_cond_signal (priv->cond);
	g_mutex_unlock (priv->mutex);

	g_thread_exit (NULL);

	return NULL;
}

static RejillaBurnResult
rejilla_libisofs_create_image (RejillaLibisofs *self,
			       GError **error)
{
	RejillaLibisofsPrivate *priv;
	GError *thread_error = NULL;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	if (priv->thread)
		return REJILLA_BURN_RUNNING;

	if (iso_init () < 0) {
		g_set_error (error,
			     REJILLA_BURN_ERROR,
			     REJILLA_BURN_ERROR_GENERAL,
			     _("libisofs could not be initialized."));
		return REJILLA_BURN_ERR;
	}

	iso_set_msgs_severities ("NEVER", "ALL", "rejilla (libisofs)");

	g_mutex_lock (priv->mutex);
	priv->thread = g_thread_create (rejilla_libisofs_thread_started,
					self,
					FALSE,
					&thread_error);
	g_mutex_unlock (priv->mutex);

	/* Reminder: this is not necessarily an error as the thread may have finished */
	//if (!priv->thread)
	//	return REJILLA_BURN_ERR;

	if (thread_error) {
		g_propagate_error (error, thread_error);
		return REJILLA_BURN_ERR;
	}

	return REJILLA_BURN_OK;
}

static gboolean
rejilla_libisofs_create_volume_thread_finished (gpointer data)
{
	RejillaLibisofs *self = data;
	RejillaLibisofsPrivate *priv;
	RejillaJobAction action;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	priv->thread_id = 0;
	if (priv->error) {
		GError *error;

		error = priv->error;
		priv->error = NULL;
		rejilla_job_error (REJILLA_JOB (self), error);
		return FALSE;
	}

	rejilla_job_get_action (REJILLA_JOB (self), &action);
	if (action == REJILLA_JOB_ACTION_IMAGE) {
		RejillaBurnResult result;
		GError *error = NULL;

		result = rejilla_libisofs_create_image (self, &error);
		if (error)
		rejilla_job_error (REJILLA_JOB (self), error);
		else
			return FALSE;
	}

	rejilla_job_finished_track (REJILLA_JOB (self));
	return FALSE;
}

static gint
rejilla_libisofs_sort_graft_points (gconstpointer a, gconstpointer b)
{
	const RejillaGraftPt *graft_a, *graft_b;
	gint len_a, len_b;

	graft_a = a;
	graft_b = b;

	/* we only want to know if:
	 * - a is a parent of b (a > b, retval < 0) 
	 * - b is a parent of a (b > a, retval > 0). */
	len_a = strlen (graft_a->path);
	len_b = strlen (graft_b->path);

	return len_a - len_b;
}

static int 
rejilla_libisofs_import_read (IsoDataSource *src, uint32_t lba, uint8_t *buffer)
{
	struct burn_drive *d;
	off_t data_count;
	gint result;

	d = (struct burn_drive*)src->data;

	result = burn_read_data(d,
				(off_t) lba * (off_t) 2048,
				(char*)buffer, 
				2048,
				&data_count,
				0);
	if (result < 0 )
		return -1; /* error */

	return 1;
}

static int
rejilla_libisofs_import_open (IsoDataSource *src)
{
	return 1;
}

static int
rejilla_libisofs_import_close (IsoDataSource *src)
{
	return 1;
}
    
static void 
rejilla_libisofs_import_free (IsoDataSource *src)
{ }

static RejillaBurnResult
rejilla_libisofs_import_last_session (RejillaLibisofs *self,
				      IsoImage *image,
				      IsoWriteOpts *wopts,
				      GError **error)
{
	int result;
	IsoReadOpts *opts;
	RejillaMedia media;
	IsoDataSource *src;
	goffset start_block;
	goffset session_block;
	RejillaLibisofsPrivate *priv;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	priv->ctx = rejilla_libburn_common_ctx_new (REJILLA_JOB (self), FALSE, error);
	if (!priv->ctx)
		return REJILLA_BURN_ERR;

	result = iso_read_opts_new (&opts, 0);
	if (result < 0) {
		g_set_error (error,
			     REJILLA_BURN_ERROR,
			     REJILLA_BURN_ERROR_GENERAL,
			     _("Read options could not be created"));
		return REJILLA_BURN_ERR;
	}

	src = g_new0 (IsoDataSource, 1);
	src->version = 0;
	src->refcount = 1;
	src->read_block = rejilla_libisofs_import_read;
	src->open = rejilla_libisofs_import_open;
	src->close = rejilla_libisofs_import_close;
	src->free_data = rejilla_libisofs_import_free;
	src->data = priv->ctx->drive;

	rejilla_job_get_last_session_address (REJILLA_JOB (self), &session_block);
	iso_read_opts_set_start_block (opts, session_block);

	/* import image */
	result = iso_image_import (image, src, opts, NULL);
	iso_data_source_unref (src);
	iso_read_opts_free (opts);

	/* release the drive */
	if (priv->ctx) {
		/* This may not be a good idea ...*/
		rejilla_libburn_common_ctx_free (priv->ctx);
		priv->ctx = NULL;
	}

	if (result < 0) {
		REJILLA_JOB_LOG (self, "Import failed 0x%x", result);
		g_set_error (error,
			     REJILLA_BURN_ERROR,
			     REJILLA_BURN_ERROR_IMAGE_LAST_SESSION,
			     _("Last session import failed"));	
		return REJILLA_BURN_ERR;
	}

	/* check is this is a DVD+RW */
	rejilla_job_get_next_writable_address (REJILLA_JOB (self), &start_block);

	rejilla_job_get_media (REJILLA_JOB (self), &media);
	if (REJILLA_MEDIUM_IS (media, REJILLA_MEDIUM_DVDRW_PLUS)
	||  REJILLA_MEDIUM_IS (media, REJILLA_MEDIUM_DVDRW_RESTRICTED)
	||  REJILLA_MEDIUM_IS (media, REJILLA_MEDIUM_DVDRW_PLUS_DL)) {
		/* This is specific to overwrite media; the start address is the
		 * size of all the previous data written */
		REJILLA_JOB_LOG (self, "Growing image (start %i)", start_block);
	}

	/* set the start block for the multisession image */
	iso_write_opts_set_ms_block (wopts, start_block);
	iso_write_opts_set_appendable (wopts, 1);

	iso_tree_set_replace_mode (image, ISO_REPLACE_ALWAYS);
	return REJILLA_BURN_OK;
}

static gpointer
rejilla_libisofs_create_volume_thread (gpointer data)
{
	RejillaLibisofs *self = REJILLA_LIBISOFS (data);
	RejillaLibisofsPrivate *priv;
	RejillaTrack *track = NULL;
	IsoWriteOpts *opts = NULL;
	IsoImage *image = NULL;
	RejillaBurnFlag flags;
	GSList *grafts = NULL;
	gchar *label = NULL;
	gchar *publisher;
	GSList *excluded;
	GSList *iter;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	if (priv->libburn_src) {
		burn_source_free (priv->libburn_src);
		priv->libburn_src = NULL;
	}

	REJILLA_JOB_LOG (self, "creating volume");

	/* create volume */
	rejilla_job_get_data_label (REJILLA_JOB (self), &label);
	if (!iso_image_new (label, &image)) {
		priv->error = g_error_new (REJILLA_BURN_ERROR,
					   REJILLA_BURN_ERROR_GENERAL,
					   _("Volume could not be created"));
		g_free (label);
		goto end;
	}

	iso_write_opts_new (&opts, 2);
	iso_write_opts_set_relaxed_vol_atts(opts, 1);

	rejilla_job_get_flags (REJILLA_JOB (self), &flags);
	if (flags & REJILLA_BURN_FLAG_MERGE) {
		RejillaBurnResult result;

		result = rejilla_libisofs_import_last_session (self,
							       image,
							       opts,
							       &priv->error);
		if (result != REJILLA_BURN_OK) {
			g_free (label);
			goto end;
		}
	}
	else if (flags & REJILLA_BURN_FLAG_APPEND) {
		goffset start_block;

		rejilla_job_get_next_writable_address (REJILLA_JOB (self), &start_block);
		iso_write_opts_set_ms_block (opts, start_block);
	}

	/* set label but set it after merging so the
	 * new does not get replaced by the former */
	publisher = g_strdup_printf ("Rejilla-%i.%i.%i",
				     REJILLA_MAJOR_VERSION,
				     REJILLA_MINOR_VERSION,
				     REJILLA_SUB);

	if (label)
		iso_image_set_volume_id (image, label);

	iso_image_set_publisher_id (image, publisher);
	iso_image_set_data_preparer_id (image, g_get_real_name ());

	g_free (publisher);
	g_free (label);

	rejilla_job_start_progress (REJILLA_JOB (self), FALSE);

	/* copy the list as we're going to reorder it */
	rejilla_job_get_current_track (REJILLA_JOB (self), &track);
	grafts = rejilla_track_data_get_grafts (REJILLA_TRACK_DATA (track));
	grafts = g_slist_copy (grafts);
	grafts = g_slist_sort (grafts, rejilla_libisofs_sort_graft_points);

	/* add global exclusions */
	for (excluded = rejilla_track_data_get_excluded_list (REJILLA_TRACK_DATA (track));
	     excluded; excluded = excluded->next) {
		gchar *uri, *local;

		uri = excluded->data;
		local = g_filename_from_uri (uri, NULL, NULL);
		iso_tree_add_exclude (image, local);
		g_free (local);
	}

	for (iter = grafts; iter; iter = iter->next) {
		RejillaGraftPt *graft;
		gboolean is_directory;
		gchar *path_parent;
		gchar *path_name;
		IsoNode *parent;

		if (priv->cancel)
			goto end;

		graft = iter->data;

		REJILLA_JOB_LOG (self,
				 "Adding graft disc path = %s, URI = %s",
				 graft->path,
				 graft->uri);

		/* search for parent node.
		 * NOTE: because of mkisofs/genisoimage, we add a "/" at the end
		 * directories. So make sure there isn't one when getting the 
		 * parent path or g_path_get_dirname () will return the same
		 * exact name */
		if (g_str_has_suffix (graft->path, G_DIR_SEPARATOR_S)) {
			gchar *tmp;

			/* remove trailing "/" */
			tmp = g_strdup (graft->path);
			tmp [strlen (tmp) - 1] = '\0';
			path_parent = g_path_get_dirname (tmp);
			path_name = g_path_get_basename (tmp);
			g_free (tmp);

			is_directory = TRUE;
		}
		else {
			path_parent = g_path_get_dirname (graft->path);
			path_name = g_path_get_basename (graft->path);
			is_directory = FALSE;
		}

		iso_tree_path_to_node (image, path_parent, &parent);
		g_free (path_parent);

		if (!parent) {
			/* an error has occurred, possibly libisofs hasn't been
			 * able to find a parent for this node */
			g_free (path_name);
			priv->error = g_error_new (REJILLA_BURN_ERROR,
						   REJILLA_BURN_ERROR_GENERAL,
						   /* Translators: %s is the path */
						   _("No parent could be found in the tree for the path \"%s\""),
						   graft->path);
			goto end;
		}

		REJILLA_JOB_LOG (self, "Found parent");

		/* add the file/directory to the volume */
		if (graft->uri) {
			gchar *local_path;
			IsoDirIter *sibling;

			/* graft->uri can be a path or a URI */
			if (graft->uri [0] == '/')
				local_path = g_strdup (graft->uri);
			else if (g_str_has_prefix (graft->uri, "file://"))
				local_path = g_filename_from_uri (graft->uri, NULL, NULL);
			else
				local_path = NULL;

			if (!local_path){
				priv->error = g_error_new (REJILLA_BURN_ERROR,
							   REJILLA_BURN_ERROR_FILE_NOT_LOCAL,
							   _("The file is not stored locally"));
				g_free (path_name);
				goto end;
			}

			/* see if the node exists with the same name among the 
			 * children of the parent directory. If there is a
			 * sibling destroy it. */
			sibling = NULL;
			iso_dir_get_children (ISO_DIR (parent), &sibling);

			IsoNode *node;
			while (iso_dir_iter_next (sibling, &node) == 1) {
				const gchar *iso_name;

				/* check if it has the same name */
				iso_name = iso_node_get_name (node);
				if (iso_name && !strcmp (iso_name, path_name))
					REJILLA_JOB_LOG (self,
							 "Found sibling for %s: removing %x",
							 path_name,
							 iso_dir_iter_remove (sibling));
			}

			if  (is_directory) {
				int result;
				IsoDir *directory;

				/* add directory node */
				result = iso_tree_add_new_dir (ISO_DIR (parent), path_name, &directory);
				if (result < 0) {
					REJILLA_JOB_LOG (self,
							 "ERROR %s %x",
							 path_name,
							 result);
					priv->error = g_error_new (REJILLA_BURN_ERROR,
								   REJILLA_BURN_ERROR_GENERAL,
								   _("libisofs reported an error while creating directory \"%s\""),
								   graft->path);
					g_free (path_name);
					goto end;
				}

				/* add contents */
				result = iso_tree_add_dir_rec (image, directory, local_path);
				if (result < 0) {
					REJILLA_JOB_LOG (self,
							 "ERROR %s %x",
							 path_name,
							 result);
					priv->error = g_error_new (REJILLA_BURN_ERROR,
								   REJILLA_BURN_ERROR_GENERAL,
								   _("libisofs reported an error while adding contents to directory \"%s\" (%x)"),
								   graft->path,
								   result);
					g_free (path_name);
					goto end;
				}
			}
			else {
				IsoNode *node;
				int err;

				err = iso_tree_add_new_node (image,
							 ISO_DIR (parent),
				                         path_name,
							 local_path,
							 &node);
				if (err < 0) {
					REJILLA_JOB_LOG (self,
							 "ERROR %s %x",
							 path_name,
							 err);
					priv->error = g_error_new (REJILLA_BURN_ERROR,
								   REJILLA_BURN_ERROR_GENERAL,
								   _("libisofs reported an error while adding file at path \"%s\""),
								   graft->path);
					g_free (path_name);
					goto end;
				}

				if (iso_node_get_name (node)
				&&  strcmp (iso_node_get_name (node), path_name)) {
					err = iso_node_set_name (node, path_name);
					if (err < 0) {
						REJILLA_JOB_LOG (self,
								 "ERROR %s %x",
								 path_name,
								 err);
						priv->error = g_error_new (REJILLA_BURN_ERROR,
									   REJILLA_BURN_ERROR_GENERAL,
									   _("libisofs reported an error while adding file at path \"%s\""),
									   graft->path);
						g_free (path_name);
						goto end;
					}
				}
			}

			g_free (local_path);
		}
		else if (iso_tree_add_new_dir (ISO_DIR (parent), path_name, NULL) < 0) {
			priv->error = g_error_new (REJILLA_BURN_ERROR,
						   REJILLA_BURN_ERROR_GENERAL,
						   _("libisofs reported an error while creating directory \"%s\""),
						   graft->path);
			g_free (path_name);
			goto end;

		}

		g_free (path_name);
	}


end:

	if (grafts)
		g_slist_free (grafts);

	if (!priv->error && !priv->cancel) {
		gint64 size;
		RejillaImageFS image_fs;

		image_fs = rejilla_track_data_get_fs (REJILLA_TRACK_DATA (track));

		if ((image_fs & REJILLA_IMAGE_FS_ISO)
		&&  (image_fs & REJILLA_IMAGE_ISO_FS_LEVEL_3))
			iso_write_opts_set_iso_level (opts, 3);
		else
			iso_write_opts_set_iso_level (opts, 2);

		iso_write_opts_set_rockridge (opts, 1);
		iso_write_opts_set_joliet (opts, (image_fs & REJILLA_IMAGE_FS_JOLIET) != 0);
		iso_write_opts_set_allow_deep_paths (opts, (image_fs & REJILLA_IMAGE_ISO_FS_DEEP_DIRECTORY) != 0);

		if (iso_image_create_burn_source (image, opts, &priv->libburn_src) >= 0) {
			size = priv->libburn_src->get_size (priv->libburn_src);
			rejilla_job_set_output_size_for_current_track (REJILLA_JOB (self),
								       REJILLA_BYTES_TO_SECTORS (size, 2048),
								       size);
		}
	}

	if (opts)
		iso_write_opts_free (opts);

	if (image)
		iso_image_unref (image);

	/* End thread */
	g_mutex_lock (priv->mutex);

	/* It is important that the following is done inside the lock; indeed,
	 * if the main loop is idle then that rejilla_libisofs_stop_real () can
	 * be called immediatly to stop the plugin while priv->thread is not
	 * NULL.
	 * As in this callback we check whether the thread is running (which
	 * means that we were cancelled) in some cases it would mean that we
	 * would cancel the libburn_src object and create crippled images. */
	if (!priv->cancel)
		priv->thread_id = g_idle_add (rejilla_libisofs_create_volume_thread_finished, self);

	priv->thread = NULL;
	g_cond_signal (priv->cond);
	g_mutex_unlock (priv->mutex);

	g_thread_exit (NULL);

	return NULL;
}

static RejillaBurnResult
rejilla_libisofs_create_volume (RejillaLibisofs *self, GError **error)
{
	RejillaLibisofsPrivate *priv;
	GError *thread_error = NULL;

	priv = REJILLA_LIBISOFS_PRIVATE (self);
	if (priv->thread)
		return REJILLA_BURN_RUNNING;

	if (iso_init () < 0) {
		g_set_error (error,
			     REJILLA_BURN_ERROR,
			     REJILLA_BURN_ERROR_GENERAL,
			     _("libisofs could not be initialized."));
		return REJILLA_BURN_ERR;
	}

	iso_set_msgs_severities ("NEVER", "ALL", "rejilla (libisofs)");
	g_mutex_lock (priv->mutex);
	priv->thread = g_thread_create (rejilla_libisofs_create_volume_thread,
					self,
					FALSE,
					&thread_error);
	g_mutex_unlock (priv->mutex);

	/* Reminder: this is not necessarily an error as the thread may have finished */
	//if (!priv->thread)
	//	return REJILLA_BURN_ERR;
	if (thread_error) {
		g_propagate_error (error, thread_error);
		return REJILLA_BURN_ERR;
	}

	return REJILLA_BURN_OK;
}

static void
rejilla_libisofs_clean_output (RejillaLibisofs *self)
{
	RejillaLibisofsPrivate *priv;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	if (priv->libburn_src) {
		burn_source_free (priv->libburn_src);
		priv->libburn_src = NULL;
	}

	if (priv->error) {
		g_error_free (priv->error);
		priv->error = NULL;
	}
}

static RejillaBurnResult
rejilla_libisofs_start (RejillaJob *job,
			GError **error)
{
	RejillaLibisofs *self;
	RejillaJobAction action;
	RejillaLibisofsPrivate *priv;

	self = REJILLA_LIBISOFS (job);
	priv = REJILLA_LIBISOFS_PRIVATE (self);

	rejilla_job_get_action (job, &action);
	if (action == REJILLA_JOB_ACTION_SIZE) {
		/* do this to avoid a problem when using
		 * DUMMY flag. libisofs would not generate
		 * a second time. */
		rejilla_libisofs_clean_output (REJILLA_LIBISOFS (job));
		rejilla_job_set_current_action (REJILLA_JOB (self),
						REJILLA_BURN_ACTION_GETTING_SIZE,
						NULL,
						FALSE);
		return rejilla_libisofs_create_volume (self, error);
	}

	if (priv->error) {
		g_error_free (priv->error);
		priv->error = NULL;
	}

	/* we need the source before starting anything */
	if (!priv->libburn_src)
		return rejilla_libisofs_create_volume (self, error);

	return rejilla_libisofs_create_image (self, error);
}

static void
rejilla_libisofs_stop_real (RejillaLibisofs *self)
{
	RejillaLibisofsPrivate *priv;

	priv = REJILLA_LIBISOFS_PRIVATE (self);

	/* Check whether we properly shut down or if we were cancelled */
	g_mutex_lock (priv->mutex);
	if (priv->thread) {
		/* NOTE: this can only happen when we're preparing the volumes
		 * for a multi session disc. At this point we're only running
		 * to get the size of the future volume and we can't race with
		 * libburn plugin that isn't operating at this stage. */
		if (priv->ctx) {
			rejilla_libburn_common_ctx_free (priv->ctx);
			priv->ctx = NULL;
		}

		/* A thread is running. In this context we are probably cancelling */
		if (priv->libburn_src)
			priv->libburn_src->cancel (priv->libburn_src);

		priv->cancel = 1;
		g_cond_wait (priv->cond, priv->mutex);
		priv->cancel = 0;
	}
	g_mutex_unlock (priv->mutex);

	if (priv->thread_id) {
		g_source_remove (priv->thread_id);
		priv->thread_id = 0;
	}
}

static RejillaBurnResult
rejilla_libisofs_stop (RejillaJob *job,
		       GError **error)
{
	RejillaLibisofs *self;

	self = REJILLA_LIBISOFS (job);
	rejilla_libisofs_stop_real (self);
	return REJILLA_BURN_OK;
}

static void
rejilla_libisofs_class_init (RejillaLibisofsClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	RejillaJobClass *job_class = REJILLA_JOB_CLASS (klass);

	g_type_class_add_private (klass, sizeof (RejillaLibisofsPrivate));

	parent_class = g_type_class_peek_parent(klass);
	object_class->finalize = rejilla_libisofs_finalize;

	job_class->start = rejilla_libisofs_start;
	job_class->stop = rejilla_libisofs_stop;
}

static void
rejilla_libisofs_init (RejillaLibisofs *obj)
{
	RejillaLibisofsPrivate *priv;

	priv = REJILLA_LIBISOFS_PRIVATE (obj);
	priv->mutex = g_mutex_new ();
	priv->cond = g_cond_new ();
}

static void
rejilla_libisofs_finalize (GObject *object)
{
	RejillaLibisofs *cobj;
	RejillaLibisofsPrivate *priv;

	cobj = REJILLA_LIBISOFS (object);
	priv = REJILLA_LIBISOFS_PRIVATE (object);

	rejilla_libisofs_stop_real (cobj);
	rejilla_libisofs_clean_output (cobj);

	if (priv->mutex) {
		g_mutex_free (priv->mutex);
		priv->mutex = NULL;
	}

	if (priv->cond) {
		g_cond_free (priv->cond);
		priv->cond = NULL;
	}

	/* close libisofs library */
	iso_finish ();

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
rejilla_libisofs_export_caps (RejillaPlugin *plugin)
{
	GSList *output;
	GSList *input;

	rejilla_plugin_define (plugin,
			       "libisofs",
	                       NULL,
			       _("Creates disc images from a file selection"),
			       "Philippe Rouquier",
			       3);

	rejilla_plugin_set_flags (plugin,
				  REJILLA_MEDIUM_CDR|
				  REJILLA_MEDIUM_CDRW|
				  REJILLA_MEDIUM_DVDR|
				  REJILLA_MEDIUM_DVDRW|
				  REJILLA_MEDIUM_DUAL_L|
				  REJILLA_MEDIUM_APPENDABLE|
				  REJILLA_MEDIUM_HAS_AUDIO|
				  REJILLA_MEDIUM_HAS_DATA,
				  REJILLA_BURN_FLAG_APPEND|
				  REJILLA_BURN_FLAG_MERGE,
				  REJILLA_BURN_FLAG_NONE);

	rejilla_plugin_set_flags (plugin,
				  REJILLA_MEDIUM_DVDRW_PLUS|
				  REJILLA_MEDIUM_RESTRICTED|
				  REJILLA_MEDIUM_DUAL_L|
				  REJILLA_MEDIUM_APPENDABLE|
				  REJILLA_MEDIUM_CLOSED|
				  REJILLA_MEDIUM_HAS_DATA,
				  REJILLA_BURN_FLAG_APPEND|
				  REJILLA_BURN_FLAG_MERGE,
				  REJILLA_BURN_FLAG_NONE);

	output = rejilla_caps_image_new (REJILLA_PLUGIN_IO_ACCEPT_FILE|
					 REJILLA_PLUGIN_IO_ACCEPT_PIPE,
					 REJILLA_IMAGE_FORMAT_BIN);

	input = rejilla_caps_data_new (REJILLA_IMAGE_FS_ISO|
				       REJILLA_IMAGE_ISO_FS_DEEP_DIRECTORY|
				       REJILLA_IMAGE_ISO_FS_LEVEL_3|
				       REJILLA_IMAGE_FS_JOLIET);
	rejilla_plugin_link_caps (plugin, output, input);
	g_slist_free (input);

	input = rejilla_caps_data_new (REJILLA_IMAGE_FS_ISO|
				       REJILLA_IMAGE_ISO_FS_DEEP_DIRECTORY|
				       REJILLA_IMAGE_ISO_FS_LEVEL_3|
				       REJILLA_IMAGE_FS_SYMLINK);
	rejilla_plugin_link_caps (plugin, output, input);
	g_slist_free (input);

	g_slist_free (output);

	rejilla_plugin_register_group (plugin, _(LIBBURNIA_DESCRIPTION));
}
