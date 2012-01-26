/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Librejilla-misc
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Librejilla-misc is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-misc authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-misc. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-burn is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-misc is distributed in the hope that it will be useful,
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

#ifndef _REJILLA_NOTIFY_H_
#define _REJILLA_NOTIFY_H_

#include <glib-object.h>

#include <gtk/gtk.h>

#include "rejilla-disc-message.h"

G_BEGIN_DECLS

typedef enum {
	REJILLA_NOTIFY_CONTEXT_NONE		= 0,
	REJILLA_NOTIFY_CONTEXT_SIZE		= 1,
	REJILLA_NOTIFY_CONTEXT_LOADING		= 2,
	REJILLA_NOTIFY_CONTEXT_MULTISESSION	= 3,
} RejillaNotifyContextId;

GType rejilla_notify_get_type (void) G_GNUC_CONST;

GtkWidget *rejilla_notify_new (void);

GtkWidget *
rejilla_notify_message_add (GtkWidget *notify,
			    const gchar *primary,
			    const gchar *secondary,
			    gint timeout,
			    guint context_id);

void
rejilla_notify_message_remove (GtkWidget *notify,
			       guint context_id);

GtkWidget *
rejilla_notify_get_message_by_context_id (GtkWidget *notify,
					  guint context_id);

G_END_DECLS

#endif /* _REJILLA_NOTIFY_H_ */
