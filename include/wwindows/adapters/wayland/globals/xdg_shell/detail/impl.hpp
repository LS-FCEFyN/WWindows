#pragma once

// Shared between the files in sources/wayland/xdg_shell/ that together
// implement register_xdg_shell() (see ../XdgShell.hpp, one directory up,
// for the public entry point): structs and functions that cross the
// file-per-interface boundary.
//
//   xdg_shell/Positioner.cpp — xdg_positioner
//   xdg_shell/Surface.cpp    — xdg_surface (creates xdg_toplevel / xdg_popup)
//   xdg_shell/Toplevel.cpp   — xdg_toplevel
//   xdg_shell/Popup.cpp      — xdg_popup
//   xdg_shell/WmBase.cpp     — xdg_wm_base, register_xdg_shell()
//
// Nothing here is part of WWindows' public API.

#include <cstdint>

struct wl_client;
struct wl_resource;
struct xdg_toplevel_interface;
struct xdg_surface_interface;
struct xdg_popup_interface;

namespace ww {

class SurfaceManager;

// -------------------------------------------------------------------------
// xdg_surface (xdg_shell/Surface.cpp)
// -------------------------------------------------------------------------
struct XdgSurfaceData {
  SurfaceManager *manager;
  wl_resource *surface;
  // xdg_surface.get_toplevel / .get_popup: whether one of them has already
  // been called on this xdg_surface. The protocol allows exactly one --
  // xdg_surface.error.already_constructed for a second call, of either
  // kind, checked in Surface.cpp before creating the new toplevel/popup
  // resource or touching SurfaceManager at all.
  bool has_role{false};
};

extern const struct xdg_surface_interface xdg_surface_impl;
// Resource destructor for an xdg_surface's XdgSurfaceData. Passed to
// wl_resource_set_implementation so the data is freed however the resource
// dies, not only when the client sends xdg_surface.destroy.
void xdg_surface_resource_destroy(struct wl_resource *resource);

// -------------------------------------------------------------------------
// xdg_positioner (xdg_shell/Positioner.cpp)
// -------------------------------------------------------------------------
// Real state, replacing the previous no-op stub — an xdg_popup's placement
// is computed straight from this (see compute_popup_position below).
struct PositionerState {
  int32_t width{0};
  int32_t height{0};
  int32_t anchor_x{0};
  int32_t anchor_y{0};
  int32_t anchor_w{0};
  int32_t anchor_h{0};
  uint32_t anchor{0};   // XDG_POSITIONER_ANCHOR_NONE
  uint32_t gravity{0};  // XDG_POSITIONER_GRAVITY_NONE
  uint32_t constraint_adjustment{0};
  int32_t offset_x{0};
  int32_t offset_y{0};
};

// Creates and installs an xdg_positioner object. Extracted out of
// xdg_wm_base.create_positioner (xdg_shell/WmBase.cpp) so the positioner's
// own request handlers live next to the state they manipulate.
void create_positioner(wl_client *client, int version, uint32_t id);

// Computes the popup's parent-relative top-left position from a positioner:
// find the anchor point on the anchor rect, then place the popup so the
// corner named by `gravity` sits on that point, then apply the offset.
// This is the standard xdg_positioner algorithm minus constraint
// re-adjustment against output bounds — WWindows only ever advertises one
// wl_output and doesn't track its usable geometry precisely enough to
// reconstrain against it, so a popup positioned to hang off the screen
// edge is a known simplification rather than a crash risk (X11 just clips
// it, same as it would clip an ordinary override-redirect window).
void compute_popup_position(const PositionerState &pos, int32_t &out_x,
                            int32_t &out_y);

// -------------------------------------------------------------------------
// xdg_toplevel (xdg_shell/Toplevel.cpp)
// -------------------------------------------------------------------------
extern const struct xdg_toplevel_interface toplevel_interface;

// -------------------------------------------------------------------------
// xdg_popup (xdg_shell/Popup.cpp)
// -------------------------------------------------------------------------
// Carries what reposition/destroy need to talk back to SurfaceManager:
// this popup's own wl_surface + xdg_surface (for re-sending configure) and
// its parent's wl_surface (for repositioning math).
struct PopupData {
  SurfaceManager *manager;
  wl_resource *surface;
  wl_resource *xdg_surface;
  wl_resource *parent_surface;
};

extern const struct xdg_popup_interface popup_interface;
void popup_resource_destroy(struct wl_resource *resource);

} // namespace ww
