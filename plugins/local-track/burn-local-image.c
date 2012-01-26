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

/* This is for getline() and isblank() */
#define _GNU_SOURCE

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <gio/gio.h>

#include <gmodule.h>

#include "burn-job.h"
#include "rejilla-plugin-registration.h"
#include "rejilla-xfer.h"
#include "rejilla-track-image.h"


#define REJILLA_TYPE_LOCAL_TRACK         (rejilla_local_track_get_type ())
#define REJILLA_LOCAL_TRACK(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), REJILLA_TYPE_LOCAL_TRACK, RejillaLocalTrack))
#define REJILLA_LOCAL_TRACK_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), REJILLA_TYPE_LOCAL_TRACK, RejillaLocalTrackClass))
#define REJILLA_IS_LOCAL_TRACK(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), REJILLA_TYPE_LOCAL_TRACK))
#define REJILLA_IS_LOCAL_TRACK_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), REJILLA_TYPE_LOCAL_TRACK))
#define REJILLA_LOCAL_TRACK_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), REJILLA_TYPE_LOCAL_TRACK, RejillaLocalTrackClass))

REJILLA_PLUGIN_BOILERPLATE (RejillaLocalTrack, rejilla_local_track, REJILLA_TYPE_JOB, RejillaJob);

struct _RejillaLocalTrackPrivate {
	GCancellable *cancel;
	RejillaXferCtx *xfer_ctx;

	gchar *checksum;
	gchar *checksum_path;
	GChecksumType gchecksum_type;
	RejillaChecksumType checksum_type;

	GHashTable *nonlocals;

	guint thread_id;
	GThread *thread;
	GMutex *mutex;
	GCond *cond;

	GSList *src_list;
	GSList *dest_list;

	GError *error;

	guint download_checksum:1;
};
typedef struct _RejillaLocalTrackPrivate RejillaLocalTrackPrivate;

#define REJILLA_LOCAL_TRACK_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), REJILLA_TYPE_LOCAL_TRACK, RejillaLocalTrackPrivate))

static GObjectClass *parent_class = NULL;

static RejillaBurnResult
rejilla_local_track_clock_tick (RejillaJob *job)
{
	RejillaLocalTrackPrivate *priv;
	goffset written = 0;
	goffset total = 0;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (job);

	if (!priv->xfer_ctx)
		return REJILLA_BURN_OK;

	rejilla_job_start_progress (job, FALSE);

	rejilla_xfer_get_progress (priv->xfer_ctx,
				   &written,
				   &total);
	rejilla_job_set_progress (job, (gdouble) written / (gdouble) total);

	return REJILLA_BURN_OK;
}

/**
 * That's for URI translation ...
 */

static gchar *
rejilla_local_track_translate_uri (RejillaLocalTrack *self,
				   const gchar *uri)
{
	gchar *newuri;
	gchar *parent;
	RejillaLocalTrackPrivate *priv;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	if (!uri)
		return NULL;

	/* see if it is a local file */
	if (g_str_has_prefix (uri, "file://"))
		return g_strdup (uri);

	/* see if it was downloaded itself */
	newuri = g_hash_table_lookup (priv->nonlocals, uri);
	if (newuri) {
		/* we copy this string as it will be freed when freeing 
		 * downloaded GSList */
		return g_strdup (newuri);
	}

	/* see if one of its parent will be downloaded */
	parent = g_path_get_dirname (uri);
	while (parent [1] != '\0') {
		gchar *tmp;

		tmp = g_hash_table_lookup (priv->nonlocals, parent);
		if (tmp) {
			newuri = g_build_path (G_DIR_SEPARATOR_S,
					       tmp,
					       uri + strlen (parent),
					       NULL);
			g_free (parent);
			return newuri;
		}

		tmp = parent;
		parent = g_path_get_dirname (tmp);
		g_free (tmp);
	}

	/* that should not happen */
	REJILLA_JOB_LOG (self, "Can't find a downloaded parent for %s", uri);

	g_free (parent);
	return NULL;
}

static RejillaBurnResult
rejilla_local_track_read_checksum (RejillaLocalTrack *self)
{
	gint size;
	FILE *file;
	gchar *name;
	gchar *source;
	gchar *line = NULL;
	size_t line_len = 0;
	RejillaTrack *track = NULL;
	RejillaLocalTrackPrivate *priv;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	/* get the file_checksum from the checksum file */
	file = fopen (priv->checksum_path, "r");

	/* get the name of the file that was downloaded */
	rejilla_job_get_current_track (REJILLA_JOB (self), &track);
	source = rejilla_track_image_get_source (REJILLA_TRACK_IMAGE (track), TRUE);
	name = g_path_get_basename (source);
	g_free (source);

	if (!file) {
		g_free (name);
		REJILLA_JOB_LOG (self, "Impossible to open the downloaded checksum file");
		return REJILLA_BURN_ERR;
	}

	/* find a line with the name of our file (there could be several ones) */
	REJILLA_JOB_LOG (self, "Searching %s in file", name);
	while ((size = getline (&line, &line_len, file)) > 0) {
		if (strstr (line, name)) {
			gchar *ptr;

			/* Skip blanks */
			ptr = line;
			while (isblank (*ptr)) { ptr ++; size --; }

			if (g_checksum_type_get_length (priv->checksum_type) * 2 > size) {
				g_free (line);
				line = NULL;
				continue;
			}
	
			ptr [g_checksum_type_get_length (priv->gchecksum_type) * 2] = '\0';
			priv->checksum = g_strdup (ptr);
			g_free (line);
			REJILLA_JOB_LOG (self, "Found checksum %s", priv->checksum);
			break;
		}

		g_free (line);
		line = NULL;
	}
	fclose (file);

	return (priv->checksum? REJILLA_BURN_OK:REJILLA_BURN_ERR);
}

static RejillaBurnResult
rejilla_local_track_download_checksum (RejillaLocalTrack *self)
{
	RejillaLocalTrackPrivate *priv;
	RejillaBurnResult result;
	RejillaTrack *track;
	gchar *checksum_src;
	GFile *src, *dest;
	GFile *tmp;
	gchar *uri;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	REJILLA_JOB_LOG (self, "Copying checksum file");

	/* generate a unique name for destination */
	result = rejilla_job_get_tmp_file (REJILLA_JOB (self),
					   NULL,
					   &priv->checksum_path,
					   NULL);
	if (result != REJILLA_BURN_OK)
		goto error;

	rejilla_job_set_current_action (REJILLA_JOB (self),
					REJILLA_BURN_ACTION_FILE_COPY,
					_("Copying checksum file"),
					TRUE);

	/* This is an image. See if there is any checksum sitting in the same
	 * directory to check our download integrity.
	 * Try all types of checksum. */
	rejilla_job_get_current_track (REJILLA_JOB (self), &track);
	uri = rejilla_track_image_get_source (REJILLA_TRACK_IMAGE (track), TRUE);
	dest = g_file_new_for_path (priv->checksum_path);

	/* Try with three difference sources */
	checksum_src = g_strdup_printf ("%s.md5", uri);
	src = g_file_new_for_uri (checksum_src);
	g_free (checksum_src);

	result = rejilla_xfer_start (priv->xfer_ctx,
				     src,
				     dest,
				     priv->cancel,
				     NULL);
	g_object_unref (src);

	if (result == REJILLA_BURN_OK) {
		priv->gchecksum_type = G_CHECKSUM_MD5;
		priv->checksum_type = REJILLA_CHECKSUM_MD5;
		goto end;
	}

	checksum_src = g_strdup_printf ("%s.sha1", uri);
	src = g_file_new_for_uri (checksum_src);
	g_free (checksum_src);

	result = rejilla_xfer_start (priv->xfer_ctx,
				     src,
				     dest,
				     priv->cancel,
				     NULL);
	g_object_unref (src);

	if (result == REJILLA_BURN_OK) {
		priv->gchecksum_type = G_CHECKSUM_SHA1;
		priv->checksum_type = REJILLA_CHECKSUM_SHA1;
		goto end;
	}

	checksum_src = g_strdup_printf ("%s.sha256", uri);
	src = g_file_new_for_uri (checksum_src);
	g_free (checksum_src);

	result = rejilla_xfer_start (priv->xfer_ctx,
				     src,
				     dest,
				     priv->cancel,
				     NULL);
	g_object_unref (src);

	if (result == REJILLA_BURN_OK) {
		priv->gchecksum_type = G_CHECKSUM_SHA256;
		priv->checksum_type = REJILLA_CHECKSUM_SHA256;
		goto end;
	}

	/* There are also ftp sites that includes all images checksum in one big
	 * SHA1 file. */
	tmp = g_file_new_for_uri (uri);
	src = g_file_get_parent (tmp);
	g_object_unref (tmp);

	tmp = src;
	src = g_file_get_child_for_display_name (tmp, "SHA1SUM", NULL);
	g_object_unref (tmp);

	result = rejilla_xfer_start (priv->xfer_ctx,
				     src,
				     dest,
				     priv->cancel,
				     NULL);
	g_object_unref (src);

	if (result != REJILLA_BURN_OK) {
		g_free (uri);
		g_object_unref (dest);
		goto error;
	}

	priv->gchecksum_type = G_CHECKSUM_SHA1;
	priv->checksum_type = REJILLA_CHECKSUM_SHA1;

end:

	g_object_unref (dest);
	g_free (uri);

	return result;

error:

	/* we give up */
	REJILLA_JOB_LOG (self, "No checksum file available");
	g_free (priv->checksum_path);
	priv->checksum_path = NULL;
	return result;
}

static RejillaBurnResult
rejilla_local_track_update_track (RejillaLocalTrack *self)
{
	RejillaTrack *track = NULL;
	RejillaTrack *current = NULL;
	RejillaLocalTrackPrivate *priv;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	/* now we update all the track with the local uris in retval */
	rejilla_job_get_current_track (REJILLA_JOB (self), &current);

	/* make a copy of the tracks instead of modifying them */
	if (REJILLA_IS_TRACK_DATA (current)) {
		GSList *next;
		GSList *grafts;
		GSList *unreadable;
		guint64 file_num = 0;

		track = REJILLA_TRACK (rejilla_track_data_new ());
		rejilla_track_tag_copy_missing (REJILLA_TRACK (track), current);

		rejilla_track_data_add_fs (REJILLA_TRACK_DATA (track), rejilla_track_data_get_fs (REJILLA_TRACK_DATA (current)));

		rejilla_track_data_get_file_num (REJILLA_TRACK_DATA (current), &file_num);
		rejilla_track_data_set_file_num (REJILLA_TRACK_DATA (track), file_num);

		grafts = rejilla_track_data_get_grafts (REJILLA_TRACK_DATA (current));
		for (; grafts; grafts = grafts->next) {
			RejillaGraftPt *graft;
			gchar *uri;

			graft = grafts->data;
			uri = rejilla_local_track_translate_uri (self, graft->uri);
			if (uri) {
				g_free (graft->uri);
				graft->uri = uri;
			}
		}

		REJILLA_JOB_LOG (self, "Translating unreadable");

		/* Translate the globally excluded.
		 * NOTE: if we can't find a parent for an excluded URI that
		 * means it shouldn't be included. */
		unreadable = rejilla_track_data_get_excluded_list (REJILLA_TRACK_DATA (current));
		for (; unreadable; unreadable = next) {
			gchar *new_uri;

			next = unreadable->next;
			new_uri = rejilla_local_track_translate_uri (self, unreadable->data);
			g_free (unreadable->data);

			if (new_uri)
				unreadable->data = new_uri;
			else
				unreadable = g_slist_remove (unreadable, unreadable->data);
		}
	}
	else if (REJILLA_IS_TRACK_STREAM (current)) {
		gchar *uri;
		gchar *newuri;

		uri = rejilla_track_stream_get_source (REJILLA_TRACK_STREAM (current), TRUE);
		newuri = rejilla_local_track_translate_uri (self, uri);

		track = REJILLA_TRACK (rejilla_track_stream_new ());
		rejilla_track_tag_copy_missing (REJILLA_TRACK (track), current);
		rejilla_track_stream_set_source (REJILLA_TRACK_STREAM (track), newuri);
		rejilla_track_stream_set_format (REJILLA_TRACK_STREAM (track), rejilla_track_stream_get_format (REJILLA_TRACK_STREAM (current)));
		rejilla_track_stream_set_boundaries (REJILLA_TRACK_STREAM (track),
						     rejilla_track_stream_get_start (REJILLA_TRACK_STREAM (current)),
						     rejilla_track_stream_get_end (REJILLA_TRACK_STREAM (current)),
						     rejilla_track_stream_get_gap (REJILLA_TRACK_STREAM (current)));
		g_free (uri);
	}
	else if (REJILLA_IS_TRACK_IMAGE (current)) {
		gchar *uri;
		gchar *newtoc;
		gchar *newimage;
		goffset blocks = 0;

		uri = rejilla_track_image_get_source (REJILLA_TRACK_IMAGE (current), TRUE);
		newimage = rejilla_local_track_translate_uri (self, uri);
		g_free (uri);

		uri = rejilla_track_image_get_toc_source (REJILLA_TRACK_IMAGE (current), TRUE);
		newtoc = rejilla_local_track_translate_uri (self, uri);
		g_free (uri);

		rejilla_track_get_size (current, &blocks, NULL);

		track = REJILLA_TRACK (rejilla_track_image_new ());
		rejilla_track_tag_copy_missing (REJILLA_TRACK (track), current);
		rejilla_track_image_set_source (REJILLA_TRACK_IMAGE (track),
						newimage,
						newtoc,
						rejilla_track_image_get_format (REJILLA_TRACK_IMAGE (current)));
		rejilla_track_image_set_block_num (REJILLA_TRACK_IMAGE (track), blocks);
	}
	else
		REJILLA_JOB_NOT_SUPPORTED (self);

	if (priv->download_checksum)
		rejilla_track_set_checksum (track,
					    priv->checksum_type,
					    priv->checksum);

	rejilla_job_add_track (REJILLA_JOB (self), track);

	/* It's good practice to unref the track afterwards as we don't need it
	 * anymore. RejillaTaskCtx refs it. */
	g_object_unref (track);

	return REJILLA_BURN_OK;
}

static gboolean
rejilla_local_track_thread_finished (RejillaLocalTrack *self)
{
	RejillaLocalTrackPrivate *priv;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	priv->thread_id = 0;

	if (priv->xfer_ctx) {
		rejilla_xfer_free (priv->xfer_ctx);
		priv->xfer_ctx = NULL;
	}

	if (priv->cancel) {
		g_object_unref (priv->cancel);
		priv->cancel = NULL;
		if (g_cancellable_is_cancelled (priv->cancel))
			return FALSE;
	}

	if (priv->error) {
		GError *error;

		error = priv->error;
		priv->error = NULL;
		rejilla_job_error (REJILLA_JOB (self), error);
		return FALSE;
	}

	rejilla_local_track_update_track (self);

	rejilla_job_finished_track (REJILLA_JOB (self));
	return FALSE;
}

static gpointer
rejilla_local_track_thread (gpointer data)
{
	RejillaLocalTrack *self = REJILLA_LOCAL_TRACK (data);
	RejillaLocalTrackPrivate *priv;
	GSList *src, *dest;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);
	rejilla_job_set_current_action (REJILLA_JOB (self),
					REJILLA_BURN_ACTION_FILE_COPY,
					_("Copying files locally"),
					TRUE);

	for (src = priv->src_list, dest = priv->dest_list;
	     src && dest;
	     src = src->next, dest = dest->next) {
		gchar *name;
		gchar *string;
		GFile *src_file;
		GFile *dest_file;
		RejillaBurnResult result;

		src_file = src->data;
		dest_file = dest->data;

		name = g_file_get_basename (src_file);
		REJILLA_JOB_LOG (self, "Downloading %s", name);
		string = g_strdup_printf (_("Copying `%s` locally"), name);
		g_free (name);

		result = rejilla_xfer_start (priv->xfer_ctx,
					     src_file,
					     dest_file,
					     priv->cancel,
					     &priv->error);

		if (g_cancellable_is_cancelled (priv->cancel))
			goto end;

		if (result != REJILLA_BURN_OK)
			goto end;
	}

	/* successfully downloaded files, get a checksum if we can. */
	if (priv->download_checksum
	&& !priv->checksum_path
	&&  rejilla_local_track_download_checksum (self) == REJILLA_BURN_OK)
		rejilla_local_track_read_checksum (self);

end:

	if (!g_cancellable_is_cancelled (priv->cancel))
		priv->thread_id = g_idle_add ((GSourceFunc) rejilla_local_track_thread_finished, self);

	/* End thread */
	g_mutex_lock (priv->mutex);
	priv->thread = NULL;
	g_cond_signal (priv->cond);
	g_mutex_unlock (priv->mutex);

	g_thread_exit (NULL);

	return NULL;
}

static RejillaBurnResult
rejilla_local_track_start_thread (RejillaLocalTrack *self,
				  GError **error)
{
	RejillaLocalTrackPrivate *priv;
	GError *thread_error = NULL;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	if (priv->thread)
		return REJILLA_BURN_RUNNING;

	priv->cancel = g_cancellable_new ();
	priv->xfer_ctx = rejilla_xfer_new ();

	g_mutex_lock (priv->mutex);
	priv->thread = g_thread_create (rejilla_local_track_thread,
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
_foreach_non_local_cb (const gchar *uri,
		       const gchar *localuri,
		       gpointer *data)
{
	RejillaLocalTrack *self = REJILLA_LOCAL_TRACK (data);
	RejillaLocalTrackPrivate *priv;
	GFile *file, *tmpfile;
	gchar *parent;
	gchar *tmp;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (data);

	/* check that is hasn't any parent in the hash */
	parent = g_path_get_dirname (uri);
	while (parent [1] != '\0') {
		gchar *uri_local;

		uri_local = g_hash_table_lookup (priv->nonlocals, parent);
		if (uri_local) {
			REJILLA_JOB_LOG (self, "Parent for %s was found %s", uri, parent);
			g_free (parent);
			return TRUE;
		}

		tmp = parent;
		parent = g_path_get_dirname (tmp);
		g_free (tmp);
	}
	g_free (parent);

	file = g_file_new_for_uri (uri);
	priv->src_list = g_slist_append (priv->src_list, file);

	tmpfile = g_file_new_for_uri (localuri);
	priv->dest_list = g_slist_append (priv->dest_list, tmpfile);

	REJILLA_JOB_LOG (self, "%s set to be downloaded to %s", uri, localuri);
	return FALSE;
}

static RejillaBurnResult
rejilla_local_track_add_if_non_local (RejillaLocalTrack *self,
				      const gchar *uri,
				      GError **error)
{
	RejillaLocalTrackPrivate *priv;
	RejillaBurnResult result;
	gchar *localuri = NULL;

	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	if (!uri
	||   uri [0] == '\0'
	||   uri [0] == '/'
	||   g_str_has_prefix (uri, "file://")
	||   g_str_has_prefix (uri, "burn://"))
		return REJILLA_BURN_OK;

	/* add it to the list or uris to download */
	if (!priv->nonlocals)
		priv->nonlocals = g_hash_table_new_full (g_str_hash,
							 g_str_equal,
							 NULL,
							 g_free);

	/* generate a unique name */
	result = rejilla_job_get_tmp_file (REJILLA_JOB (self),
					   NULL,
					   &localuri,
					   error);
	if (result != REJILLA_BURN_OK)
		return result;

	if (!g_str_has_prefix (localuri, "file://")) {
		gchar *tmp;

		tmp = localuri;
		localuri = g_strconcat ("file://", tmp, NULL);
		g_free (tmp);
	}

	/* we don't want to replace it if it has already been downloaded */
	if (!g_hash_table_lookup (priv->nonlocals, uri))
		g_hash_table_insert (priv->nonlocals, g_strdup (uri), localuri);

	return REJILLA_BURN_OK;
}

static RejillaBurnResult
rejilla_local_track_start (RejillaJob *job,
			   GError **error)
{
	RejillaLocalTrackPrivate *priv;
	RejillaBurnResult result;
	RejillaJobAction action;
	RejillaLocalTrack *self;
	RejillaTrack *track;
	GSList *grafts;
	gchar *uri;

	self = REJILLA_LOCAL_TRACK (job);
	priv = REJILLA_LOCAL_TRACK_PRIVATE (self);

	/* skip that part */
	rejilla_job_get_action (job, &action);
	if (action == REJILLA_JOB_ACTION_SIZE) {
		/* say we won't write to disc */
		rejilla_job_set_output_size_for_current_track (job, 0, 0);
		return REJILLA_BURN_NOT_RUNNING;
	}

	if (action != REJILLA_JOB_ACTION_IMAGE)
		return REJILLA_BURN_NOT_SUPPORTED;

	/* can't be piped so rejilla_job_get_current_track will work */
	rejilla_job_get_current_track (job, &track);

	result = REJILLA_BURN_OK;

	/* make a list of all non local uris to be downloaded and put them in a
	 * list to avoid to download the same file twice. */
	if (REJILLA_IS_TRACK_DATA (track)) {
		/* we put all the non local graft point uris in the hash */
		grafts = rejilla_track_data_get_grafts (REJILLA_TRACK_DATA (track));
		for (; grafts; grafts = grafts->next) {
			RejillaGraftPt *graft;

			graft = grafts->data;
			result = rejilla_local_track_add_if_non_local (self, graft->uri, error);
			if (result != REJILLA_BURN_OK)
				break;
		}
	}
	else if (REJILLA_IS_TRACK_STREAM (track)) {
		/* NOTE: don't delete URI as they will be inserted in hash */
		uri = rejilla_track_stream_get_source (REJILLA_TRACK_STREAM (track), TRUE);
		result = rejilla_local_track_add_if_non_local (self, uri, error);
		g_free (uri);
	}
	else if (REJILLA_IS_TRACK_IMAGE (track)) {
		/* NOTE: don't delete URI as they will be inserted in hash */
		uri = rejilla_track_image_get_source (REJILLA_TRACK_IMAGE (track), TRUE);
		result = rejilla_local_track_add_if_non_local (self, uri, error);
		g_free (uri);

		if (result == REJILLA_BURN_OK) {
			priv->download_checksum = TRUE;

			uri = rejilla_track_image_get_toc_source (REJILLA_TRACK_IMAGE (track), TRUE);
			result = rejilla_local_track_add_if_non_local (self, uri, error);
			g_free (uri);
		}
	}
	else
		REJILLA_JOB_NOT_SUPPORTED (self);

	if (result != REJILLA_BURN_OK)
		return result;

	/* see if there is anything to download */
	if (!priv->nonlocals) {
		REJILLA_JOB_LOG (self, "no remote URIs");
		return REJILLA_BURN_NOT_RUNNING;
	}

	/* first we create a list of all the non local files that need to be
	 * downloaded. To be elligible a file must not have one of his parent
	 * in the hash. */
	g_hash_table_foreach_remove (priv->nonlocals,
				     (GHRFunc) _foreach_non_local_cb,
				     job);

	return rejilla_local_track_start_thread (self, error);
}

static RejillaBurnResult
rejilla_local_track_stop (RejillaJob *job,
			  GError **error)
{
	RejillaLocalTrackPrivate *priv = REJILLA_LOCAL_TRACK_PRIVATE (job);

	if (priv->cancel) {
		/* signal that we've been cancelled */
		g_cancellable_cancel (priv->cancel);
	}

	g_mutex_lock (priv->mutex);
	if (priv->thread)
		g_cond_wait (priv->cond, priv->mutex);
	g_mutex_unlock (priv->mutex);

	if (priv->xfer_ctx) {
		rejilla_xfer_free (priv->xfer_ctx);
		priv->xfer_ctx = NULL;
	}

	if (priv->cancel) {
		/* unref it after the thread has stopped */
		g_object_unref (priv->cancel);
		priv->cancel = NULL;
	}

	if (priv->thread_id) {
		g_source_remove (priv->thread_id);
		priv->thread_id = 0;
	}

	if (priv->error) {
		g_error_free (priv->error);
		priv->error = NULL;
	}

	if (priv->src_list) {
		g_slist_foreach (priv->src_list, (GFunc) g_object_unref, NULL);
		g_slist_free (priv->src_list);
		priv->src_list = NULL;
	}

	if (priv->dest_list) {
		g_slist_foreach (priv->dest_list, (GFunc) g_object_unref, NULL);
		g_slist_free (priv->dest_list);
		priv->dest_list = NULL;
	}

	if (priv->nonlocals) {
		g_hash_table_destroy (priv->nonlocals);
		priv->nonlocals = NULL;
	}

	if (priv->checksum_path) {
		g_free (priv->checksum_path);
		priv->checksum_path = NULL;
	}

	if (priv->checksum) {
		g_free (priv->checksum);
		priv->checksum = NULL;
	}

	return REJILLA_BURN_OK;
}

static void
rejilla_local_track_finalize (GObject *object)
{
	RejillaLocalTrackPrivate *priv = REJILLA_LOCAL_TRACK_PRIVATE (object);

	if (priv->mutex) {
		g_mutex_free (priv->mutex);
		priv->mutex = NULL;
	}

	if (priv->cond) {
		g_cond_free (priv->cond);
		priv->cond = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
rejilla_local_track_class_init (RejillaLocalTrackClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	RejillaJobClass *job_class = REJILLA_JOB_CLASS (klass);

	g_type_class_add_private (klass, sizeof (RejillaLocalTrackPrivate));

	parent_class = g_type_class_peek_parent (klass);
	object_class->finalize = rejilla_local_track_finalize;

	job_class->start = rejilla_local_track_start;
	job_class->stop = rejilla_local_track_stop;
	job_class->clock_tick = rejilla_local_track_clock_tick;
}

static void
rejilla_local_track_init (RejillaLocalTrack *obj)
{
	RejillaLocalTrackPrivate *priv = REJILLA_LOCAL_TRACK_PRIVATE (obj);

	priv->mutex = g_mutex_new ();
	priv->cond = g_cond_new ();
}

static void
rejilla_local_track_export_caps (RejillaPlugin *plugin)
{
	GSList *caps;

	rejilla_plugin_define (plugin,
	                       "file-downloader",
			       /* Translators: this is the name of the plugin
				* which will be translated only when it needs
				* displaying. */
			       N_("File Downloader"),
			       _("Allows files not stored locally to be burned"),
			       "Philippe Rouquier",
			       10);

	caps = rejilla_caps_image_new (REJILLA_PLUGIN_IO_ACCEPT_FILE,
				       REJILLA_IMAGE_FORMAT_ANY);
	rejilla_plugin_process_caps (plugin, caps);
	g_slist_free (caps);

	caps = rejilla_caps_audio_new (REJILLA_PLUGIN_IO_ACCEPT_FILE,
				       REJILLA_AUDIO_FORMAT_UNDEFINED|
	                               REJILLA_AUDIO_FORMAT_DTS|
				       REJILLA_AUDIO_FORMAT_RAW|
				       REJILLA_AUDIO_FORMAT_RAW_LITTLE_ENDIAN|
				       REJILLA_VIDEO_FORMAT_UNDEFINED|
				       REJILLA_VIDEO_FORMAT_VCD|
				       REJILLA_VIDEO_FORMAT_VIDEO_DVD|
				       REJILLA_AUDIO_FORMAT_AC3|
				       REJILLA_AUDIO_FORMAT_MP2|
				       REJILLA_METADATA_INFO);
	rejilla_plugin_process_caps (plugin, caps);
	g_slist_free (caps);

	caps = rejilla_caps_audio_new (REJILLA_PLUGIN_IO_ACCEPT_FILE,
				       REJILLA_AUDIO_FORMAT_UNDEFINED|
	                               REJILLA_AUDIO_FORMAT_DTS|
				       REJILLA_AUDIO_FORMAT_RAW|
				       REJILLA_AUDIO_FORMAT_RAW_LITTLE_ENDIAN|
				       REJILLA_VIDEO_FORMAT_UNDEFINED|
				       REJILLA_VIDEO_FORMAT_VCD|
				       REJILLA_VIDEO_FORMAT_VIDEO_DVD|
				       REJILLA_AUDIO_FORMAT_AC3|
				       REJILLA_AUDIO_FORMAT_MP2);
	rejilla_plugin_process_caps (plugin, caps);
	g_slist_free (caps);
	caps = rejilla_caps_data_new (REJILLA_IMAGE_FS_ANY);
	rejilla_plugin_process_caps (plugin, caps);
	g_slist_free (caps);

	rejilla_plugin_set_process_flags (plugin, REJILLA_PLUGIN_RUN_PREPROCESSING);

	rejilla_plugin_set_compulsory (plugin, FALSE);
}
