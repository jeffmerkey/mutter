/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004, 2005 Elijah Newren
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

/**
 * MetaWorkspace:
 *
 * Workspaces
 *
 * A workspace is a set of windows which all live on the same
 * screen.  (You may also see the name "desktop" around the place,
 * which is the EWMH's name for the same thing.)  Only one workspace
 * of a screen may be active at once; all windows on all other workspaces
 * are unmapped.
 */

#include "config.h"

#include "meta/workspace.h"

#include <string.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-cursor-tracker-private.h"
#include "backends/meta-logical-monitor-private.h"
#include "cogl/cogl.h"
#include "compositor/compositor-private.h"
#include "core/boxes-private.h"
#include "core/meta-workspace-manager-private.h"
#include "core/workspace-private.h"
#include "meta/prefs.h"

void meta_workspace_queue_calc_showing   (MetaWorkspace *workspace);
static void focus_ancestor_or_mru_window (MetaWorkspace *workspace,
                                          MetaWindow    *not_this_one,
                                          guint32        timestamp);
static MetaWindow * get_pointer_window (MetaWorkspace *workspace,
                                        MetaWindow    *not_this_one);

G_DEFINE_TYPE (MetaWorkspace, meta_workspace, G_TYPE_OBJECT);

enum
{
  PROP_0,

  PROP_N_WINDOWS,
  PROP_WORKSPACE_INDEX,
  PROP_ACTIVE,

  PROP_LAST,
};

static GParamSpec *obj_props[PROP_LAST];

enum
{
  WINDOW_ADDED,
  WINDOW_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct _MetaWorkspaceLogicalMonitorData
{
  GList *logical_monitor_region;
  MtkRectangle logical_monitor_work_area;
} MetaWorkspaceLogicalMonitorData;

typedef struct _MetaWorkspaceFocusableAncestorData
{
  MetaWorkspace *workspace;
  MetaWindow *out_window;
} MetaWorkspaceFocusableAncestorData;

static MetaWorkspaceLogicalMonitorData *
meta_workspace_get_logical_monitor_data (MetaWorkspace      *workspace,
                                         MetaLogicalMonitor *logical_monitor)
{
  if (!workspace->logical_monitor_data)
    return NULL;
  return g_hash_table_lookup (workspace->logical_monitor_data, logical_monitor);
}

static void
workspace_logical_monitor_data_free (MetaWorkspaceLogicalMonitorData *data)
{
  g_clear_pointer (&data->logical_monitor_region,
                   meta_rectangle_free_list_and_elements);
  g_free (data);
}

static MetaWorkspaceLogicalMonitorData *
meta_workspace_ensure_logical_monitor_data (MetaWorkspace      *workspace,
                                            MetaLogicalMonitor *logical_monitor)
{
  MetaWorkspaceLogicalMonitorData *data;

  data = meta_workspace_get_logical_monitor_data (workspace, logical_monitor);
  if (data)
    return data;

  if (!workspace->logical_monitor_data)
    {
      workspace->logical_monitor_data =
        g_hash_table_new_full (g_direct_hash,
                               g_direct_equal,
                               NULL,
                               (GDestroyNotify) workspace_logical_monitor_data_free);
    }

  data = g_new0 (MetaWorkspaceLogicalMonitorData, 1);
  g_hash_table_insert (workspace->logical_monitor_data, logical_monitor, data);

  return data;
}

static void
meta_workspace_clear_logical_monitor_data (MetaWorkspace *workspace)
{
  g_clear_pointer (&workspace->logical_monitor_data, g_hash_table_destroy);
}

static void
meta_workspace_get_property (GObject      *object,
                             guint         prop_id,
                             GValue       *value,
                             GParamSpec   *pspec)
{
  MetaWorkspace *ws = META_WORKSPACE (object);

  switch (prop_id)
    {
    case PROP_N_WINDOWS:
      /*
       * This is reliable, but not very efficient; should we store
       * the list length ?
       */
      g_value_set_uint (value, g_list_length (ws->windows));
      break;
    case PROP_WORKSPACE_INDEX:
      g_value_set_uint (value, meta_workspace_index (ws));
      break;
    case PROP_ACTIVE:
      g_value_set_boolean (value, ws->manager->active_workspace == ws);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_workspace_class_init (MetaWorkspaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = meta_workspace_get_property;

  signals[WINDOW_ADDED] = g_signal_new ("window-added",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL, NULL, NULL,
                                        G_TYPE_NONE, 1,
                                        META_TYPE_WINDOW);
  signals[WINDOW_REMOVED] = g_signal_new ("window-removed",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST,
                                          0,
                                          NULL, NULL, NULL,
                                          G_TYPE_NONE, 1,
                                          META_TYPE_WINDOW);

  obj_props[PROP_N_WINDOWS] = g_param_spec_uint ("n-windows", NULL, NULL,
                                                 0, G_MAXUINT, 0,
                                                 G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_WORKSPACE_INDEX] = g_param_spec_uint ("workspace-index", NULL, NULL,
                                                       0, G_MAXUINT, 0,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL,
                                                 FALSE,
                                                 G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, obj_props);
}

static void
meta_workspace_init (MetaWorkspace *workspace)
{
}

MetaWorkspace *
meta_workspace_new (MetaWorkspaceManager *workspace_manager)
{
  MetaDisplay *display = workspace_manager->display;
  MetaWorkspace *workspace;
  GSList *windows, *l;

  workspace = g_object_new (META_TYPE_WORKSPACE, NULL);

  workspace->display = display;
  workspace->manager = workspace_manager;

  workspace_manager->workspaces =
    g_list_append (workspace_manager->workspaces, workspace);
  workspace->windows = NULL;
  workspace->mru_list = NULL;

  workspace->work_areas_invalid = TRUE;
  workspace->work_area_screen.x = 0;
  workspace->work_area_screen.y = 0;
  workspace->work_area_screen.width = 0;
  workspace->work_area_screen.height = 0;

  workspace->screen_region = NULL;
  workspace->screen_edges = NULL;
  workspace->monitor_edges = NULL;
  workspace->list_containing_self = g_list_prepend (NULL, workspace);

  workspace->builtin_struts = NULL;
  workspace->all_struts = NULL;

  workspace->showing_desktop = FALSE;

  /* make sure sticky windows are in our mru_list */
  windows = meta_display_list_windows (display, META_LIST_SORTED);
  for (l = windows; l; l = l->next)
    if (meta_window_located_on_workspace (l->data, workspace))
      meta_workspace_add_window (workspace, l->data);
  g_slist_free (windows);

  return workspace;
}

/**
 * workspace_free_all_struts:
 * @workspace: The workspace.
 *
 * Frees the combined struts list of a workspace.
 */
static void
workspace_free_all_struts (MetaWorkspace *workspace)
{
  if (workspace->all_struts == NULL)
    return;

  g_clear_slist (&workspace->all_struts, g_free);
}

/**
 * workspace_free_builtin_struts:
 * @workspace: The workspace.
 *
 * Frees the struts list set with meta_workspace_set_builtin_struts
 */
static void
workspace_free_builtin_struts (MetaWorkspace *workspace)
{
  if (workspace->builtin_struts == NULL)
    return;

  g_clear_slist (&workspace->all_struts, g_free);
}

/* Ensure that the workspace is empty by making sure that
 * all of our windows are on-all-workspaces. */
static void
assert_workspace_empty (MetaWorkspace *workspace)
{
  GList *l;
  for (l = workspace->windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;
      g_assert (window->on_all_workspaces);
    }
}

void
meta_workspace_remove (MetaWorkspace *workspace)
{
  MetaWorkspaceManager *manager = workspace->display->workspace_manager;

  g_return_if_fail (workspace != manager->active_workspace);

  assert_workspace_empty (workspace);

  manager->workspaces =
    g_list_remove (manager->workspaces, workspace);

  meta_workspace_clear_logical_monitor_data (workspace);

  g_list_free (workspace->mru_list);
  g_list_free (workspace->list_containing_self);

  workspace_free_builtin_struts (workspace);

  /* screen.c:update_num_workspaces(), which calls us, removes windows from
   * workspaces first, which can cause the workareas on the workspace to be
   * invalidated (and hence for struts/regions/edges to be freed).
   * So, no point trying to double free it; that causes a crash
   * anyway.  #361804.
   */

  if (!workspace->work_areas_invalid)
    {
      workspace_free_all_struts (workspace);
      meta_rectangle_free_list_and_elements (workspace->screen_region);
      meta_rectangle_free_list_and_elements (workspace->screen_edges);
      meta_rectangle_free_list_and_elements (workspace->monitor_edges);
    }

  g_object_unref (workspace);

  /* don't bother to reset names, pagers can just ignore
   * extra ones
   */
}

static void
update_workspace_default_focus (MetaWorkspace *workspace,
                                MetaWindow    *not_this_one)
{
  g_autoptr (GSList) windows = NULL;
  GSList *l;

  windows = meta_display_list_windows (workspace->display, META_LIST_DEFAULT);
  for (l = windows; l; l = l->next)
    {
      MetaWindow *window = META_WINDOW (l->data);

      if (meta_window_located_on_workspace (window, workspace) &&
          window != not_this_one)
        meta_window_update_appears_focused (window);
    }
}

void
meta_workspace_add_window (MetaWorkspace *workspace,
                           MetaWindow    *window)
{
  MetaWorkspaceManager *workspace_manager;

  g_return_if_fail (g_list_find (workspace->mru_list, window) == NULL);

  COGL_TRACE_BEGIN_SCOPED (MetaWorkspaceAddWindow,
                           "Meta::Workspace::add_window()");

  workspace_manager = workspace->display->workspace_manager;

  workspace->mru_list = g_list_prepend (workspace->mru_list, window);

  workspace->windows = g_list_prepend (workspace->windows, window);

  if (window->struts)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Invalidating work area of workspace %d since we're adding window %s to it",
                  meta_workspace_index (workspace), window->desc);
      meta_workspace_invalidate_work_area (workspace);
    }

  if (workspace != workspace_manager->active_workspace)
    update_workspace_default_focus (workspace, window);

  g_signal_emit (workspace, signals[WINDOW_ADDED], 0, window);
  g_object_notify_by_pspec (G_OBJECT (workspace), obj_props[PROP_N_WINDOWS]);
}

void
meta_workspace_remove_window (MetaWorkspace *workspace,
                              MetaWindow    *window)
{
  MetaWorkspaceManager *workspace_manager = workspace->display->workspace_manager;

  COGL_TRACE_BEGIN_SCOPED (MetaWorkspaceRemoveWindow,
                           "Meta::Workspace::remove_window()");

  workspace->windows = g_list_remove (workspace->windows, window);

  workspace->mru_list = g_list_remove (workspace->mru_list, window);
  g_assert (g_list_find (workspace->mru_list, window) == NULL);

  if (window->struts)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Invalidating work area of workspace %d since we're removing window %s from it",
                  meta_workspace_index (workspace), window->desc);
      meta_workspace_invalidate_work_area (workspace);
    }

  if (workspace != workspace_manager->active_workspace)
    update_workspace_default_focus (workspace, window);

  g_signal_emit (workspace, signals[WINDOW_REMOVED], 0, window);
  g_object_notify (G_OBJECT (workspace), "n-windows");
}

void
meta_workspace_relocate_windows (MetaWorkspace *workspace,
                                 MetaWorkspace *new_home)
{
  GList *copy, *l;

  g_return_if_fail (workspace != new_home);

  /* can't modify list we're iterating over */
  copy = g_list_copy (workspace->windows);

  for (l = copy; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;

      if (!window->on_all_workspaces)
        meta_window_change_workspace (window, new_home);
    }

  g_list_free (copy);

  assert_workspace_empty (workspace);
}

void
meta_workspace_queue_calc_showing  (MetaWorkspace *workspace)
{
  GList *l;

  for (l = workspace->windows; l != NULL; l = l->next)
    meta_window_queue (l->data, META_QUEUE_CALC_SHOWING);
}

static void
workspace_switch_sound (MetaWorkspace *from,
                        MetaWorkspace *to)
{
  MetaSoundPlayer *player;
  MetaWorkspaceLayout layout;
  int n_workspaces;
  int from_idx, to_idx;
  int i;
  int x, y;
  const char *sound_name;

  n_workspaces = meta_workspace_manager_get_n_workspaces (from->manager);
  from_idx = meta_workspace_index(from);
  to_idx = meta_workspace_index(to);

  meta_workspace_manager_calc_workspace_layout (from->manager,
                                                n_workspaces,
                                                from_idx,
                                                &layout);

  for (i = 0; i < n_workspaces; i++)
    {
      if (layout.grid[i] == to_idx)
        break;
    }

  if (i >= n_workspaces)
    {
      g_warning ("Failed to find destination workspace in layout");
      goto finish;
    }

  y = i / layout.cols;
  x = i % layout.cols;

  /* We priorize horizontal over vertical movements here. The
     rationale for this is that horizontal movements are probably more
     interesting for sound effects because speakers are usually
     positioned on a horizontal and not a vertical axis. i.e. your
     spatial "Woosh!" effects will easily be able to encode horizontal
     movement but not such much vertical movement. */

  if (x < layout.current_col)
    sound_name = "desktop-switch-left";
  else if (x > layout.current_col)
    sound_name = "desktop-switch-right";
  else if (y < layout.current_row)
    sound_name = "desktop-switch-up";
  else if (y > layout.current_row)
    sound_name = "desktop-switch-down";
  else
    {
      g_warn_if_reached ();
      goto finish;
    }

  player = meta_display_get_sound_player (from->display);
  meta_sound_player_play_from_theme (player,
                                     sound_name, _("Workspace switched"),
                                     NULL);

 finish:
  meta_workspace_manager_free_workspace_layout (&layout);
}

static void
workspace_a11y_notification (MetaWorkspace *workspace)
{
  MetaContext *context = meta_display_get_context (workspace->display);
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterActor *stage = meta_backend_get_stage (backend);
  AtkObject *stage_accessible = clutter_actor_get_accessible (stage);
  int index;

  if (!stage_accessible)
    return;

  index = meta_workspace_index (workspace);
  g_signal_emit_by_name (stage_accessible,
                         "notification",
                         meta_prefs_get_workspace_name (index),
                         ATK_LIVE_POLITE,
                         NULL);
}

/**
 * meta_workspace_activate_with_focus:
 * @workspace: a #MetaWorkspace
 * @focus_this: the #MetaWindow to be focused, or %NULL
 * @timestamp: timestamp for @focus_this
 *
 * Switches to @workspace and possibly activates the window @focus_this.
 *
 * The window @focus_this is activated by calling meta_window_activate()
 * which will unminimize it and transient parents, raise it and give it
 * the focus.
 *
 * If a window is currently being moved by the user, it will be
 * moved to @workspace.
 *
 * The advantage of calling this function instead of meta_workspace_activate()
 * followed by meta_window_activate() is that it happens as a unit, so
 * no other window gets focused first before @focus_this.
 */
void
meta_workspace_activate_with_focus (MetaWorkspace *workspace,
                                    MetaWindow    *focus_this,
                                    guint32        timestamp)
{
  MetaWorkspace  *old;
  MetaWindow     *move_window;
  MetaCompositor *comp;
  MetaWorkspaceLayout layout1, layout2;
  gint num_workspaces, current_space, new_space;
  MetaMotionDirection direction;
  MetaWindowDrag *window_drag;

  g_return_if_fail (META_IS_WORKSPACE (workspace));
  g_return_if_fail (meta_workspace_index (workspace) != -1);

  meta_topic (META_DEBUG_WORKSPACES,
              "Activating workspace %d",
              meta_workspace_index (workspace));

  if (workspace->manager->active_workspace == workspace)
    {
      if (focus_this)
        meta_window_activate (focus_this, timestamp);
      return;
    }

  window_drag =
    meta_compositor_get_current_window_drag (workspace->display->compositor);

  /* Free any cached pointers to the workspaces's edges from
   * a current resize or move operation */
  if (window_drag)
    meta_window_drag_update_edges (window_drag);

  if (workspace->manager->active_workspace)
    workspace_switch_sound (workspace->manager->active_workspace, workspace);

  /* Note that old can be NULL; e.g. when starting up */
  old = workspace->manager->active_workspace;

  workspace->manager->active_workspace = workspace;

  g_signal_emit_by_name (workspace->manager, "active-workspace-changed");

  g_object_notify_by_pspec (G_OBJECT (workspace), obj_props[PROP_ACTIVE]);

  workspace_a11y_notification (workspace);

  if (old == NULL)
    return;

  g_object_notify_by_pspec (G_OBJECT (old), obj_props[PROP_ACTIVE]);

  /* If the "show desktop" mode is active for either the old workspace
   * or the new one *but not both*, then update the
   * _net_showing_desktop hint
   */
  if (old->showing_desktop != workspace->showing_desktop)
    g_signal_emit_by_name (workspace->manager, "showing-desktop-changed");

  move_window = NULL;
  if (window_drag &&
      meta_grab_op_is_moving (meta_window_drag_get_grab_op (window_drag)))
    move_window = meta_window_drag_get_window (window_drag);

  if (move_window != NULL)
    {
      /* We put the window on the new workspace, flip spaces,
       * then remove from old workspace, so the window
       * never gets unmapped and we maintain the button grab
       * on it.
       *
       * \bug  This comment appears to be the reverse of what happens
       */
      if (!meta_window_located_on_workspace (move_window, workspace))
        meta_window_change_workspace (move_window, workspace);
    }

  meta_workspace_queue_calc_showing (old);
  meta_workspace_queue_calc_showing (workspace);

   /*
    * Notify the compositor that the active workspace is changing.
    */
   comp = meta_display_get_compositor (workspace->display);
   direction = 0;

   current_space = meta_workspace_index (old);
   new_space     = meta_workspace_index (workspace);
   num_workspaces = meta_workspace_manager_get_n_workspaces (workspace->manager);
   meta_workspace_manager_calc_workspace_layout (workspace->manager, num_workspaces,
                                                 current_space, &layout1);

   meta_workspace_manager_calc_workspace_layout (workspace->manager, num_workspaces,
                                                 new_space, &layout2);

   if (clutter_get_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
     {
       if (layout1.current_col > layout2.current_col)
         direction = META_MOTION_RIGHT;
       else if (layout1.current_col < layout2.current_col)
         direction = META_MOTION_LEFT;
     }
   else
    {
       if (layout1.current_col < layout2.current_col)
         direction = META_MOTION_RIGHT;
       else if (layout1.current_col > layout2.current_col)
         direction = META_MOTION_LEFT;
    }

   if (layout1.current_row < layout2.current_row)
     {
       if (!direction)
         direction = META_MOTION_DOWN;
       else if (direction == META_MOTION_RIGHT)
         direction = META_MOTION_DOWN_RIGHT;
       else
         direction = META_MOTION_DOWN_LEFT;
     }

   if (layout1.current_row > layout2.current_row)
     {
       if (!direction)
         direction = META_MOTION_UP;
       else if (direction == META_MOTION_RIGHT)
         direction = META_MOTION_UP_RIGHT;
       else
         direction = META_MOTION_UP_LEFT;
     }

   meta_workspace_manager_free_workspace_layout (&layout1);
   meta_workspace_manager_free_workspace_layout (&layout2);

   meta_compositor_switch_workspace (comp, old, workspace, direction);

  /* This needs to be done after telling the compositor we are switching
   * workspaces since focusing a window will cause it to be immediately
   * shown and that would confuse the compositor if it didn't know we
   * were in a workspace switch.
   */
  if (focus_this)
    {
      meta_window_activate (focus_this, timestamp);
      update_workspace_default_focus (workspace, focus_this);
    }
  else if (move_window)
    {
      meta_window_raise (move_window);
    }
  else
    {
      meta_topic (META_DEBUG_FOCUS, "Focusing default window on new workspace");
      meta_workspace_focus_default_window (workspace, NULL, timestamp);
    }

   meta_workspace_manager_workspace_switched (workspace->manager, current_space,
                                              new_space, direction);
}

void
meta_workspace_activate (MetaWorkspace *workspace,
                         guint32        timestamp)
{
  meta_workspace_activate_with_focus (workspace, NULL, timestamp);
}

int
meta_workspace_index (MetaWorkspace *workspace)
{
  int ret;

  g_return_val_if_fail (META_IS_WORKSPACE (workspace), -1);

  ret = g_list_index (workspace->manager->workspaces, workspace);

  g_return_val_if_fail (ret >= 0, -1);

  return ret;
}

void
meta_workspace_index_changed (MetaWorkspace *workspace)
{
  GList *l;
  for (l = workspace->windows; l != NULL; l = l->next)
    {
      MetaWindow *win = l->data;
      meta_window_current_workspace_changed (win);
    }

  g_object_notify_by_pspec (G_OBJECT (workspace), obj_props[PROP_WORKSPACE_INDEX]);
}

/**
 * meta_workspace_list_windows:
 * @workspace: a #MetaWorkspace
 *
 * Gets windows contained on the workspace, including workspace->windows
 * and also sticky windows. Override-redirect windows are not included.
 *
 * Return value: (transfer container) (element-type MetaWindow): the list of windows.
 */
GList*
meta_workspace_list_windows (MetaWorkspace *workspace)
{
  GSList *display_windows, *l;
  GList *workspace_windows;

  display_windows = meta_display_list_windows (workspace->display,
                                               META_LIST_DEFAULT);

  workspace_windows = NULL;
  for (l = display_windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;

      if (meta_window_located_on_workspace (window, workspace))
        workspace_windows = g_list_prepend (workspace_windows,
                                            window);
    }

  g_slist_free (display_windows);

  return workspace_windows;
}

void
meta_workspace_invalidate_work_area (MetaWorkspace *workspace)
{
  MetaWindowDrag *window_drag;
  GList *windows, *l;

  if (workspace->work_areas_invalid)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Work area for workspace %d is already invalid",
                  meta_workspace_index (workspace));
      return;
    }

  meta_topic (META_DEBUG_WORKAREA,
              "Invalidating work area for workspace %d",
              meta_workspace_index (workspace));

  window_drag =
    meta_compositor_get_current_window_drag (workspace->display->compositor);

  /* If we are in the middle of a resize or move operation, we
   * might have cached pointers to the workspace's edges */
  if (window_drag &&
      workspace == workspace->manager->active_workspace)
    meta_window_drag_update_edges (window_drag);

  meta_workspace_clear_logical_monitor_data (workspace);

  workspace_free_all_struts (workspace);

  meta_rectangle_free_list_and_elements (workspace->screen_region);
  meta_rectangle_free_list_and_elements (workspace->screen_edges);
  meta_rectangle_free_list_and_elements (workspace->monitor_edges);
  workspace->screen_region = NULL;
  workspace->screen_edges = NULL;
  workspace->monitor_edges = NULL;

  workspace->work_areas_invalid = TRUE;

  /* redo the size/position constraints on all windows */
  windows = meta_workspace_list_windows (workspace);

  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *w = l->data;
      meta_window_queue (w, META_QUEUE_MOVE_RESIZE);
    }

  g_list_free (windows);

  meta_display_queue_workarea_recalc (workspace->display);
}

static MetaStrut *
copy_strut(MetaStrut *original)
{
  return g_memdup2 (original, sizeof (MetaStrut));
}

static GSList *
copy_strut_list(GSList *original)
{
  GSList *result = NULL;

  for (; original != NULL; original = original->next)
    result = g_slist_prepend (result, copy_strut (original->data));

  return g_slist_reverse (result);
}

static void
ensure_work_areas_validated (MetaWorkspace *workspace)
{
  MetaContext *context = meta_display_get_context (workspace->display);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  GList *windows;
  GList *tmp;
  GList *logical_monitors, *l;
  MtkRectangle display_rect = { 0 };
  MtkRectangle work_area;

  if (!workspace->work_areas_invalid)
    return;

  g_assert (workspace->all_struts == NULL);
  g_assert (workspace->screen_region == NULL);
  g_assert (workspace->screen_edges == NULL);
  g_assert (workspace->monitor_edges == NULL);

  meta_display_get_size (workspace->display,
                         &display_rect.width,
                         &display_rect.height);

  /* STEP 1: Get the list of struts */

  workspace->all_struts = copy_strut_list (workspace->builtin_struts);

  windows = meta_workspace_list_windows (workspace);
  for (tmp = windows; tmp != NULL; tmp = tmp->next)
    {
      MetaWindow *win = tmp->data;
      GSList *s_iter;

      for (s_iter = win->struts; s_iter != NULL; s_iter = s_iter->next) {
        workspace->all_struts = g_slist_prepend (workspace->all_struts,
                                                 copy_strut(s_iter->data));
      }
    }
  g_list_free (windows);

  /* STEP 2: Get the maximal/spanning rects for the onscreen and
   *         on-single-monitor regions
   */
  g_assert (workspace->screen_region   == NULL);

  logical_monitors =
    meta_monitor_manager_get_logical_monitors (monitor_manager);
  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;
      MetaWorkspaceLogicalMonitorData *data;

      g_assert (!meta_workspace_get_logical_monitor_data (workspace,
                                                          logical_monitor));

      data = meta_workspace_ensure_logical_monitor_data (workspace,
                                                         logical_monitor);
      data->logical_monitor_region =
        meta_rectangle_get_minimal_spanning_set_for_region (
          &logical_monitor->rect,
          workspace->all_struts);
    }

  workspace->screen_region =
    meta_rectangle_get_minimal_spanning_set_for_region (
      &display_rect,
      workspace->all_struts);

  /* STEP 3: Get the work areas (region-to-maximize-to) for the screen and
   *         monitors.
   */
  work_area = display_rect;  /* start with the screen */
  if (workspace->screen_region == NULL)
    work_area = MTK_RECTANGLE_INIT (0, 0, -1, -1);
  else
    meta_rectangle_clip_to_region (workspace->screen_region,
                                   FIXED_DIRECTION_NONE,
                                   &work_area);

  /* Lots of paranoia checks, forcing work_area_screen to be sane */
#define MIN_SANE_AREA 100
  if (work_area.width < MIN_SANE_AREA &&
      work_area.width != display_rect.width)
    {
      g_warning ("struts occupy an unusually large percentage of the screen; "
                 "available remaining width = %d < %d",
                 work_area.width, MIN_SANE_AREA);
      if (work_area.width < 1)
        {
          work_area.x = (display_rect.width - MIN_SANE_AREA)/2;
          work_area.width = MIN_SANE_AREA;
        }
      else
        {
          int amount = (MIN_SANE_AREA - work_area.width)/2;
          work_area.x     -=   amount;
          work_area.width += 2*amount;
        }
    }
  if (work_area.height < MIN_SANE_AREA &&
      work_area.height != display_rect.height)
    {
      g_warning ("struts occupy an unusually large percentage of the screen; "
                 "available remaining height = %d < %d",
                 work_area.height, MIN_SANE_AREA);
      if (work_area.height < 1)
        {
          work_area.y = (display_rect.height - MIN_SANE_AREA)/2;
          work_area.height = MIN_SANE_AREA;
        }
      else
        {
          int amount = (MIN_SANE_AREA - work_area.height)/2;
          work_area.y      -=   amount;
          work_area.height += 2*amount;
        }
    }
  workspace->work_area_screen = work_area;
  meta_topic (META_DEBUG_WORKAREA,
              "Computed work area for workspace %d: %d,%d %d x %d",
              meta_workspace_index (workspace),
              workspace->work_area_screen.x,
              workspace->work_area_screen.y,
              workspace->work_area_screen.width,
              workspace->work_area_screen.height);

  /* Now find the work areas for each monitor */
  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;
      MetaWorkspaceLogicalMonitorData *data;

      data = meta_workspace_get_logical_monitor_data (workspace,
                                                      logical_monitor);
      work_area = logical_monitor->rect;

      if (!data->logical_monitor_region)
        /* FIXME: constraints.c untested with this, but it might be nice for
         * a screen reader or magnifier.
         */
        work_area = MTK_RECTANGLE_INIT (work_area.x, work_area.y, -1, -1);
      else
        meta_rectangle_clip_to_region (data->logical_monitor_region,
                                       FIXED_DIRECTION_NONE,
                                       &work_area);

      data->logical_monitor_work_area = work_area;

      meta_topic (META_DEBUG_WORKAREA,
                  "Computed work area for workspace %d "
                  "monitor %d: %d,%d %d x %d",
                  meta_workspace_index (workspace),
                  logical_monitor->number,
                  data->logical_monitor_work_area.x,
                  data->logical_monitor_work_area.y,
                  data->logical_monitor_work_area.width,
                  data->logical_monitor_work_area.height);
    }

  /* STEP 4: Make sure the screen_region is nonempty (separate from step 2
   *         since it relies on step 3).
   */
  if (workspace->screen_region == NULL)
    {
      MtkRectangle *nonempty_region;
      nonempty_region = g_new (MtkRectangle, 1);
      *nonempty_region = workspace->work_area_screen;
      workspace->screen_region = g_list_prepend (NULL, nonempty_region);
    }

  /* STEP 5: Cache screen and monitor edges for edge resistance and snapping */
  g_assert (workspace->screen_edges    == NULL);
  g_assert (workspace->monitor_edges  == NULL);
  workspace->screen_edges =
    meta_rectangle_find_onscreen_edges (&display_rect,
                                        workspace->all_struts);
  tmp = NULL;
  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;

      tmp = g_list_prepend (tmp, &logical_monitor->rect);
    }
  workspace->monitor_edges =
    meta_rectangle_find_nonintersected_monitor_edges (tmp,
                                                       workspace->all_struts);
  g_list_free (tmp);

  /* We're all done, YAAY!  Record that everything has been validated. */
  workspace->work_areas_invalid = FALSE;
}

static gboolean
strut_lists_equal (GSList *l,
                   GSList *m)
{
  for (; l && m; l = l->next, m = m->next)
    {
      MetaStrut *a = l->data;
      MetaStrut *b = m->data;

      if (a->side != b->side ||
          !mtk_rectangle_equal (&a->rect, &b->rect))
        return FALSE;
    }

  return l == NULL && m == NULL;
}

/**
 * meta_workspace_set_builtin_struts:
 * @workspace: a #MetaWorkspace
 * @struts: (element-type Meta.Strut) (transfer none): list of #MetaStrut
 *
 * Sets a list of struts that will be used in addition to the struts
 * of the windows in the workspace when computing the work area of
 * the workspace.
 */
void
meta_workspace_set_builtin_struts (MetaWorkspace *workspace,
                                   GSList        *struts)
{
  MetaContext *context = meta_display_get_context (workspace->display);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaDisplay *display = workspace->display;
  MtkRectangle display_rect = { 0 };
  GSList *l;

  meta_display_get_size (display, &display_rect.width, &display_rect.height);

  for (l = struts; l; l = l->next)
    {
      MetaStrut *strut = l->data;
      MetaLogicalMonitor *logical_monitor;

      logical_monitor =
        meta_monitor_manager_get_logical_monitor_from_rect (monitor_manager,
                                                            &strut->rect);

      switch (strut->side)
        {
        case META_SIDE_TOP:
          if (meta_monitor_manager_get_logical_monitor_neighbor (monitor_manager,
                                                                 logical_monitor,
                                                                 META_DISPLAY_UP))
            continue;

          strut->rect.height += strut->rect.y;
          strut->rect.y = 0;
          break;
        case META_SIDE_BOTTOM:
          if (meta_monitor_manager_get_logical_monitor_neighbor (monitor_manager,
                                                                 logical_monitor,
                                                                 META_DISPLAY_DOWN))
            continue;

          strut->rect.height = display_rect.height - strut->rect.y;
          break;
        case META_SIDE_LEFT:
          if (meta_monitor_manager_get_logical_monitor_neighbor (monitor_manager,
                                                                 logical_monitor,
                                                                 META_DISPLAY_LEFT))
            continue;

          strut->rect.width += strut->rect.x;
          strut->rect.x = 0;
          break;
        case META_SIDE_RIGHT:
          if (meta_monitor_manager_get_logical_monitor_neighbor (monitor_manager,
                                                                 logical_monitor,
                                                                 META_DISPLAY_RIGHT))
            continue;

          strut->rect.width = display_rect.width - strut->rect.x;
          break;
        }
    }

  /* Reordering doesn't actually matter, so we don't catch all
   * no-impact changes, but this is just a (possibly unnecessary
   * anyways) optimization */
  if (strut_lists_equal (struts, workspace->builtin_struts))
    return;

  workspace_free_builtin_struts (workspace);
  workspace->builtin_struts = copy_strut_list (struts);

  meta_workspace_invalidate_work_area (workspace);
}

GSList *
meta_workspace_get_builtin_struts (MetaWorkspace *workspace)
{
  return copy_strut_list (workspace->builtin_struts);
}

void
meta_workspace_get_work_area_for_logical_monitor (MetaWorkspace      *workspace,
                                                  MetaLogicalMonitor *logical_monitor,
                                                  MtkRectangle       *area)
{
  meta_workspace_get_work_area_for_monitor (workspace,
                                            logical_monitor->number,
                                            area);
}

/**
 * meta_workspace_get_work_area_for_monitor:
 * @workspace: a #MetaWorkspace
 * @which_monitor: a monitor index
 * @area: (out): location to store the work area
 *
 * Stores the work area for @which_monitor on @workspace
 * in @area.
 */
void
meta_workspace_get_work_area_for_monitor (MetaWorkspace *workspace,
                                          int            which_monitor,
                                          MtkRectangle  *area)
{
  MetaContext *context = meta_display_get_context (workspace->display);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor;
  MetaWorkspaceLogicalMonitorData *data;

  logical_monitor =
    meta_monitor_manager_get_logical_monitor_from_number (monitor_manager,
                                                          which_monitor);
  g_return_if_fail (logical_monitor != NULL);

  ensure_work_areas_validated (workspace);
  data = meta_workspace_get_logical_monitor_data (workspace, logical_monitor);

  g_return_if_fail (data != NULL);

  *area = data->logical_monitor_work_area;
}

/**
 * meta_workspace_get_work_area_all_monitors:
 * @workspace: a #MetaWorkspace
 * @area: (out): location to store the work area
 *
 * Stores the work area in @area.
 */
void
meta_workspace_get_work_area_all_monitors (MetaWorkspace *workspace,
                                           MtkRectangle  *area)
{
  ensure_work_areas_validated (workspace);

  *area = workspace->work_area_screen;
}

GList*
meta_workspace_get_onscreen_region (MetaWorkspace *workspace)
{
  ensure_work_areas_validated (workspace);

  return workspace->screen_region;
}

GList *
meta_workspace_get_onmonitor_region (MetaWorkspace      *workspace,
                                     MetaLogicalMonitor *logical_monitor)
{
  MetaWorkspaceLogicalMonitorData *data;

  ensure_work_areas_validated (workspace);

  data = meta_workspace_get_logical_monitor_data (workspace, logical_monitor);

  return data->logical_monitor_region;
}

#ifdef WITH_VERBOSE_MODE
static const char *
meta_motion_direction_to_string (MetaMotionDirection direction)
{
  switch (direction)
    {
    case META_MOTION_UP:
      return "Up";
    case META_MOTION_DOWN:
      return "Down";
    case META_MOTION_LEFT:
      return "Left";
    case META_MOTION_RIGHT:
      return "Right";
    case META_MOTION_UP_RIGHT:
      return "Up-Right";
    case META_MOTION_DOWN_RIGHT:
      return "Down-Right";
    case META_MOTION_UP_LEFT:
      return "Up-Left";
    case META_MOTION_DOWN_LEFT:
      return "Down-Left";
    }

  return "Unknown";
}
#endif /* WITH_VERBOSE_MODE */

/**
 * meta_workspace_get_neighbor:
 * @workspace: a #MetaWorkspace
 * @direction: a #MetaMotionDirection, relative to @workspace
 *
 * Calculate and retrieve the workspace that is next to @workspace,
 * according to @direction and the current workspace layout, as set
 * by meta_screen_override_workspace_layout().
 *
 * Returns: (transfer none): the workspace next to @workspace, or
 *   @workspace itself if the neighbor would be outside the layout
 */
MetaWorkspace*
meta_workspace_get_neighbor (MetaWorkspace      *workspace,
                             MetaMotionDirection direction)
{
  MetaWorkspaceLayout layout;
  int i, current_space, num_workspaces;
  gboolean ltr;

  current_space = meta_workspace_index (workspace);
  num_workspaces = meta_workspace_manager_get_n_workspaces (workspace->manager);
  meta_workspace_manager_calc_workspace_layout (workspace->manager, num_workspaces,
                                                current_space, &layout);

  meta_topic (META_DEBUG_WORKSPACES,
              "Getting neighbor of %d in direction %s",
              current_space, meta_motion_direction_to_string (direction));

  ltr = (clutter_get_text_direction () == CLUTTER_TEXT_DIRECTION_LTR);

  switch (direction)
    {
    case META_MOTION_LEFT:
      layout.current_col -= ltr ? 1 : -1;
      break;
    case META_MOTION_RIGHT:
      layout.current_col += ltr ? 1 : -1;
      break;
    case META_MOTION_UP:
      layout.current_row -= 1;
      break;
    case META_MOTION_DOWN:
      layout.current_row += 1;
      break;
    default:;
    }

  if (layout.current_col < 0)
    layout.current_col = 0;
  if (layout.current_col >= layout.cols)
    layout.current_col = layout.cols - 1;
  if (layout.current_row < 0)
    layout.current_row = 0;
  if (layout.current_row >= layout.rows)
    layout.current_row = layout.rows - 1;

  i = layout.grid[layout.current_row * layout.cols + layout.current_col];

  if (i < 0)
    i = current_space;

  if (i >= num_workspaces)
    meta_bug ("calc_workspace_layout left an invalid (too-high) workspace number %d in the grid",
              i);

  meta_topic (META_DEBUG_WORKSPACES,
              "Neighbor workspace is %d at row %d col %d",
              i, layout.current_row, layout.current_col);

  meta_workspace_manager_free_workspace_layout (&layout);

  return meta_workspace_manager_get_workspace_by_index (workspace->manager, i);
}

static MetaWindow *
workspace_find_focused_window (MetaWorkspace *workspace)
{
  GList *l;

  for (l = workspace->windows; l != NULL; l = l->next)
    {
      MetaWindow *win = l->data;

      if (meta_window_has_focus (win))
        return win;
    }

  return NULL;
}

void
meta_workspace_focus_default_window (MetaWorkspace *workspace,
                                     MetaWindow    *not_this_one,
                                     guint32        timestamp)
{
  MetaWindow *window = NULL;
  MetaWindow *current_focus_window = NULL;

  if (timestamp == META_CURRENT_TIME)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "META_CURRENT_TIME used to choose focus window; "
                  "focus window may not be correct.");
    }

  current_focus_window = workspace_find_focused_window (workspace);

  if (current_focus_window)
    {
      focus_ancestor_or_mru_window (workspace, not_this_one, timestamp);
      return;
    }

  if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK ||
      !workspace->display->mouse_mode)
    {
      focus_ancestor_or_mru_window (workspace, not_this_one, timestamp);
      return;
    }

  window = get_pointer_window (workspace, not_this_one);

  if (!window ||
      window->type == META_WINDOW_DOCK ||
      window->type == META_WINDOW_DESKTOP)
    {
      if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_MOUSE)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Setting focus to no_focus_window, since no valid "
                      "window to focus found.");
          meta_display_unset_input_focus (workspace->display, timestamp);
        }
      else /* G_DESKTOP_FOCUS_MODE_SLOPPY */
        {
          focus_ancestor_or_mru_window (workspace, not_this_one, timestamp);
        }

      return;
    }

  if (timestamp == META_CURRENT_TIME)
    {
      /* We would like for this to never happen.  However, if
       * it does happen then we kludge since using META_CURRENT_TIME
       * can mean ugly race conditions--and we can avoid these
       * by allowing EnterNotify events (which come with
       * timestamps) to handle focus.
       */

      meta_topic (META_DEBUG_FOCUS,
                  "Not focusing mouse window %s because EnterNotify events should handle that",
                  window->desc);
    }
  else
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Focusing mouse window %s", window->desc);
      meta_window_focus (window, timestamp);
    }

  if (workspace->display->autoraise_window != window &&
      meta_prefs_get_auto_raise ())
    {
      meta_display_queue_autoraise_callback (workspace->display, window);
    }
}

static gboolean
is_focusable (MetaWindow    *window,
              MetaWorkspace *workspace)
{
  return !window->unmanaging &&
         window->unmaps_pending == 0 &&
         window->type != META_WINDOW_DOCK &&
         meta_window_is_focusable (window) &&
         meta_window_should_be_showing_on_workspace (window, workspace);
}

static gboolean
find_focusable_ancestor (MetaWindow *window,
                         gpointer    user_data)
{
  MetaWorkspaceFocusableAncestorData *data = user_data;

  if (is_focusable (window, data->workspace) && !window->hidden)
    {
      data->out_window = window;
      return FALSE;
    }

  return TRUE;
}

GList *
meta_workspace_get_default_focus_candidates (MetaWorkspace *workspace)
{
  GList *l;
  GList *candidates = NULL;

  for (l = workspace->mru_list; l; l = l->next)
    {
      MetaWindow *window = l->data;

      g_assert (window);

      if (!is_focusable (window, workspace))
        continue;

      candidates = g_list_prepend (candidates, window);
    }

  return candidates;
}

static gboolean
window_contains_point (MetaWindow *window,
                       int         root_x,
                       int         root_y)
{
  MtkRectangle rect;

  meta_window_get_frame_rect (window, &rect);

  return mtk_rectangle_contains_point (&rect, root_x, root_y);
}

MetaWindow *
meta_workspace_get_default_focus_window_at_point (MetaWorkspace *workspace,
                                                  MetaWindow    *not_this_one,
                                                  int            root_x,
                                                  int            root_y)
{
  g_autoptr (GList) sorted = NULL;
  GList *l;
  MetaStack *stack;

  g_return_val_if_fail (META_IS_WORKSPACE (workspace), NULL);
  g_return_val_if_fail (!not_this_one || META_IS_WINDOW (not_this_one), NULL);

  stack = workspace->display->stack;

  g_return_val_if_fail (META_IS_STACK (stack), NULL);

  /* Find the topmost, focusable, mapped, window.
   * not_this_one is being unfocused or going away, so exclude it.
   */
  sorted = g_list_reverse (meta_stack_list_windows (stack, workspace));

  /* top of this layer is at the front of the list */
  for (l = sorted; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;

      g_assert (window);

      if (window == not_this_one)
        continue;

      if (!is_focusable (window, workspace))
        continue;

      if (!window_contains_point (window, root_x, root_y))
        continue;

      return window;
    }

  return NULL;
}

MetaWindow *
meta_workspace_get_default_focus_window (MetaWorkspace *workspace,
                                         MetaWindow    *not_this_one)
{
  GList *l;

  g_return_val_if_fail (META_IS_WORKSPACE (workspace), NULL);
  g_return_val_if_fail (!not_this_one || META_IS_WINDOW (not_this_one), NULL);

  for (l = workspace->mru_list; l; l = l->next)
    {
      MetaWindow *window = l->data;

      g_assert (window);

      if (window == not_this_one)
        continue;

      if (!is_focusable (window, workspace))
        continue;

      return window;
    }

  return NULL;
}

static MetaWindow *
get_pointer_window (MetaWorkspace *workspace,
                    MetaWindow    *not_this_one)
{
  MetaContext *context = meta_display_get_context (workspace->display);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);
  graphene_point_t point;

  if (not_this_one)
    meta_topic (META_DEBUG_FOCUS,
                "Focusing mouse window excluding %s", not_this_one->desc);

  meta_cursor_tracker_get_pointer (cursor_tracker, &point, NULL);

  return meta_workspace_get_default_focus_window_at_point (workspace,
                                                           not_this_one,
                                                           (int) point.x,
                                                           (int) point.y);
}

static gboolean
try_to_set_focus_and_check (MetaWindow *window,
                            MetaWindow *not_this_one,
                            uint32_t    timestamp)
{
  meta_window_focus (window, timestamp);

  /* meta_focus_window() will not change focus for clients using the
   * "globally active input" model of input handling, hence defeating
   * the assumption that focus should be changed for such windows.
   * See https://tronche.com/gui/x/icccm/sec-4.html#s-4.1.7
   */
  if (meta_window_is_focus_async (window))
    return TRUE;

  /* meta_window_focus() does not guarantee that focus will end up
   * where we expect, it can fail for various reasons, better check
   * it did not actually changed or even left focus to the window we
   * explicitly want to avoid.
   */
  if (not_this_one &&
      meta_display_get_focus_window (window->display) == not_this_one)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Failed to focus window %s while avoiding %s",
                  window->desc, not_this_one->desc);

      return FALSE;
    }

  return TRUE;
}

/* Focus ancestor of not_this_one if there is one */
static void
focus_ancestor_or_mru_window (MetaWorkspace *workspace,
                              MetaWindow    *not_this_one,
                              guint32        timestamp)
{
  MetaWindow *window = NULL;

  if (not_this_one)
    meta_topic (META_DEBUG_FOCUS,
                "Focusing MRU window excluding %s", not_this_one->desc);
  else
    meta_topic (META_DEBUG_FOCUS,
                "Focusing MRU window");

  /* First, check to see if we need to focus an ancestor of a window */
  if (not_this_one)
    {
      MetaWindow *ancestor;
      MetaWorkspaceFocusableAncestorData data;

      data = (MetaWorkspaceFocusableAncestorData) {
        .workspace = workspace,
      };
      meta_window_foreach_ancestor (not_this_one, find_focusable_ancestor, &data);
      ancestor = data.out_window;

      if (ancestor)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Focusing %s, ancestor of %s",
                      ancestor->desc, not_this_one->desc);

          if (try_to_set_focus_and_check (ancestor, not_this_one, timestamp))
            {
              /* Also raise the window if in click-to-focus */
              if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK &&
                  meta_prefs_get_raise_on_click ())
                meta_window_raise (ancestor);

              return;
            }
        }
    }

  window = meta_workspace_get_default_focus_window (workspace, not_this_one);

  if (window)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Focusing workspace MRU window %s", window->desc);
      if (try_to_set_focus_and_check (window, not_this_one, timestamp))
        {
          /* Also raise the window if in click-to-focus */
          if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK &&
              meta_prefs_get_raise_on_click ())
            meta_window_raise (window);

          return;
        }
    }

  meta_topic (META_DEBUG_FOCUS,
             "No MRU window to focus found; focusing no_focus_window.");
  meta_display_unset_input_focus (workspace->display, timestamp);
}

/**
 * meta_workspace_get_display:
 * @workspace: a #MetaWorkspace
 *
 * Gets the #MetaDisplay that the workspace is part of.
 *
 * Return value: (transfer none): the #MetaDisplay for the workspace
 */
MetaDisplay *
meta_workspace_get_display (MetaWorkspace *workspace)
{
  return workspace->display;
}
