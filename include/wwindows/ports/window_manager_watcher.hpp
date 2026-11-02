#pragma once

#include <cstdint>
#include <functional>

namespace ww {

// Which of a toplevel's four edges a window manager reports as part of the
// tiling arrangement -- i.e. not freely resizable by dragging, and not in
// need of a drop shadow or rounded corner on that side. All-false is
// "floating" (or not yet reported).
//
// A window manager with no finer-grained signal than "tiled or not" (the
// generic EWMH maximized-both-axes heuristic X11Backend falls back to when
// nothing richer is available) reports all four together or all four
// false -- see SurfaceManager::set_tiled's own doc comment for that
// simpler case. i3, which can and does report real per-window geometry via
// its IPC tree, can in principle report a true subset through
// set_tiled_edges -- see i3_wire.hpp's edges_within_workspace for why, for
// i3 specifically, that subset turns out to always be all four or all
// false in practice: i3's tiling model fully partitions each workspace, so
// every edge of every tiled window always borders either a sibling or the
// workspace boundary, never open space. The per-edge plumbing is still
// real throughout this port and SurfaceManager, for a window manager whose
// layout model does leave a genuine subset (a DE's own edge-snap/half-tile
// feature, say) to report through it correctly.
struct TiledEdges {
  bool left{false};
  bool right{false};
  bool top{false};
  bool bottom{false};

  [[nodiscard]] bool any() const { return left || right || top || bottom; }

  friend bool operator==(const TiledEdges &, const TiledEdges &) = default;
};

// Outbound port: an out-of-band source of truth about how the window manager
// is laying out our toplevels.
//
// Wayland clients are told, through xdg_toplevel's TILED_* states, that the
// compositor is tiling them and that they therefore must not draw a drop
// shadow or rounded corners and must fill exactly the size they were
// configured with. X11 gives this backend no way to learn that from the
// window alone: a plain ICCCM/EWMH window manager only reports a size. Some
// window managers do publish it, each in its own way (i3 over its IPC
// socket), so each gets an adapter behind this one interface and the core
// never learns which it is talking to.
//
// The watcher never resizes anything. Geometry stays the window manager's;
// this only reports *why* a window has the geometry it has.
class IWindowManagerWatcher {
public:
  // Called with a toplevel's current tiling every time the watcher learns
  // the window manager's layout (at connect, and again after each event that
  // may have changed it). Level-triggered: the same edges may be reported
  // repeatedly, and the receiver -- which owns the per-window state -- must
  // treat an unchanged report as a no-op. Never called for a window that
  // has been closed.
  using TilingCallback =
      std::function<void(uint32_t x11_window, TiledEdges edges)>;

  virtual ~IWindowManagerWatcher() = default;

  virtual void set_tiling_callback(TilingCallback cb) = 0;
};

} // namespace ww
