#pragma once

#include "wwindows/ports/surface_interactions.hpp"
#include "wwindows/ports/window_backend.hpp"
#include "wwindows/ports/window_manager_watcher.hpp" // TiledEdges
#include "wwindows/ports/resize_timer.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <wayland-server-core.h>

struct wl_resource;

namespace ww {

class SurfaceManager final : public ISurfaceInteractions {
public:
  explicit SurfaceManager(IWindowBackend &backend, IResizeTimer *resize_timer = nullptr);
  ~SurfaceManager();

  SurfaceManager(const SurfaceManager &) = delete;
  SurfaceManager &operator=(const SurfaceManager &) = delete;

  // Surface lifetime
  void create_surface(wl_resource *resource);
  void destroy_surface(wl_resource *resource);

  // XDG-shell
  void request_resize(uint32_t x11_window, uint16_t width, uint16_t height);
  void set_toplevel(wl_resource *surface_res, wl_resource *toplevel_res,
                    wl_resource *xdg_surface_res);
  void set_title(wl_resource *surface_res, const char *title);

  // xdg_toplevel.set_app_id: the application's identity, which becomes the
  // window's WM_CLASS (instance and class both) so window-manager rules
  // ([class="foot"], assign, for_window) can match on it.
  void set_app_id(wl_resource *surface_res, const char *app_id);

  // xdg_toplevel.set_parent. `parent_surface_res` is the parent toplevel's
  // wl_surface, or null to make the toplevel independent again. Becomes
  // WM_TRANSIENT_FOR, which is how a window manager knows to treat the
  // window as a dialog of another one.
  void set_toplevel_parent(wl_resource *surface_res,
                           wl_resource *parent_surface_res);

  // xdg_toplevel.set_min_size / set_max_size. Double-buffered like the
  // protocol says: recorded now, applied to the window at the next commit.
  void set_min_size(wl_resource *surface_res, int32_t width, int32_t height);
  void set_max_size(wl_resource *surface_res, int32_t width, int32_t height);

  // xdg_wm_base.get_xdg_surface: whether this wl_surface has already had an
  // xdg_surface created for it. The xdg-shell protocol makes creating a
  // second one -- or giving a role to a surface that already has an
  // unrelated one (a cursor, say) -- a protocol error
  // (xdg_wm_base.error.role); this is what the wire handler checks before
  // creating the resource, and what it marks true on success. Once marked,
  // stays true for the surface's lifetime -- there is no way to un-become
  // an xdg_surface any more than there is to un-become a toplevel.
  [[nodiscard]] bool has_xdg_surface_role(wl_resource *surface_res) const;
  void mark_xdg_surface_role(wl_resource *surface_res);

  // xdg_surface.set_window_geometry — the sub-rectangle of the surface's
  // buffer that is the window proper, everything outside it being the
  // client's own shadow and invisible resize border. See
  // Surface::geometry for why this matters and what goes wrong
  // without it.
  void set_window_geometry(wl_resource *surface_res, int32_t x, int32_t y,
                           int32_t width, int32_t height);

  // xdg_toplevel.set_fullscreen / set_maximized (and their unset_
  // counterparts). Forwarded to the window manager, which owns top-level
  // geometry here; the resulting size comes back later as an ordinary
  // resize. Fullscreen is the only way a client escapes the work area the
  // WM hands out — screen minus panels and bars.
  void set_toplevel_state(wl_resource *surface_res, WindowState state,
                          bool enabled);

  // xdg_toplevel.move / xdg_toplevel.resize / xdg_toplevel.set_minimized:
  // forwarded to the window manager the same way set_toplevel_state is,
  // and for the same reason (WWindows never owns toplevel geometry).
  // `edges` is xdg_toplevel_resize_edge's raw wire value.
  void move_toplevel(wl_resource *surface_res);
  void resize_toplevel(wl_resource *surface_res, uint32_t edges);
  void minimize_toplevel(wl_resource *surface_res);

  // The window manager's layout decision for a toplevel: laid out in a
  // tiling container (`tiled`) or floating. Tells the client, via the
  // xdg_toplevel TILED_* states, that it is being tiled and so must fill
  // exactly the size it is configured with and draw no shadow or rounded
  // corners -- the difference between a terminal that fills its tile and one
  // that stops a cell short of it. Sends a fresh configure when the answer
  // changes even though the size has not; never resizes the X11 window, so
  // the window manager keeps sole authority over geometry. Unknown windows
  // are ignored: the watcher reports every window the WM manages, not only
  // ours.
  //
  // This is the all-four-together entry point, for a window manager with no
  // finer-grained signal than "tiled or not" -- true sets every one of
  // TILED_LEFT/RIGHT/TOP/BOTTOM, false clears all four. set_tiled_edges()
  // below is the per-edge counterpart.
  void set_tiled(uint32_t x11_window, bool tiled);

  // Per-edge counterpart to set_tiled() above, for a window manager that
  // can say which specific edges are part of the tiling arrangement (see
  // ww::TiledEdges' own comment) rather than only "tiled or not". The two
  // never disagree about a window reported through only one of them; where
  // both are wired to the same window (unusual -- a watcher would need to
  // call both), the last call simply wins, same as any other repeated
  // set_tiled()/set_tiled_edges().
  void set_tiled_edges(uint32_t x11_window, TiledEdges edges);

  // Keyboard focus moved to `surface` (nullptr: to nothing of ours). If it is
  // a popup or subsurface, the toplevel at the root of its parent chain is
  // the one that counts, so a menu with focus does not deactivate its own
  // window. Marks that toplevel ACTIVATED and every other one not, sending a fresh
  // configure (same size, new states) to each whose answer changed. Purely
  // informational for the client; it never changes geometry or focus itself.
  void set_activated_surface(wl_resource *surface);

  // The window manager's own account of a toplevel (EWMH _NET_WM_STATE, or
  // any backend's equivalent), stored verbatim. This -- never the client's
  // request -- is what the client is told: FULLSCREEN when the WM reports
  // fullscreen, MAXIMIZED when it reports both axes maximized (which, filling
  // exactly the area it was given, is also reported as tiled on every edge,
  // unless fullscreen). It is kept apart from set_tiled()/set_tiled_edges()
  // (a tiling WM's per-edge IPC), and the two are combined when states are
  // built, so neither can erase what the other said. A request from the
  // client (set_toplevel_state) is only forwarded to the WM and answered with
  // a configure; the state changes when the WM is seen to have changed it.
  // Wired automatically to every backend's set_wm_state_callback in the
  // constructor.
  void set_wm_state(uint32_t x11_window, IWindowBackend::WmWindowState state);

  // wl_subcompositor / wl_subsurface
  void create_subsurface(wl_resource *subsurface_res, wl_resource *surface_res,
                         wl_resource *parent_res);
  // wl_subsurface.destroy: drops the subsurface role but leaves the
  // wl_surface itself alive (per protocol).
  void destroy_subsurface_role(wl_resource *surface_res);
  // Both set_position and place_subsurface below are double-buffered
  // against the *parent's* next applied state, not this call -- see
  // Subsurface::pending_position and ::pending_restack.
  void set_subsurface_position(wl_resource *surface_res, int32_t x, int32_t y);
  void place_subsurface(wl_resource *surface_res, wl_resource *sibling_res,
                        bool above);
  // wl_subsurface.set_sync (true) / set_desync (false). Takes effect
  // immediately; see this method's own implementation comment for the one
  // conditional side effect set_desync can have (flushing cached state if
  // the parent is itself desynchronized).
  void set_subsurface_sync(wl_resource *surface_res, bool sync);

  // xdg_popup — a popup is structurally a subsurface-like X11 child window
  // (positioned relative to its parent, clipped, moved with it) whose
  // position instead comes from an xdg_positioner rather than
  // wl_subsurface.set_position.
  void create_popup(wl_resource *surface_res, wl_resource *parent_surface_res,
                    wl_resource *popup_res, int32_t x, int32_t y,
                    uint16_t width, uint16_t height);
  // Re-place an already-mapped popup (xdg_popup.reposition).
  void reposition_popup(wl_resource *surface_res, int32_t x, int32_t y,
                        uint16_t width, uint16_t height);
  // xdg_popup.destroy / resource teardown: drops the popup role, tears down
  // its X11 window, and removes it from the dismiss stack. Idempotent.
  void destroy_popup(wl_resource *surface_res);
  // Called on every pointer button press. If one or more popups are mapped
  // and the click did not land on one of them, every mapped popup is sent
  // popup_done (topmost first) and unmapped — the standard xdg_popup
  // "click outside dismisses" behavior.
  void dismiss_popups_not_owning(wl_resource *clicked_surface) override;

  // wl_surface.set_input_region, double-buffered: takes effect on the next
  // commit(). `has_region == false` means set_input_region(nullptr) — reset
  // to "the whole surface accepts input". Rectangles are a snapshot of the
  // wl_region's add()/subtract() calls at the time set_input_region was
  // sent, since the client is free to destroy or keep mutating the region
  // object afterward.
  void set_pending_input_region(wl_resource *surface_res,
                                std::vector<InputRect> add_rects,
                                std::vector<InputRect> sub_rects,
                                bool has_region);

  // wl_pointer.set_cursor: gives a wl_surface the cursor role, so its
  // commits become backend cursors instead of window contents (see
  // ISurfaceInteractions::set_cursor_surface and Surface::is_cursor()).
  void set_cursor_surface(wl_resource *surface_res, int32_t hotspot_x,
                          int32_t hotspot_y,
                          CursorChangedCallback on_changed) override;
  void clear_cursor_surface(wl_resource *surface_res) override;
  [[nodiscard]] uint32_t cursor_for_surface(
      wl_resource *surface_res) const override;

  // Buffer pipeline
  void attach_buffer(wl_resource *surface, wl_resource *buffer);
  // Damage requests are double-buffered until commit. This backend currently
  // repaints whole buffers, so the request only needs to preserve whether a
  // non-empty damage rectangle was submitted.
  void mark_surface_damage(wl_resource *surface, int32_t width,
                           int32_t height);
  void add_frame_callback(wl_resource *surface, wl_resource *callback);
  // Configure serials are recorded when xdg_surface.configure is sent.
  // Returns false when `serial` was not issued for this surface.
  void record_configure_serial(wl_resource *surface, uint32_t serial);
  [[nodiscard]] bool acknowledge_configure(wl_resource *surface,
                                           uint32_t serial);
  void commit(wl_resource *surface);

  // wl_surface.offset, double-buffered: takes effect on the next commit(),
  // added to the running total rather than replacing it (see
  // Surface::offset_{x,y}'s doc comment for why). `x`/`y` here are
  // this one request's own value, not yet accumulated -- a second call
  // before the next commit replaces this pending value rather than
  // stacking with it, matching wl_surface.attach's pre-offset-request x/y
  // argument it stands in for.
  void set_pending_offset(wl_resource *surface_res, int32_t x, int32_t y);

  // wl_surface.set_buffer_scale, double-buffered: takes effect on the next
  // commit(). See Surface::buffer_scale's doc comment -- recorded for
  // diagnostics, since this compositor does not resample presented pixels.
  void set_pending_buffer_scale(wl_resource *surface_res, int32_t scale);

  // wp_viewport.set_source, double-buffered: takes effect on the next
  // commit(). All four values are wl_fixed_t. When all four are -1.0 (wl_fixed_from_int(-1)),
  // the source rectangle is unset.
  void set_pending_viewport_source(wl_resource *surface_res,
                                   wl_fixed_t x, wl_fixed_t y,
                                   wl_fixed_t width, wl_fixed_t height);

  // wp_viewport.set_destination, double-buffered: takes effect on the next
  // commit(). When both are -1, the destination size is unset.
  void set_pending_viewport_destination(wl_resource *surface_res,
                                        int32_t width, int32_t height);

  // Input: look up the wl_surface resource for a given X11 window id.
  // Returns nullptr if the window is not tracked.
  [[nodiscard]] wl_resource *surface_for_window(uint32_t x11_window) const override;

  void on_expose(uint32_t x11_window) override;

  // Window-geometry origin (geometry.x/y when declared, else 0,0). See
  // ISurfaceInteractions::surface_local_origin.
  void surface_local_origin(wl_resource *surface, int32_t &x,
                            int32_t &y) const override;

  void request_close(uint32_t x11_window);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww