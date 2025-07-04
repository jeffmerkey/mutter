/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Giovanni Campagna <gcampagn@redhat.com>
 */

#pragma once

#include <glib-object.h>

#include "clutter/clutter.h"
#include "cogl/cogl.h"
#include "meta/types.h"
#include "meta/workspace.h"

#define META_TYPE_CURSOR_TRACKER (meta_cursor_tracker_get_type ())

META_EXPORT
G_DECLARE_DERIVABLE_TYPE (MetaCursorTracker,
                          meta_cursor_tracker,
                          META, CURSOR_TRACKER,
                          GObject)

META_EXPORT
void           meta_cursor_tracker_get_hot    (MetaCursorTracker *tracker,
                                               int               *x,
                                               int               *y);

META_EXPORT
CoglTexture   *meta_cursor_tracker_get_sprite (MetaCursorTracker *tracker);

META_EXPORT
float          meta_cursor_tracker_get_scale (MetaCursorTracker *tracker);

META_EXPORT
void           meta_cursor_tracker_get_pointer (MetaCursorTracker   *tracker,
                                                graphene_point_t    *coords,
                                                ClutterModifierType *mods);

META_EXPORT
gboolean       meta_cursor_tracker_get_pointer_visible (MetaCursorTracker *tracker);

META_EXPORT
void           meta_cursor_tracker_inhibit_cursor_visibility (MetaCursorTracker *tracker);

META_EXPORT
void           meta_cursor_tracker_uninhibit_cursor_visibility (MetaCursorTracker *tracker);
