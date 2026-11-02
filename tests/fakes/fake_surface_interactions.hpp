#pragma once

// A recording in-memory ISurfaceInteractions.
//
// InputTranslator reaches surfaces exclusively through this narrow port
// rather than through SurfaceManager, which is what lets the whole input
// layer be tested without a compositor state machine behind it. This fake
// is the other half of that claim.

#include <wwindows/ports/surface_interactions.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

struct wl_resource;

namespace ww_test {

struct FakeSurfaceInteractions final : ww::ISurfaceInteractions {
  // window id -> surface, populated by tests to model what the compositor
  // currently has mapped.
  std::unordered_map<uint32_t, wl_resource *> window_to_surface;

  std::vector<wl_resource *> dismiss_calls;
  std::vector<uint32_t> expose_calls;
  struct CursorSetCall {
    wl_resource *surface{nullptr};
    int32_t hotspot_x{0};
    int32_t hotspot_y{0};
  };
  std::vector<CursorSetCall> cursor_set_calls;
  std::vector<wl_resource *> cursor_cleared_calls;
  // Models SurfaceManager's cursor bookkeeping: the handle each cursor
  // surface currently owns, and the callback to run when a commit changes it.
  std::unordered_map<wl_resource *, uint32_t> cursor_handles;
  std::unordered_map<wl_resource *, CursorChangedCallback> cursor_callbacks;

  // A client committing a new image on its cursor surface: the surface now
  // owns `handle`, and the input layer is told, as SurfaceManager would.
  void commit_cursor(wl_resource *surface, uint32_t handle) {
    cursor_handles[surface] = handle;
    if (auto it = cursor_callbacks.find(surface);
        it != cursor_callbacks.end() && it->second)
      it->second();
  }

  // surface -> declared window-geometry origin, populated by tests. A
  // surface with no entry has no declared geometry, i.e. origin (0,0).
  std::unordered_map<wl_resource *, std::pair<int32_t, int32_t>> origins;

  void set_origin(wl_resource *surface, int32_t x, int32_t y) {
    origins[surface] = {x, y};
  }

  void bind(uint32_t window, wl_resource *surface) {
    window_to_surface[window] = surface;
  }

  wl_resource *surface_for_window(uint32_t window) const override {
    auto it = window_to_surface.find(window);
    return it == window_to_surface.end() ? nullptr : it->second;
  }
  void dismiss_popups_not_owning(wl_resource *clicked) override {
    dismiss_calls.push_back(clicked);
  }
  void on_expose(uint32_t window) override { expose_calls.push_back(window); }
  void set_cursor_surface(wl_resource *surface, int32_t hotspot_x,
                          int32_t hotspot_y,
                          CursorChangedCallback on_changed) override {
    cursor_set_calls.push_back({surface, hotspot_x, hotspot_y});
    cursor_callbacks[surface] = std::move(on_changed);
  }
  void clear_cursor_surface(wl_resource *surface) override {
    cursor_cleared_calls.push_back(surface);
    cursor_handles.erase(surface);
    cursor_callbacks.erase(surface);
  }
  uint32_t cursor_for_surface(wl_resource *surface) const override {
    auto it = cursor_handles.find(surface);
    return it == cursor_handles.end() ? 0 : it->second;
  }
  void surface_local_origin(wl_resource *surface, int32_t &x,
                            int32_t &y) const override {
    auto it = origins.find(surface);
    x = it == origins.end() ? 0 : it->second.first;
    y = it == origins.end() ? 0 : it->second.second;
  }
};

} // namespace ww_test
