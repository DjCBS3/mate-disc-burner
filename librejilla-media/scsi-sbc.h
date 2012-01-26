/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Librejilla-media
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Librejilla-media is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-media authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-media. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-media is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-media is distributed in the hope that it will be useful,
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

#include <glib.h>

#include "scsi-base.h"
#include "scsi-error.h"
#include "scsi-device.h"

#ifndef _BURN_SBC_H
#define _BURN_SBC_H

G_BEGIN_DECLS

RejillaScsiResult
rejilla_sbc_medium_removal (RejillaDeviceHandle *handle,
                            int prevent_removal,
                            RejillaScsiErrCode *error);

RejillaScsiResult
rejilla_sbc_read10_block (RejillaDeviceHandle *handle,
			  int start,
			  int num_blocks,
			  unsigned char *buffer,
			  int buffer_size,
			  RejillaScsiErrCode *error);

G_END_DECLS

#endif /* _BURN_SBC_H */

 
/***************************************************************************
 *            scsi-sbc.h
 *
 *  Copyright  2008  Rouquier Philippe
 *  <bonfire-app@wanadoo.fr>
 ****************************************************************************/

/*
 * Librejilla-media is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * Librejilla-media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor Boston, MA 02110-1301,  USA
 */

#include <glib.h>

#include "scsi-base.h"
#include "scsi-error.h"

#ifndef _BURN_SBC_H
#define _BURN_SBC_H

G_BEGIN_DECLS

RejillaScsiResult
rejilla_sbc_read10_block (RejillaDeviceHandle *handle,
			  int start,
			  int num_blocks,
			  unsigned char *buffer,
			  int buffer_size,
			  RejillaScsiErrCode *error);

G_END_DECLS

#endif /* _BURN_SBC_H */

 
