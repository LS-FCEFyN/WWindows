#pragma once

// Shared between the files in sources/wayland/core_globals/ that together
// implement register_core_globals() (see ../CoreGlobals.hpp, one directory
// up, for the public entry point): structs and functions that cross the
// file-per-interface boundary described in each .cpp's own header comment.
//
//   core_globals/Region.cpp        — wl_region
//   core_globals/Surface.cpp       — wl_surface
//   core_globals/Compositor.cpp    — wl_compositor (creates wl_surface / wl_region)
//   core_globals/Subcompositor.cpp — wl_subcompositor / wl_subsurface
//   core_globals/Seat.cpp          — wl_seat / wl_pointer / wl_keyboard
//   core_globals/Output.cpp        — wl_output
//   core_globals/Register.cpp      — register_core_globals(), ties the
//                                    above four globals together
//
// Nothing here is part of WWindows' public API; it exists purely so the
// files above can hand resources to one another without every one of them
// becoming a god file again.

#include "wwindows/core/surface_manager.hpp" // SurfaceManager, and transitively InputRect
#include "wwindows/adapters/wayland/globals/core_globals.hpp" // OutputState

#include <vector>
#include <wayland-server-core.h> // wl_listener

struct wl_client;
struct wl_resource;
struct wl_region_interface;
struct wl_surface_interface;

namespace ww {

// -------------------------------------------------------------------------
// wl_region (core_globals/Region.cpp)
// -------------------------------------------------------------------------
// Tracks the rectangles passed to add()/subtract() so wl_surface.
// set_input_region can hand a real region to SurfaceManager instead of
// silently discarding it. The two lists are collapsed into "union of adds,
// then punch out subs" (see IWindowBackend::set_input_shape) rather than
// replayed as an interleaved sequence — covers the overwhelmingly common
// case (build up a region, occasionally cut a hole in it) without needing
// ordered multi-op Shape combining.
struct RegionData {
  std::vector<InputRect> add_rects;
  std::vector<InputRect> sub_rects;
};

extern const struct wl_region_interface region_iface;
void region_resource_destroy(wl_resource *r);

// -------------------------------------------------------------------------
// wl_surface (core_globals/Surface.cpp)
// -------------------------------------------------------------------------
extern const struct wl_surface_interface surface_iface;
void surface_resource_destroy(wl_resource *resource);

// -------------------------------------------------------------------------
// wl_compositor / wl_subcompositor / wl_seat / wl_output bind callbacks
// -------------------------------------------------------------------------
// Each is defined in its own core_globals/*.cpp file and only ever
// referenced by core_globals/Register.cpp's wl_global_create() calls.
void bind_compositor(wl_client *client, void *data, uint32_t version,
                     uint32_t id);
void bind_subcompositor(wl_client *client, void *data, uint32_t version,
                        uint32_t id);
void bind_seat(wl_client *client, void *data, uint32_t version, uint32_t id);
void bind_output(wl_client *client, void *data, uint32_t version,
                 uint32_t id);

class OutputGlobal {
public:
  OutputState state;
  // Every wl_output a client currently holds; each removes itself when
  // destroyed. Needed so update_output() can reach them.
  std::vector<wl_resource *> bound;
  wl_listener display_destroy{};
};

} // namespace ww
