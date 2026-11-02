#pragma once

#include <cstdint>
#include <functional>

struct wl_resource;

namespace ww {

// Input-facing port for the subset of surface operations that the input
// subsystem needs. Keeping this separate from SurfaceManager prevents the
// input layer from depending on the complete compositor state machine.
class ISurfaceInteractions {
public:
  virtual ~ISurfaceInteractions() = default;

  [[nodiscard]] virtual wl_resource *surface_for_window(uint32_t window) const = 0;
  virtual void dismiss_popups_not_owning(wl_resource *clicked_surface) = 0;
  virtual void on_expose(uint32_t window) = 0;

  // wl_pointer.set_cursor. Gives `surface` the cursor role: from now on every
  // commit of it turns its buffer into a window-system cursor (see
  // IWindowBackend::create_cursor) instead of presenting it in a window.
  // The hotspot is in surface-local coordinates, as on the wire.
  //
  // `on_changed` fires each time a commit produced a new cursor image, so
  // the caller can apply it to the window the pointer is over; it is never
  // called from inside this function. A surface that already holds committed
  // content yields its cursor immediately -- ask cursor_for_surface() right
  // after this call rather than waiting for `on_changed`.
  using CursorChangedCallback = std::function<void()>;
  virtual void set_cursor_surface(wl_resource *surface, int32_t hotspot_x,
                                  int32_t hotspot_y,
                                  CursorChangedCallback on_changed) = 0;
  virtual void clear_cursor_surface(wl_resource *surface) = 0;
  // The backend cursor handle for the surface's current image, or 0 when it
  // has none yet (nothing committed, or the image could not be built). The
  // handle belongs to the surface: it stays valid until the next commit that
  // replaces it, so callers apply it at once and do not store it.
  [[nodiscard]] virtual uint32_t cursor_for_surface(
      wl_resource *surface) const = 0;

  // The surface-local position of the top-left corner of `surface`'s X11
  // window, i.e. its declared xdg window-geometry origin (0,0 when none is
  // declared). X11 reports pointer coordinates relative to that window, but
  // Wayland clients expect them relative to the wl_surface's own origin, so
  // the input layer adds this back on before sending wl_pointer /
  // wl_data_device positions. Written to `x`/`y` (overwritten, not
  // accumulated).
  virtual void surface_local_origin(wl_resource *surface, int32_t &x,
                                    int32_t &y) const = 0;
};

} // namespace ww
