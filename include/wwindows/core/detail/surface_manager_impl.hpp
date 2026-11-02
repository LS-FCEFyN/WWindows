#pragma once

#include "wwindows/core/surface_manager.hpp"
#include "wwindows/ports/window_backend.hpp"
#include "wwindows/core/detail/surface_state.hpp"
#include "wwindows/core/detail/surface_index.hpp"
#include "wwindows/ports/resize_timer.hpp"

#include <vector>
#include <unordered_map>

#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wayland-server-core.h>

namespace ww {


struct SurfaceManager::Impl {
  explicit Impl(IWindowBackend &backend_ref, IResizeTimer *timer_ref)
      : backend(backend_ref), resize_timer(timer_ref) {}

  IWindowBackend &backend;
  IResizeTimer *resize_timer{nullptr};
  std::unordered_map<wl_resource *, Surface> surfaces;
  SurfaceIndex surface_index;

  // Sends an xdg_toplevel.configure + xdg_surface.configure pair, unless
  // the client already has exactly this one (same size *and* same states).
  // Every configure after set_toplevel's initial one goes through here.
  // Returns whether anything was sent. Implemented in
  // SurfaceManager_toplevel.cpp.
  bool configure_if_changed(Surface &state, uint16_t width,
                            uint16_t height);
  // Resize settling is timer-driven: stale commits, duplicate ConfigureNotify
  // events and Expose events never resend a configure. Timer callbacks look up
  // the surface again before touching its state, so destroyed surfaces are safe.
  void cancel_resize_settle(Surface &state) noexcept;
  void remember_resize_echo(Surface &state, uint16_t width,
                            uint16_t height);
  void schedule_resize_settle(wl_resource *surface, Surface &state);
  void on_resize_settle_timeout(wl_resource *surface,
                                uint64_t target_generation);
  void adopt_committed_geometry(wl_resource *surface, Surface &state,
                                uint16_t width, uint16_t height);
  bool update_committed_geometry(wl_resource *surface, Surface &state,
                                 uint16_t width, uint16_t height,
                                 bool had_previous_commit,
                                 bool was_waiting_for_resize);

  // Shared by set_tiled() and set_tiled_edges(): applies `edges` to
  // `state`'s four tiled_* fields and, if anything actually changed, logs
  // and sends a fresh configure at the client's current size (same gate
  // every other configure goes through). Implemented in
  // SurfaceManager_toplevel.cpp.
  void apply_tiled_edges(Surface &state, uint32_t x11_window,
                         wl_resource *surface, TiledEdges edges);

  // --- destroy callbacks -------------------------------------------------------
  static void on_resource_destroyed(wl_listener *listener, void *);
  static void on_last_buffer_destroyed(wl_listener *listener, void *);
  static void on_toplevel_destroyed(wl_listener *listener, void *);
  static void on_xdg_surface_destroyed(wl_listener *listener, void *);
  void detach_current_buffer(Surface &state);

  // Realizes the X11 child window for a subsurface once its parent has one
  // of its own (immediately in create_subsurface() for the common case, or
  // lazily on first commit if the parent wasn't realized yet). No-op if
  // already realized or the parent still isn't. Implemented in
  // SurfaceManager_subsurface.cpp.
  void ensure_subsurface_window(wl_resource *surface_res, Surface &state);

  // A child X11 window hangs off a parent window whose top-left is the
  // parent's *window-geometry* origin, but wl_subsurface.set_position is
  // expressed in the parent surface's own (buffer) coordinates. The
  // difference is the parent's shadow margin, so subtract it. Returns the
  // position unchanged when the parent declared no geometry.
  //
  // xdg_popup positions deliberately do *not* go through this: an
  // xdg_positioner's anchor rect is already in the parent's window-geometry
  // coordinates, which is exactly what the child window's origin now is.
  void subsurface_origin(const Surface &parent, int32_t x, int32_t y,
                         int16_t &out_x, int16_t &out_y) const;

  // Re-apply every subsurface child's X11 position after the parent's
  // geometry origin moved. Implemented in SurfaceManager_subsurface.cpp.
  void reposition_children(const Surface &parent);

  // Whether `state`'s own wl_surface.commit should cache its pending
  // state (buffer, damage, regions, ...) rather than apply it directly --
  // true if its own Subsurface::sync says so, or any ancestor's does (the
  // wl_subsurface spec's recursive rule: a desynchronized subsurface
  // still behaves synchronized if its parent does). False for anything
  // that isn't a subsurface at all -- a toplevel always applies directly.
  // Depth-capped against a pathological parent cycle a client should
  // never be able to construct but that nothing currently rejects at
  // creation time either. Implemented in SurfaceManager_subsurface.cpp.
  bool is_effectively_synchronized(const Surface &state,
                                   int depth = 0) const;

  // The actual "apply" side of a commit: everything that used to be
  // commit()'s entire body (offset, window geometry, buffer_scale, size
  // limits and first-map, input region, buffer presentation, frame
  // callbacks). Called directly by commit() itself when `state` is not
  // effectively synchronized, and by cascade_to_children() below for a
  // synchronized child whose cached state is being applied now that its
  // parent's was. Implemented in SurfaceManager_buffers.cpp (unchanged
  // from where this logic always lived).
  void apply_surface_state(wl_resource *surface, Surface &state);

  // The apply side of a commit for a wl_pointer.set_cursor surface: flattens
  // the attached wl_shm buffer into a backend cursor and tells the input
  // layer, in place of the window presentation apply_surface_state does for
  // every other role. Implemented in SurfaceManager_buffers.cpp.
  void apply_cursor_surface_state(wl_resource *surface, Surface &state);

  // Builds the cursor for `buffer` (an attached wl_shm buffer) with the
  // surface's current scale and hotspot, replacing state.cursor_handle.
  // Returns whether a new cursor was installed. Implemented in
  // SurfaceManager_buffers.cpp.
  bool rebuild_cursor(Surface &state, wl_resource *buffer);

  // Everything a commit does once the attached buffer has been consumed:
  // release it to the client and make it the surface's last_buffer.
  static void retire_pending_buffer(Surface &state);

  // Called immediately after `parent_state`'s own state is applied
  // (whether via its own direct commit or because cascade_to_children()
  // is itself what just applied it): for every direct subsurface child,
  // applies the child's pending position and pending restack
  // unconditionally (per spec, both are timed to the parent's apply
  // regardless of the child's own sync mode), applies the child's own
  // cached wl_surface state if it has any waiting, and recurses into
  // that child's own children -- a grandchild may have state waiting on
  // *this* child's apply, which just happened, whether or not this
  // child itself had anything pending. Depth-capped like
  // is_effectively_synchronized() above, same reason. Implemented in
  // SurfaceManager_subsurface.cpp.
  void cascade_to_children(Surface &parent_state, int depth = 0);

  // Shared teardown for xdg_popup, used by both client-initiated destroy
  // and compositor-initiated dismissal (click outside the popup chain).
  // Idempotent: safe to call on a surface that isn't (or is no longer) a
  // popup. Implemented in SurfaceManager_popup.cpp.
  static void teardown_popup(Impl &impl, wl_resource *surface_res,
                             Surface &state);
};

} // namespace ww
