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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include "rejilla-tool-color-picker.h"

typedef struct _RejillaToolColorPickerPrivate RejillaToolColorPickerPrivate;
struct _RejillaToolColorPickerPrivate
{
	GtkWidget *dialog;

	GtkWidget *label;
	GtkWidget *icon;

	GdkColor color;
};

enum {
	COLOR_SET_SIGNAL,
	LAST_SIGNAL,
};
static guint tool_color_picker_signals[LAST_SIGNAL] = { 0 };


#define REJILLA_TOOL_COLOR_PICKER_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), REJILLA_TYPE_TOOL_COLOR_PICKER, RejillaToolColorPickerPrivate))

G_DEFINE_TYPE (RejillaToolColorPicker, rejilla_tool_color_picker, GTK_TYPE_TOOL_BUTTON);

void
rejilla_tool_color_picker_set_text (RejillaToolColorPicker *self,
				    const gchar *text)
{
	RejillaToolColorPickerPrivate *priv;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);
	gtk_label_set_text_with_mnemonic (GTK_LABEL (priv->label), text);
}

void
rejilla_tool_color_picker_get_color (RejillaToolColorPicker *self,
				     GdkColor *color)
{
	RejillaToolColorPickerPrivate *priv;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);
	color->red = priv->color.red;
	color->green = priv->color.green;
	color->blue = priv->color.blue;
}

void
rejilla_tool_color_picker_set_color (RejillaToolColorPicker *self,
				     GdkColor *color)
{
	RejillaToolColorPickerPrivate *priv;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);
	priv->color.red = color->red;
	priv->color.green = color->green;
	priv->color.blue = color->blue;

	gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
rejilla_tool_color_picker_expose (GtkWidget *widget,
				  GdkEventExpose *event,
				  RejillaToolColorPicker *self)
{
	RejillaToolColorPickerPrivate *priv;
	GtkAllocation allocation;
	cairo_t *ctx;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);

	ctx = gdk_cairo_create (GDK_DRAWABLE (gtk_widget_get_window (widget)));
	gdk_cairo_set_source_color (ctx, &priv->color);
	gtk_widget_get_allocation (widget, &allocation);
	cairo_rectangle (ctx,
			 allocation.x,
			 allocation.y,
			 allocation.width,
			 allocation.height);
	cairo_fill (ctx);
	cairo_stroke (ctx);
	cairo_destroy (ctx);

	return FALSE;
}

static void
rejilla_tool_color_picker_destroy (GtkWidget *widget,
                                   RejillaToolColorPicker *self)
{
	RejillaToolColorPickerPrivate *priv;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);
	priv->dialog = NULL;
}

static void
rejilla_tool_color_picker_response (GtkWidget *widget,
                                    GtkResponseType response,
                                    RejillaToolColorPicker *self)
{
	RejillaToolColorPickerPrivate *priv;
	GtkColorSelection *selection;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);

	if (response == GTK_RESPONSE_OK) {
		selection = GTK_COLOR_SELECTION (gtk_color_selection_dialog_get_color_selection (GTK_COLOR_SELECTION_DIALOG (priv->dialog)));
		gtk_color_selection_get_current_color (selection, &priv->color);

		g_signal_emit (self,
			       tool_color_picker_signals[COLOR_SET_SIGNAL],
			       0);
	}

	gtk_widget_destroy (priv->dialog);
	priv->dialog = NULL;
}

static void
rejilla_tool_color_picker_clicked (RejillaToolColorPicker *self,
				   gpointer NULL_data)
{
	GtkWidget *dialog;
	GtkWidget *toplevel;
	GtkColorSelection *selection;
	RejillaToolColorPickerPrivate *priv;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (self);

	dialog = gtk_color_selection_dialog_new (_("Pick a Color"));
	selection = GTK_COLOR_SELECTION (gtk_color_selection_dialog_get_color_selection (GTK_COLOR_SELECTION_DIALOG (dialog)));
	gtk_color_selection_set_current_color (selection, &priv->color);

	toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
	if (toplevel && GTK_IS_WINDOW (toplevel)) {
		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (toplevel));
		gtk_window_set_modal (GTK_WINDOW (dialog), gtk_window_get_modal (GTK_WINDOW (toplevel)));
	}

	g_signal_connect (GTK_COLOR_SELECTION_DIALOG (dialog),
			  "response",
			  G_CALLBACK (rejilla_tool_color_picker_response),
			  self);
	g_signal_connect (dialog,
			  "destroy",
			  G_CALLBACK (rejilla_tool_color_picker_destroy),
			  self);

	priv->dialog = dialog;
	gtk_widget_show (dialog);
	gtk_window_present (GTK_WINDOW (dialog));
}

static void
rejilla_tool_color_picker_init (RejillaToolColorPicker *object)
{
	RejillaToolColorPickerPrivate *priv;

	priv = REJILLA_TOOL_COLOR_PICKER_PRIVATE (object);

	g_signal_connect (object,
			  "clicked",
			  G_CALLBACK (rejilla_tool_color_picker_clicked),
			  NULL);

	priv->label = gtk_label_new_with_mnemonic ("");
	gtk_widget_show (priv->label);
	gtk_tool_button_set_label_widget (GTK_TOOL_BUTTON (object), priv->label);

	priv->icon = gtk_image_new ();
	gtk_widget_show (priv->icon);
	g_signal_connect (priv->icon,
			  "expose-event",
			  G_CALLBACK (rejilla_tool_color_picker_expose),
			  object);

	/* This function expects a GtkMisc object!! */
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON (object), priv->icon);
}

static void
rejilla_tool_color_picker_finalize (GObject *object)
{
	G_OBJECT_CLASS (rejilla_tool_color_picker_parent_class)->finalize (object);
}

static void
rejilla_tool_color_picker_class_init (RejillaToolColorPickerClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (RejillaToolColorPickerPrivate));

	object_class->finalize = rejilla_tool_color_picker_finalize;

	tool_color_picker_signals[COLOR_SET_SIGNAL] =
		g_signal_new ("color_set",
		              G_OBJECT_CLASS_TYPE (klass),
		              G_SIGNAL_RUN_FIRST,
		              0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0,
		              G_TYPE_NONE);
}

GtkWidget *
rejilla_tool_color_picker_new (void)
{
	return g_object_new (REJILLA_TYPE_TOOL_COLOR_PICKER, NULL);
}
