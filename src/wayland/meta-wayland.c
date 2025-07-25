/*
 * Wayland Support
 *
 * Copyright (C) 2012,2013 Intel Corporation
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

#include "config.h"

#include "wayland/meta-wayland.h"

#include <glib/gstdio.h>

#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <wayland-server.h>

#ifdef HAVE_TIMERFD
# include <sys/timerfd.h>
# include <time.h>
#endif

#include "clutter/clutter.h"
#include "compositor/meta-surface-actor-wayland.h"
#include "core/events.h"
#include "core/meta-context-private.h"
#include "wayland/meta-wayland-activation.h"
#include "wayland/meta-wayland-buffer.h"
#include "wayland/meta-wayland-client-private.h"
#include "wayland/meta-wayland-color-management.h"
#include "wayland/meta-wayland-color-representation.h"
#include "wayland/meta-wayland-commit-timing.h"
#include "wayland/meta-wayland-cursor-shape.h"
#include "wayland/meta-wayland-fifo.h"
#include "wayland/meta-wayland-data-device.h"
#include "wayland/meta-wayland-dma-buf.h"
#include "wayland/meta-wayland-egl-stream.h"
#include "wayland/meta-wayland-filter-manager.h"
#include "wayland/meta-wayland-fixes.h"
#include "wayland/meta-wayland-idle-inhibit.h"
#include "wayland/meta-wayland-inhibit-shortcuts-dialog.h"
#include "wayland/meta-wayland-inhibit-shortcuts.h"
#include "wayland/meta-wayland-legacy-xdg-foreign.h"
#include "wayland/meta-wayland-linux-drm-syncobj.h"
#include "wayland/meta-wayland-outputs.h"
#include "wayland/meta-wayland-presentation-time-private.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-region.h"
#include "wayland/meta-wayland-seat.h"
#include "wayland/meta-wayland-subsurface.h"
#include "wayland/meta-wayland-system-bell.h"
#include "wayland/meta-wayland-tablet-manager.h"
#include "wayland/meta-wayland-transaction.h"
#include "wayland/meta-wayland-xdg-dialog.h"
#include "wayland/meta-wayland-xdg-foreign.h"
#include "wayland/meta-wayland-xdg-session-manager.h"
#include "wayland/meta-wayland-xdg-toplevel-tag.h"

#ifdef HAVE_XWAYLAND
#include "wayland/meta-wayland-x11-interop.h"
#include "wayland/meta-xwayland-grab-keyboard.h"
#include "wayland/meta-xwayland-private.h"
#include "wayland/meta-xwayland.h"
#endif

#ifdef HAVE_NATIVE_BACKEND
#include "backends/native/meta-frame-native.h"
#include "backends/native/meta-renderer-native.h"
#include "wayland/meta-wayland-drm-lease.h"
#endif

enum
{
  PREPARE_SHUTDOWN,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

static char *_display_name_override;

typedef struct _MetaWaylandCompositorPrivate
{
  gboolean is_wayland_egl_display_bound;

  MetaWaylandFilterManager *filter_manager;
  GHashTable *frame_callback_sources;
} MetaWaylandCompositorPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MetaWaylandCompositor, meta_wayland_compositor,
                            G_TYPE_OBJECT)

typedef struct
{
  GSource source;
  struct wl_display *display;
} WaylandEventSource;

typedef struct
{
  GSource source;

  MetaWaylandCompositor *compositor;
  ClutterStageView *stage_view;

#ifdef HAVE_TIMERFD
  int tfd;
  struct itimerspec tfd_spec;
#endif
} FrameCallbackSource;

static void meta_wayland_compositor_update_focus (MetaWaylandCompositor *compositor,
                                                  MetaWindow            *window);

static gboolean
wayland_event_source_prepare (GSource *base,
                              int     *timeout)
{
  WaylandEventSource *source = (WaylandEventSource *)base;

  *timeout = -1;

  wl_display_flush_clients (source->display);

  return FALSE;
}

static gboolean
wayland_event_source_dispatch (GSource    *base,
                               GSourceFunc callback,
                               void       *data)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  struct wl_event_loop *loop = wl_display_get_event_loop (source->display);

  wl_event_loop_dispatch (loop, 0);

  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  NULL,
  wayland_event_source_dispatch,
  NULL
};

static GSource *
wayland_event_source_new (struct wl_display *display)
{
  GSource *source;
  WaylandEventSource *wayland_source;
  struct wl_event_loop *loop = wl_display_get_event_loop (display);

  source = g_source_new (&wayland_event_source_funcs,
                         sizeof (WaylandEventSource));
  g_source_set_name (source, "[mutter] Wayland events");
  wayland_source = (WaylandEventSource *) source;
  wayland_source->display = display;
  g_source_add_unix_fd (&wayland_source->source,
                        wl_event_loop_get_fd (loop),
                        G_IO_IN | G_IO_ERR);

  return &wayland_source->source;
}

static void
emit_frame_callbacks_for_stage_view (MetaWaylandCompositor *compositor,
                                     ClutterStageView      *stage_view)
{
  GList *l;
  int64_t now_us;

  now_us = g_get_monotonic_time ();

  l = compositor->frame_callback_surfaces;
  while (l)
    {
      GList *l_cur = l;
      MetaWaylandSurface *surface = l->data;
      MetaSurfaceActor *actor;
      MetaWaylandActorSurface *actor_surface;

      l = l->next;

      actor = meta_wayland_surface_get_actor (surface);
      if (!actor)
        continue;

      if (!meta_surface_actor_wayland_is_view_primary (actor,
                                                       stage_view))
        continue;

      actor_surface = META_WAYLAND_ACTOR_SURFACE (surface->role);
      meta_wayland_actor_surface_emit_frame_callbacks (actor_surface,
                                                       now_us / 1000);

      compositor->frame_callback_surfaces =
        g_list_delete_link (compositor->frame_callback_surfaces, l_cur);
    }
}

#ifdef HAVE_NATIVE_BACKEND

static gboolean
frame_callback_source_prepare (GSource *base,
                               int     *timeout)
{
  FrameCallbackSource *source = (FrameCallbackSource *)base;

  *timeout = -1;

#ifdef HAVE_TIMERFD
  if (source->tfd > -1)
    {
      int64_t ready_time = g_source_get_ready_time (base);
      struct itimerspec tfd_spec;

      tfd_spec.it_interval.tv_sec = 0;
      tfd_spec.it_interval.tv_nsec = 0;

      if (ready_time > -1)
        {
          tfd_spec.it_value.tv_sec = ready_time / G_USEC_PER_SEC;
          tfd_spec.it_value.tv_nsec = (ready_time % G_USEC_PER_SEC) * 1000L;
        }
      else
        {
          tfd_spec.it_value.tv_sec = 0;
          tfd_spec.it_value.tv_nsec = 0;
        }

      if (memcmp (&tfd_spec, &source->tfd_spec, sizeof tfd_spec) != 0)
        {
          source->tfd_spec = tfd_spec;

          timerfd_settime (source->tfd,
                           TFD_TIMER_ABSTIME,
                           &source->tfd_spec,
                           NULL);
        }
    }
#endif

  return FALSE;
}

static gboolean
frame_callback_source_dispatch (GSource     *source,
                                GSourceFunc  callback,
                                gpointer     user_data)
{
  FrameCallbackSource *frame_callback_source = (FrameCallbackSource *) source;
  MetaWaylandCompositor *compositor = frame_callback_source->compositor;
  ClutterStageView *stage_view = frame_callback_source->stage_view;

  emit_frame_callbacks_for_stage_view (compositor, stage_view);
  g_source_set_ready_time (source, -1);

  return G_SOURCE_CONTINUE;
}

static void
frame_callback_source_finalize (GSource *source)
{
  FrameCallbackSource *frame_callback_source = (FrameCallbackSource *) source;

  g_signal_handlers_disconnect_by_data (frame_callback_source->stage_view,
                                        source);

#ifdef HAVE_TIMERFD
  g_clear_fd (&frame_callback_source->tfd, NULL);
#endif
}

static GSourceFuncs frame_callback_source_funcs = {
  .prepare = frame_callback_source_prepare,
  .dispatch = frame_callback_source_dispatch,
  .finalize = frame_callback_source_finalize,
};

static void
on_stage_view_destroy (ClutterStageView *stage_view,
                       GSource          *source)
{
  FrameCallbackSource *frame_callback_source = (FrameCallbackSource *) source;
  MetaWaylandCompositor *compositor = frame_callback_source->compositor;
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);

  g_hash_table_remove (priv->frame_callback_sources, stage_view);
}

static GSource*
frame_callback_source_new (MetaWaylandCompositor *compositor,
                           ClutterStageView      *stage_view)
{
  FrameCallbackSource *frame_callback_source;
  g_autofree char *name = NULL;
  GSource *source;

  source = g_source_new (&frame_callback_source_funcs,
                         sizeof (FrameCallbackSource));
  frame_callback_source = (FrameCallbackSource *) source;

  name =
    g_strdup_printf ("[mutter] Wayland frame callbacks for stage view (%p)",
                     stage_view);
  g_source_set_name (source, name);
  g_source_set_priority (source, CLUTTER_PRIORITY_REDRAW);
  g_source_set_can_recurse (source, FALSE);

  frame_callback_source->compositor = compositor;
  frame_callback_source->stage_view = stage_view;

  g_signal_connect (stage_view,
                    "destroy",
                    G_CALLBACK (on_stage_view_destroy),
                    source);

#ifdef HAVE_TIMERFD
  frame_callback_source->tfd = timerfd_create (CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  if (frame_callback_source->tfd > -1)
    g_source_add_unix_fd (source, frame_callback_source->tfd, G_IO_IN);
#endif

  return &frame_callback_source->source;
}

static GSource*
ensure_source_for_stage_view (MetaWaylandCompositor *compositor,
                           ClutterStageView      *stage_view)
{
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);
  GSource *source;

  source = g_hash_table_lookup (priv->frame_callback_sources, stage_view);
  if (!source)
    {
      source = frame_callback_source_new (compositor, stage_view);
      g_hash_table_insert (priv->frame_callback_sources, stage_view, source);
      g_source_attach (source, NULL);
      g_source_unref (source);
    }

  return source;
}
#endif /* HAVE_NATIVE_BACKEND */

static void
clear_time_constraints_for_stage_view_transactions (MetaWaylandCompositor *compositor,
                                                    ClutterStageView      *stage_view,
                                                    int64_t                target_time_us)
{
  MetaWaylandTransaction *transaction;

  while ((transaction = g_queue_peek_head (compositor->timed_transactions)))
    {
      if (meta_wayland_transaction_unblock_timed (transaction, target_time_us))
        g_queue_pop_head (compositor->timed_transactions);
      else
        break;
    }
}

static void
clear_barrier_for_stage_view_surfaces (MetaWaylandCompositor *compositor,
                                       ClutterStageView      *stage_view)
{
  GList *l;

  l = compositor->barrier_surfaces;
  while (l)
    {
      GList *l_cur = l;
      MetaWaylandSurface *surface = l->data;
      MetaSurfaceActor *actor;

      l = l->next;

      /* If the surface is not visible, the fifo condition should be
       * unblocked, even if it wasn't on this stage view */
      actor = meta_wayland_surface_get_actor (surface);
      if (actor && !meta_surface_actor_is_effectively_obscured (actor) &&
          clutter_actor_is_mapped (CLUTTER_ACTOR (actor)) &&
          !meta_surface_actor_wayland_is_view_primary (actor, stage_view))
        continue;

      compositor->barrier_surfaces =
        g_list_delete_link (compositor->barrier_surfaces, l_cur);

      meta_wayland_transaction_unblock_surface (surface);
    }
}

static void
on_before_update (ClutterStage          *stage,
                  ClutterStageView      *stage_view,
                  ClutterFrame          *frame,
                  MetaWaylandCompositor *compositor)
{
  int64_t target_time_us;

  if (!clutter_frame_get_target_presentation_time (frame, &target_time_us))
    target_time_us = g_get_monotonic_time ();

  clear_time_constraints_for_stage_view_transactions (compositor, stage_view, target_time_us);
}

static void
on_after_update (ClutterStage          *stage,
                 ClutterStageView      *stage_view,
                 ClutterFrame          *frame,
                 MetaWaylandCompositor *compositor)
{
#if defined(HAVE_NATIVE_BACKEND)
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  GSource *source;
  int64_t frame_deadline_us;

  clear_barrier_for_stage_view_surfaces (compositor, stage_view);

  if (!META_IS_BACKEND_NATIVE (backend))
    {
      emit_frame_callbacks_for_stage_view (compositor, stage_view);
      return;
    }

  source = ensure_source_for_stage_view (compositor, stage_view);

  if (clutter_frame_get_result (frame) ==
      CLUTTER_FRAME_RESULT_PENDING_PRESENTED ||
      !clutter_frame_get_frame_deadline (frame,
                                         &frame_deadline_us))
    {
      g_source_set_ready_time (source, -1);
      emit_frame_callbacks_for_stage_view (compositor, stage_view);
      return;
    }

  if (frame_deadline_us <= g_get_monotonic_time ())
    {
      g_source_set_ready_time (source, -1);
      emit_frame_callbacks_for_stage_view (compositor, stage_view);
      return;
    }

  if (g_source_get_ready_time (source) != -1)
    return;

  g_source_set_ready_time (source, frame_deadline_us);
#else
  clear_barrier_for_stage_view_surfaces (compositor, stage_view);
  emit_frame_callbacks_for_stage_view (compositor, stage_view);
#endif
}

void
meta_wayland_compositor_set_input_focus (MetaWaylandCompositor *compositor,
                                         MetaWindow            *window)
{
  MetaWaylandSurface *surface;

  if (window)
    surface = meta_window_get_wayland_surface (window);
  else
    surface = NULL;
  meta_wayland_seat_set_input_focus (compositor->seat, surface);
}

static void
wl_compositor_create_surface (struct wl_client   *client,
                              struct wl_resource *resource,
                              uint32_t            id)
{
  MetaWaylandCompositor *compositor = wl_resource_get_user_data (resource);

  meta_wayland_surface_create (compositor, client, resource, id);
}

static void
wl_compositor_create_region (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            id)
{
  MetaWaylandCompositor *compositor = wl_resource_get_user_data (resource);

  meta_wayland_region_create (compositor, client, resource, id);
}

static const struct wl_compositor_interface meta_wayland_wl_compositor_interface = {
  wl_compositor_create_surface,
  wl_compositor_create_region
};

static void
compositor_bind (struct wl_client *client,
                 void             *data,
                 uint32_t          version,
                 uint32_t          id)
{
  MetaWaylandCompositor *compositor = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation (resource,
                                  &meta_wayland_wl_compositor_interface,
                                  compositor, NULL);
}

/**
 * meta_wayland_compositor_update:
 * @compositor: the #MetaWaylandCompositor instance
 * @event: the #ClutterEvent used to update @seat's state
 *
 * This is used to update display server state like updating cursor
 * position and keeping track of buttons and keys pressed. It must be
 * called for all input events coming from the underlying devices.
 */
void
meta_wayland_compositor_update (MetaWaylandCompositor *compositor,
                                const ClutterEvent    *event)
{
  meta_wayland_seat_update (compositor->seat, event);
}

static void
on_presented (ClutterStage          *stage,
              ClutterStageView      *stage_view,
              ClutterFrameInfo      *frame_info,
              MetaWaylandCompositor *compositor)
{
  meta_wayland_presentation_time_present_feedbacks (compositor,
                                                    stage_view,
                                                    frame_info);
}

static void
on_started (MetaContext           *context,
            MetaWaylandCompositor *compositor)
{
  MetaDisplay *display = meta_context_get_display (context);

  g_signal_connect_object (display, "focus-window",
                           G_CALLBACK (meta_wayland_compositor_update_focus),
                           compositor,
                           G_CONNECT_SWAPPED);
}

/**
 * meta_wayland_compositor_handle_event:
 * @compositor: the #MetaWaylandCompositor instance
 * @event: the #ClutterEvent to be sent
 *
 * This method sends events to the focused wayland client, if any.
 *
 * Return value: whether @event was sent to a wayland client.
 */
gboolean
meta_wayland_compositor_handle_event (MetaWaylandCompositor *compositor,
                                      const ClutterEvent    *event)
{
  return meta_wayland_seat_handle_event (compositor->seat, event);
}

/* meta_wayland_compositor_update_key_state:
 * @compositor: the #MetaWaylandCompositor
 * @key_vector: bit vector of key states
 * @key_vector_len: length of @key_vector
 * @offset: the key for the first evdev keycode is found at this offset in @key_vector
 *
 * This function is used to resynchronize the key state that Mutter
 * is tracking with the actual keyboard state. This is useful, for example,
 * to handle changes in key state when a nested compositor doesn't
 * have focus. We need to fix up the XKB modifier tracking and deliver
 * any modifier changes to clients.
 */
void
meta_wayland_compositor_update_key_state (MetaWaylandCompositor *compositor,
                                          char                  *key_vector,
                                          int                    key_vector_len,
                                          int                    offset)
{
  meta_wayland_keyboard_update_key_state (compositor->seat->keyboard,
                                          key_vector, key_vector_len, offset);
}

void
meta_wayland_compositor_add_frame_callback_surface (MetaWaylandCompositor *compositor,
                                                    MetaWaylandSurface    *surface)
{
  if (g_list_find (compositor->frame_callback_surfaces, surface))
    return;

  compositor->frame_callback_surfaces =
    g_list_prepend (compositor->frame_callback_surfaces, surface);
}

void
meta_wayland_compositor_remove_frame_callback_surface (MetaWaylandCompositor *compositor,
                                                       MetaWaylandSurface    *surface)
{
  compositor->frame_callback_surfaces =
    g_list_remove (compositor->frame_callback_surfaces, surface);
}

void
meta_wayland_compositor_add_presentation_feedback_surface (MetaWaylandCompositor *compositor,
                                                           MetaWaylandSurface    *surface)
{
  if (g_list_find (compositor->presentation_time.feedback_surfaces, surface))
    return;

  compositor->presentation_time.feedback_surfaces =
    g_list_prepend (compositor->presentation_time.feedback_surfaces, surface);
}

void
meta_wayland_compositor_remove_presentation_feedback_surface (MetaWaylandCompositor *compositor,
                                                              MetaWaylandSurface    *surface)
{
  compositor->presentation_time.feedback_surfaces =
    g_list_remove (compositor->presentation_time.feedback_surfaces, surface);
}

static int
compare_transaction_times (const MetaWaylandTransaction *a,
                           const MetaWaylandTransaction *b,
                           void                         *data)
{
  int64_t target_time_us_a = meta_wayland_transaction_get_target_presentation_time_us (a);
  int64_t target_time_us_b = meta_wayland_transaction_get_target_presentation_time_us (b);

  if (target_time_us_a > target_time_us_b)
    return 1;

  if (target_time_us_a < target_time_us_b)
    return -1;

  return 0;
}

void
meta_wayland_compositor_add_timed_transaction (MetaWaylandCompositor  *compositor,
                                               MetaWaylandTransaction *transaction)
{
  if (g_queue_find (compositor->timed_transactions, transaction))
    return;

  g_queue_insert_sorted (compositor->timed_transactions, transaction,
                         (GCompareDataFunc)compare_transaction_times, NULL);
}

void
meta_wayland_compositor_remove_timed_transaction (MetaWaylandCompositor  *compositor,
                                                  MetaWaylandTransaction *transaction)
{
  g_queue_remove (compositor->timed_transactions, transaction);
}

void
meta_wayland_compositor_add_barrier_surface (MetaWaylandCompositor *compositor,
                                             MetaWaylandSurface    *surface)
{
  if (g_list_find (compositor->barrier_surfaces, surface))
    return;

  compositor->barrier_surfaces =
    g_list_prepend (compositor->barrier_surfaces, surface);
}

void
meta_wayland_compositor_remove_barrier_surface (MetaWaylandCompositor *compositor,
                                                MetaWaylandSurface    *surface)
{
  compositor->barrier_surfaces =
    g_list_remove (compositor->barrier_surfaces, surface);
}

GQueue *
meta_wayland_compositor_get_committed_transactions (MetaWaylandCompositor *compositor)
{
  return &compositor->committed_transactions;
}

static gboolean
update_activation_environment (GDBusConnection *session_bus,
                               const char      *name,
                               const char      *value)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&builder, "{ss}", name, value);

  result = g_dbus_connection_call_sync (session_bus,
                                        "org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "UpdateActivationEnvironment",
                                        g_variant_new ("(@a{ss})",
                                                       g_variant_builder_end (&builder)),
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                        -1, NULL, &error);

  if (error)
    {
      g_warning ("Failed to update activation environment with variable %s: %s",
                 name, error->message);

      return FALSE;
    }
  return TRUE;
}

static gboolean
set_gnome_env (const char *name,
	       const char *value)
{
  GDBusConnection *session_bus;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;

  setenv (name, value, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert (session_bus);

  result = g_dbus_connection_call_sync (session_bus,
			       "org.gnome.SessionManager",
			       "/org/gnome/SessionManager",
			       "org.gnome.SessionManager",
			       "Setenv",
			       g_variant_new ("(ss)", name, value),
			       NULL,
			       G_DBUS_CALL_FLAGS_NO_AUTO_START,
			       -1, NULL, &error);
  if (error)
    {
      g_autofree char *remote_error = NULL;

      remote_error = g_dbus_error_get_remote_error (error);
      if (g_strcmp0 (remote_error, "org.freedesktop.DBus.Error.NameHasNoOwner") == 0)
        {
          return update_activation_environment (session_bus, name, value);
        }
      else if (g_strcmp0 (remote_error, "org.gnome.SessionManager.NotInInitialization") != 0)
        {
          g_warning ("Failed to set environment variable %s for gnome-session: %s",
                     name, error->message);
        }

      return FALSE;
    }
  return TRUE;
}

static void meta_wayland_log_func (const char *, va_list) G_GNUC_PRINTF (1, 0);

static void
meta_wayland_log_func (const char *fmt,
                       va_list     arg)
{
  char *str = g_strdup_vprintf (fmt, arg);
  g_warning ("WL: %s", str);
  g_free (str);
}

static void
on_client_created (struct wl_listener *listener,
                   void               *user_data)
{
  struct wl_client *client = user_data;
  MetaWaylandCompositor *compositor =
    wl_container_of (listener, compositor, client_created_listener);
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  g_autoptr (MetaWaylandClient) wayland_client = NULL;

  wayland_client = meta_wayland_client_new_from_wl (context, client);
}

void
meta_wayland_compositor_prepare_shutdown (MetaWaylandCompositor *compositor)
{
  g_signal_emit (compositor, signals[PREPARE_SHUTDOWN], 0, NULL);

  if (compositor->wayland_display)
    wl_display_destroy_clients (compositor->wayland_display);
}

static void
meta_wayland_compositor_finalize (GObject *object)
{
  MetaWaylandCompositor *compositor = META_WAYLAND_COMPOSITOR (object);
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);
  MetaBackend *backend = meta_context_get_backend (compositor->context);
  ClutterActor *stage = meta_backend_get_stage (backend);

  meta_wayland_xdg_session_management_finalize (compositor);
  meta_wayland_activation_finalize (compositor);
  meta_wayland_outputs_finalize (compositor);
  meta_wayland_presentation_time_finalize (compositor);

  g_hash_table_destroy (compositor->scheduled_surface_associations);

  g_signal_handlers_disconnect_by_func (stage, on_after_update, compositor);
  g_signal_handlers_disconnect_by_func (stage, on_presented, compositor);

  meta_wayland_transaction_finalize (compositor);

  g_clear_object (&compositor->dma_buf_manager);

  g_clear_pointer (&compositor->seat, meta_wayland_seat_free);
  meta_wayland_tablet_manager_finalize (compositor);

  g_clear_pointer (&priv->filter_manager, meta_wayland_filter_manager_free);
  g_clear_pointer (&priv->frame_callback_sources, g_hash_table_destroy);

  g_clear_pointer (&compositor->display_name, g_free);
  g_clear_pointer (&compositor->wayland_display, wl_display_destroy);
  g_clear_pointer (&compositor->source, g_source_destroy);

  g_queue_free (compositor->timed_transactions);

  G_OBJECT_CLASS (meta_wayland_compositor_parent_class)->finalize (object);
}

static void
meta_wayland_compositor_init (MetaWaylandCompositor *compositor)
{
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);

  compositor->scheduled_surface_associations = g_hash_table_new (NULL, NULL);

  wl_log_set_handler_server (meta_wayland_log_func);

  compositor->wayland_display = wl_display_create ();
  if (compositor->wayland_display == NULL)
    g_error ("Failed to create the global wl_display");

  compositor->client_created_listener.notify = on_client_created;
  wl_display_add_client_created_listener (compositor->wayland_display,
                                          &compositor->client_created_listener);

  priv->filter_manager = meta_wayland_filter_manager_new (compositor);
  priv->frame_callback_sources =
    g_hash_table_new_full (NULL, NULL, NULL,
                           (GDestroyNotify) g_source_destroy);
  compositor->timed_transactions = g_queue_new ();
}

static void
meta_wayland_compositor_class_init (MetaWaylandCompositorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_wayland_compositor_finalize;

  signals[PREPARE_SHUTDOWN] =
    g_signal_new ("prepare-shutdown",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

void
meta_wayland_override_display_name (const char *display_name)
{
  g_clear_pointer (&_display_name_override, g_free);
  _display_name_override = g_strdup (display_name);
}

static void
meta_wayland_init_egl (MetaWaylandCompositor *compositor)
{
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaEgl *egl = meta_backend_get_egl (backend);
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  CoglContext *cogl_context =
    clutter_backend_get_cogl_context (clutter_backend);
  EGLDisplay egl_display = cogl_context_get_egl_display (cogl_context);
  g_autoptr (GError) error = NULL;

  if (!meta_egl_has_extensions (egl, egl_display, NULL,
                                "EGL_WL_bind_wayland_display",
                                NULL))
    {
      meta_topic (META_DEBUG_WAYLAND,
                  "Not binding Wayland display, missing extension");
      return;
    }

  meta_topic (META_DEBUG_WAYLAND,
              "Binding Wayland EGL display");

  if (meta_egl_bind_wayland_display (egl,
                                     egl_display,
                                     compositor->wayland_display,
                                     &error))
    priv->is_wayland_egl_display_bound = TRUE;
  else
    g_warning ("Failed to bind Wayland display: %s", error->message);
}

static void
init_dma_buf_support (MetaWaylandCompositor *compositor)
{
  g_autoptr (GError) error = NULL;

  compositor->dma_buf_manager = meta_wayland_dma_buf_manager_new (compositor,
                                                                  &error);
  if (!compositor->dma_buf_manager)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        {
          meta_topic (META_DEBUG_WAYLAND,
                      "Wayland DMA buffer protocol support not enabled: %s",
                      error->message);
        }
      else
        {
          g_warning ("Wayland DMA buffer protocol support not enabled: %s",
                     error->message);
        }
    }
}

MetaWaylandCompositor *
meta_wayland_compositor_new (MetaContext *context)
{
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterActor *stage = meta_backend_get_stage (backend);
  MetaWaylandCompositor *compositor;
  GSource *wayland_event_source;
#ifdef HAVE_XWAYLAND
  MetaX11DisplayPolicy x11_display_policy;
#endif

  compositor = g_object_new (META_TYPE_WAYLAND_COMPOSITOR, NULL);
  compositor->context = context;

  wl_display_set_default_max_buffer_size (compositor->wayland_display,
                                          1024 * 1024);

  wayland_event_source = wayland_event_source_new (compositor->wayland_display);

  /* XXX: Here we are setting the wayland event source to have a
   * slightly lower priority than the X event source, because we are
   * much more likely to get confused being told about surface changes
   * relating to X clients when we don't know what's happened to them
   * according to the X protocol.
   */
  g_source_set_priority (wayland_event_source, META_PRIORITY_EVENTS + 1);
  g_source_attach (wayland_event_source, NULL);
  compositor->source = wayland_event_source;
  g_source_unref (wayland_event_source);

  g_signal_connect (stage, "before-update",
                    G_CALLBACK (on_before_update), compositor);
  g_signal_connect (stage, "after-update",
                    G_CALLBACK (on_after_update), compositor);
  g_signal_connect (stage, "presented",
                    G_CALLBACK (on_presented), compositor);

  g_signal_connect (context, "started",
                    G_CALLBACK (on_started), compositor);

  if (!wl_global_create (compositor->wayland_display,
                         &wl_compositor_interface,
                         META_WL_COMPOSITOR_VERSION,
                         compositor, compositor_bind))
    g_error ("Failed to register the global wl_compositor");

  meta_wayland_init_egl (compositor);
  meta_wayland_init_shm (compositor);
  meta_wayland_outputs_init (compositor);
  meta_wayland_data_device_manager_init (compositor);
  meta_wayland_data_device_primary_manager_init (compositor);
  meta_wayland_subsurfaces_init (compositor);
  meta_wayland_shell_init (compositor);
  meta_wayland_pointer_gestures_init (compositor);
  meta_wayland_tablet_manager_init (compositor);
  meta_wayland_seat_init (compositor);
  meta_wayland_relative_pointer_init (compositor);
  meta_wayland_pointer_constraints_init (compositor);
  meta_wayland_xdg_foreign_init (compositor);
  meta_wayland_legacy_xdg_foreign_init (compositor);
  init_dma_buf_support (compositor);
  meta_wayland_init_single_pixel_buffer_manager (compositor);
  meta_wayland_keyboard_shortcuts_inhibit_init (compositor);
  meta_wayland_surface_inhibit_shortcuts_dialog_init ();
  meta_wayland_text_input_init (compositor);
  meta_wayland_init_presentation_time (compositor);
  meta_wayland_activation_init (compositor);
  meta_wayland_transaction_init (compositor);
  meta_wayland_idle_inhibit_init (compositor);
  meta_wayland_drm_syncobj_init (compositor);
  meta_wayland_init_xdg_wm_dialog (compositor);
  meta_wayland_init_color_management (compositor);
  meta_wayland_xdg_session_management_init (compositor);
  meta_wayland_init_system_bell (compositor);
  meta_wayland_xdg_toplevel_tag_init (compositor);
#ifdef HAVE_NATIVE_BACKEND
  meta_wayland_drm_lease_manager_init (compositor);
#endif
  meta_wayland_commit_timing_init (compositor);
  meta_wayland_fifo_init (compositor);
  meta_wayland_init_cursor_shape (compositor);
  meta_wayland_init_color_representation (compositor);
  meta_wayland_init_fixes (compositor);

#ifdef HAVE_WAYLAND_EGLSTREAM
  {
    gboolean should_enable_eglstream_controller = TRUE;
#if defined(HAVE_EGL_DEVICE) && defined(HAVE_NATIVE_BACKEND)
    MetaRenderer *renderer = meta_backend_get_renderer (backend);

    if (META_IS_RENDERER_NATIVE (renderer))
      {
        MetaRendererNative *renderer_native = META_RENDERER_NATIVE (renderer);

        if (meta_renderer_native_get_mode (renderer_native) ==
            META_RENDERER_NATIVE_MODE_GBM)
          should_enable_eglstream_controller = FALSE;
      }
#endif /* defined(HAVE_EGL_DEVICE) && defined(HAVE_NATIVE_BACKEND) */

    if (should_enable_eglstream_controller)
      meta_wayland_eglstream_controller_init (compositor);
  }
#endif /* HAVE_WAYLAND_EGLSTREAM */

#ifdef HAVE_XWAYLAND
  meta_wayland_x11_interop_init (compositor);

  x11_display_policy =
    meta_context_get_x11_display_policy (compositor->context);
  if (x11_display_policy != META_X11_DISPLAY_POLICY_DISABLED)
    {
      g_autoptr (GError) error = NULL;

      if (!meta_xwayland_init (&compositor->xwayland_manager,
                               compositor,
                               compositor->wayland_display,
                               &error))
        g_error ("Failed to start X Wayland: %s", error->message);
    }
#endif

  if (_display_name_override)
    {
      compositor->display_name = g_steal_pointer (&_display_name_override);

      if (wl_display_add_socket (compositor->wayland_display,
                                 compositor->display_name) != 0)
        g_error ("Failed to create_socket");
    }
  else
    {
      const char *display_name;

      display_name = wl_display_add_socket_auto (compositor->wayland_display);
      if (!display_name)
        g_error ("Failed to create socket");

      compositor->display_name = g_strdup (display_name);
    }

  g_message ("Using Wayland display name '%s'", compositor->display_name);

#ifdef HAVE_XWAYLAND
  if (x11_display_policy != META_X11_DISPLAY_POLICY_DISABLED)
    {
      gboolean status = TRUE;

      status &=
        set_gnome_env ("GNOME_SETUP_DISPLAY", compositor->xwayland_manager.private_connection.name);
      status &=
        set_gnome_env ("DISPLAY", compositor->xwayland_manager.public_connection.name);
      status &=
        set_gnome_env ("XAUTHORITY", compositor->xwayland_manager.auth_file);

      meta_xwayland_set_should_enable_ei_portal (&compositor->xwayland_manager, status);
    }
#endif

  set_gnome_env ("WAYLAND_DISPLAY", meta_wayland_get_wayland_display_name (compositor));

  return compositor;
}

const char *
meta_wayland_get_wayland_display_name (MetaWaylandCompositor *compositor)
{
  return compositor->display_name;
}

#ifdef HAVE_XWAYLAND
const char *
meta_wayland_get_public_xwayland_display_name (MetaWaylandCompositor *compositor)
{
  return compositor->xwayland_manager.public_connection.name;
}

const char *
meta_wayland_get_private_xwayland_display_name (MetaWaylandCompositor *compositor)
{
  return compositor->xwayland_manager.private_connection.name;
}
#endif /* HAVE_XWAYLAND */

void
meta_wayland_compositor_restore_shortcuts (MetaWaylandCompositor *compositor,
                                           ClutterInputDevice    *source)
{
  MetaWaylandSurface *focus;

  /* Clutter is not multi-seat aware yet, use the default seat instead */
  focus = meta_wayland_seat_get_input_focus (compositor->seat);
  if (!focus)
    return;

  if (!meta_wayland_surface_is_shortcuts_inhibited (focus, compositor->seat))
    return;

  meta_wayland_surface_restore_shortcuts (focus, compositor->seat);
}

gboolean
meta_wayland_compositor_is_shortcuts_inhibited (MetaWaylandCompositor *compositor,
                                                ClutterInputDevice    *source)
{
  MetaWaylandSurface *focus;

  /* Clutter is not multi-seat aware yet, use the default seat instead */
  focus = meta_wayland_seat_get_input_focus (compositor->seat);
  if (!focus)
    return FALSE;

  return meta_wayland_surface_is_shortcuts_inhibited (focus, compositor->seat);
}

static void on_scheduled_association_unmanaged (MetaWindow *window,
                                                gpointer    user_data);

static void
meta_wayland_compositor_remove_surface_association (MetaWaylandCompositor *compositor,
                                                    int                    id)
{
  MetaWindow *window;

  window = g_hash_table_lookup (compositor->scheduled_surface_associations,
                                GINT_TO_POINTER (id));
  if (window)
    {
      g_signal_handlers_disconnect_by_func (window,
                                            on_scheduled_association_unmanaged,
                                            GINT_TO_POINTER (id));
      g_hash_table_remove (compositor->scheduled_surface_associations,
                           GINT_TO_POINTER (id));
    }
}

static void
on_scheduled_association_unmanaged (MetaWindow *window,
                                    gpointer    user_data)
{
  MetaDisplay *display = meta_window_get_display (window);
  MetaContext *context = meta_display_get_context (display);
  MetaWaylandCompositor *compositor =
    meta_context_get_wayland_compositor (context);

  meta_wayland_compositor_remove_surface_association (compositor,
                                                      GPOINTER_TO_INT (user_data));
}

void
meta_wayland_compositor_schedule_surface_association (MetaWaylandCompositor *compositor,
                                                      int                    id,
                                                      MetaWindow            *window)
{
  g_signal_connect (window, "unmanaged",
                    G_CALLBACK (on_scheduled_association_unmanaged),
                    GINT_TO_POINTER (id));
  g_hash_table_insert (compositor->scheduled_surface_associations,
                       GINT_TO_POINTER (id), window);
}

#ifdef HAVE_XWAYLAND
void
meta_wayland_compositor_notify_surface_id (MetaWaylandCompositor *compositor,
                                           int                    id,
                                           MetaWaylandSurface    *surface)
{
  MetaWindow *window;

  window = g_hash_table_lookup (compositor->scheduled_surface_associations,
                                GINT_TO_POINTER (id));
  if (window)
    {
#ifdef HAVE_XWAYLAND
      meta_xwayland_associate_window_with_surface (window, surface);
#endif
      meta_wayland_compositor_remove_surface_association (compositor, id);
    }
}
#endif

gboolean
meta_wayland_compositor_is_egl_display_bound (MetaWaylandCompositor *compositor)
{
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);

  return priv->is_wayland_egl_display_bound;
}

#ifdef HAVE_XWAYLAND
MetaXWaylandManager *
meta_wayland_compositor_get_xwayland_manager (MetaWaylandCompositor *compositor)
{
  return &compositor->xwayland_manager;
}
#endif

MetaContext *
meta_wayland_compositor_get_context (MetaWaylandCompositor *compositor)
{
  return compositor->context;
}

/**
 * meta_wayland_compositor_get_wayland_display:
 * @compositor: The #MetaWaylandCompositor
 *
 * Returns: (transfer none): the Wayland display object
 */
struct wl_display *
meta_wayland_compositor_get_wayland_display (MetaWaylandCompositor *compositor)
{
  return compositor->wayland_display;
}

MetaWaylandFilterManager *
meta_wayland_compositor_get_filter_manager (MetaWaylandCompositor *compositor)
{
  MetaWaylandCompositorPrivate *priv =
    meta_wayland_compositor_get_instance_private (compositor);

  return priv->filter_manager;
}

MetaWaylandTextInput *
meta_wayland_compositor_get_text_input (MetaWaylandCompositor *compositor)
{
  return compositor->seat->text_input;
}

static void
meta_wayland_compositor_update_focus (MetaWaylandCompositor *compositor,
                                      MetaWindow            *window)
{
  MetaWindow *focus_window = NULL;

  /* Compositor not ready yet */
  if (!compositor->seat)
    return;

  if (window && meta_window_get_wayland_surface (window))
    focus_window = window;
  else
    meta_topic (META_DEBUG_FOCUS, "Focus change has no effect, because there is no matching wayland surface");

  meta_wayland_compositor_set_input_focus (compositor, focus_window);
}

void
meta_wayland_compositor_sync_focus (MetaWaylandCompositor *compositor)
{
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  MetaDisplay *display = meta_context_get_display (context);

  meta_wayland_compositor_update_focus (compositor,
                                        display ? display->focus_window : NULL);
}
