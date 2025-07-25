/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2005 Elijah Newren
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
 */

#pragma once

/**
 * stack:
 *
 * Which windows cover which other windows
 *
 * There are two factors that determine window position.
 *
 * One is window->stack_position, which is a unique integer
 * indicating how windows are ordered with respect to one
 * another. The ordering here transcends layers; it isn't changed
 * as the window is moved among layers. This allows us to move several
 * windows from one layer to another, while preserving the relative
 * order of the moved windows. Also, it allows us to restore
 * the stacking order from a saved session.
 *
 * However when actually stacking windows on the screen, the
 * layer overrides the stack_position; windows are first sorted
 * by layer, then by stack_position within each layer.
 */

#include "core/display-private.h"

/**
 * A sorted list of windows bearing some level of resemblance to the stack of
 * windows on the X server.
 *
 * (This is only used as a field within a MetaScreen; we treat it as a separate
 * class for simplicity.)
 */
struct _MetaStack
{
  GObject parent;

  /** The MetaDisplay containing this stack. */
  MetaDisplay *display;

  /** The MetaWindows of the windows we manage, sorted in order. */
  GList *sorted;

  /**
   * If this is zero, the local stack oughtn't to be brought up to date with
   * the X server's stack, because it is in the middle of being updated.
   * If it is positive, the local stack is said to be "frozen", and will need
   * to be thawed that many times before the stack can be brought up to date
   * again.  You may freeze the stack with meta_stack_freeze() and thaw it
   * with meta_stack_thaw().
   */
  int freeze_count;

  /**
   * The last-known stack of all windows, bottom to top.  We cache it here
   * so that subsequent times we'll be able to do incremental moves.
   */
  GArray *last_all_root_children_stacked;

  /**
   * Number of stack positions; same as the length of added, but
   * kept for quick reference.
   */
  gint n_positions;

  /** Is the stack in need of re-sorting? */
  unsigned int need_resort : 1;

  /**
   * Are the windows in the stack in need of having their
   * layers recalculated?
   */
  unsigned int need_relayer : 1;

  /**
   * Are the windows in the stack in need of having their positions
   * recalculated with respect to transiency (parent and child windows)?
   */
  unsigned int need_constrain : 1;
};

#define META_TYPE_STACK (meta_stack_get_type ())
G_DECLARE_FINAL_TYPE (MetaStack, meta_stack, META, STACK, GObject)

/**
 * meta_stack_new:
 * @display: The MetaDisplay which will be the parent of this stack.
 *
 * Creates and initialises a MetaStack.
 *
 * Returns: The new stack.
 */
MetaStack * meta_stack_new (MetaDisplay *display);

/**
 * meta_stack_add:
 * @stack: The stack to add it to
 * @window: The window to add
 *
 * Adds a window to the local stack.  It is a fatal error to call this
 * function on a window which already exists on the stack of any screen.
 */
void       meta_stack_add (MetaStack  *stack,
                           MetaWindow *window);

/**
 * meta_stack_remove:
 * @stack: The stack to remove it from
 * @window: The window to remove
 *
 * Removes a window from the local stack.  It is a fatal error to call this
 * function on a window which exists on the stack of any screen.
 */
void       meta_stack_remove (MetaStack  *stack,
                              MetaWindow *window);
/**
 * meta_stack_update_layer:
 * @stack: The stack to recalculate
 *
 * Recalculates the correct layer for all windows in the stack,
 * and moves them about accordingly.
 *
 */
void       meta_stack_update_layer (MetaStack *stack);

/**
 * meta_stack_update_transient:
 * @stack: The stack to recalculate
 *
 * Recalculates the correct stacking order for all windows in the stack
 * according to their transience, and moves them about accordingly.
 *
 */
void       meta_stack_update_transient (MetaStack *stack);

/**
 * meta_stack_raise:
 * @stack: The stack to modify.
 * @window: The window that's making an ascension.
 *              (Amulet of Yendor not required.)
 *
 * Move a window to the top of its layer.
 */
void       meta_stack_raise (MetaStack  *stack,
                             MetaWindow *window);
/**
 * meta_stack_lower:
 * @stack: The stack to modify.
 * @window: The window that's on the way downwards.
 *
 * Move a window to the bottom of its layer.
 */
void       meta_stack_lower (MetaStack  *stack,
                             MetaWindow *window);

/**
 * meta_stack_freeze:
 * @stack: The stack to freeze.
 *
 * Prevent syncing to server until the next call of meta_stack_thaw(),
 * so that we can carry out multiple operations in one go without having
 * everything halfway reflected on the X server.
 *
 * (Calls to meta_stack_freeze() nest, so that multiple calls to
 * meta_stack_freeze will require multiple calls to meta_stack_thaw().)
 */
void       meta_stack_freeze (MetaStack *stack);

/**
 * meta_stack_thaw:
 * @stack: The stack to thaw.
 *
 * Undoes a meta_stack_freeze(), and processes anything which has become
 * necessary during the freeze.  It is an error to call this function if
 * the stack has not been frozen.
 */
void        meta_stack_thaw (MetaStack *stack);

/**
 * meta_stack_get_top:
 * @stack: The stack to examine.
 *
 * Finds the top window on the stack.
 *
 * Returns: The top window on the stack, or %NULL in the vanishingly unlikely
 *          event that you have no windows on your screen whatsoever.
 */
META_EXPORT_TEST
MetaWindow * meta_stack_get_top (MetaStack  *stack);

/**
 * meta_stack_get_above:
 * @stack: The stack to search.
 * @window: The window to look above.
 * @only_within_layer: If %TRUE, will return %NULL if @window is the
 *                     top window in its layer.
 *
 * Finds the window above a given window in the stack.
 * It is not an error to pass in a window which does not exist in
 * the stack; the function will merely return %NULL.
 *
 * Returns: %NULL if there is no such window;
 *          the window above @window otherwise.
 */
MetaWindow * meta_stack_get_above (MetaStack  *stack,
                                   MetaWindow *window,
                                   gboolean    only_within_layer);

/**
 * meta_stack_get_below:
 * @stack: The stack to search.
 * @window: The window to look below.
 * @only_within_layer: If %TRUE, will return %NULL if window is the
 *                     bottom window in its layer.
 *
 * Finds the window below a given window in the stack.
 * It is not an error to pass in a window which does not exist in
 * the stack; the function will merely return %NULL.
 *
 *
 * Returns: %NULL if there is no such window;
 *          the window below @window otherwise.
 */
MetaWindow * meta_stack_get_below (MetaStack  *stack,
                                   MetaWindow *window,
                                   gboolean    only_within_layer);

/**
 * meta_stack_list_windows:
 * @stack: The stack to examine.
 * @workspace: If not %NULL, only windows on this workspace will be
 *             returned; otherwise all windows in the stack will be
 *             returned.
 *
 * Finds all the windows in the stack, in order.
 *
 * Returns: (transfer container) (element-type Meta.Window):
 *     A list of windows, in stacking order, honouring layers.
 */
GList * meta_stack_list_windows (MetaStack     *stack,
                                 MetaWorkspace *workspace);

/**
 * meta_window_set_stack_position:
 * @window: The window which is moving.
 * @position:  Where it should move to (0 is the bottom).
 *
 * Sets the position of a window within the stack.  This will only move it
 * up or down within its layer.  It is an error to attempt to move this
 * below position zero or above the last position in the stack (however, since
 * we don't provide a simple way to tell the number of windows in the stack,
 * this requirement may not be easy to fulfil).
 */
void meta_window_set_stack_position (MetaWindow *window,
                                     int         position);

void meta_stack_update_window_tile_matches (MetaStack     *stack,
                                            MetaWorkspace *workspace);

void meta_stack_ensure_sorted (MetaStack *stack);
