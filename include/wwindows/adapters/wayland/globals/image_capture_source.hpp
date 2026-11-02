#pragma once

// Image capture source protocol global registration
// ext-image-capture-source-v1

#include <wwindows/support/error.hpp>

struct wl_display;
struct wl_resource;

namespace ww {

class SurfaceManager;

namespace image_capture_source {

// Register the ext_image_capture_source_manager_v1 global on the given display.
Result<void *> register_image_capture_source(wl_display *display, SurfaceManager &surface_manager);

} // namespace image_capture_source
} // namespace ww