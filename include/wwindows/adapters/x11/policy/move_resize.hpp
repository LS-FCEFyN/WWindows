#pragma once
#include "wwindows/ports/window_backend.hpp"

namespace ww::x11::policy {

// Maps xdg_toplevel.resize's edge to the "direction" value that goes in
// an EWMH _NET_WM_MOVERESIZE client message's data.l[2]. Per the wm-spec
// (ar01s04.html, "_NET_WM_MOVERESIZE"):
//
//   _NET_WM_MOVERESIZE_SIZE_TOPLEFT      0
//   _NET_WM_MOVERESIZE_SIZE_TOP          1
//   _NET_WM_MOVERESIZE_SIZE_TOPRIGHT     2
//   _NET_WM_MOVERESIZE_SIZE_RIGHT        3
//   _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT  4
//   _NET_WM_MOVERESIZE_SIZE_BOTTOM       5
//   _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT   6
//   _NET_WM_MOVERESIZE_SIZE_LEFT         7
//
// `ResizeEdge::None` has no corresponding direction on the wire -- a
// client resizing without pinning down an edge. Mapped here to
// BottomRight (4), the corner most direct-manipulation resize grips
// default to when nothing else says otherwise.
inline uint32_t net_wm_moveresize_direction(ResizeEdge edge) noexcept {
  switch (edge) {
  case ResizeEdge::TopLeft:
    return 0;

  case ResizeEdge::Top:
    return 1;
  case ResizeEdge::TopRight:
    return 2;
  case ResizeEdge::Right:
    return 3;
  case ResizeEdge::BottomRight:
    return 4;
  case ResizeEdge::Bottom:
    return 5;
  case ResizeEdge::BottomLeft:
    return 6;
  case ResizeEdge::Left:
    return 7;
  case ResizeEdge::None:
  default:
    return 4; // BottomRight
  }
}

// _NET_WM_MOVERESIZE_MOVE: the fixed direction value for an interactive
// *move* rather than a resize (there is no edge to pick).
inline constexpr uint32_t net_wm_moveresize_move_direction = 8;

} // namespace ww::x11::policy
