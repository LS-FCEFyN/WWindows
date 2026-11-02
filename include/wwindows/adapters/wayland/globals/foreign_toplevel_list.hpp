#pragma once

// Foreign toplevel list protocol global registration
// ext-foreign-toplevel-list-v1

#include <wwindows/support/error.hpp>

#include <cstdint>

struct wl_display;
struct wl_resource;
struct wl_client;

namespace ww {

class SurfaceManager;

namespace foreign_toplevel_list {

// Register the ext_foreign_toplevel_list_v1 global on the given display.
// The surface_manager is used to track xdg_toplevel surfaces and emit events.
Result<void *> register_foreign_toplevel_list(wl_display *display, SurfaceManager &surface_manager);

// Called by SurfaceManager when a surface gets an xdg_toplevel role
wl_resource *create_toplevel_handle(void *global, wl_client *client, uint32_t version, wl_resource *surface, const char *identifier);

// Called by SurfaceManager when a surface loses its xdg_toplevel role (unmapped/destroyed)
void close_toplevel_handle(void *global, wl_resource *surface);

// Called by SurfaceManager when a toplevel's title changes
void set_toplevel_title(void *global, wl_resource *surface, const char *title);

// Called by SurfaceManager when a toplevel's app_id changes
void set_toplevel_app_id(void *global, wl_resource *surface, const char *app_id);

} // namespace foreign_toplevel_list
} // namespace ww