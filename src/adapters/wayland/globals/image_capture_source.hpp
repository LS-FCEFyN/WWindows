#pragma once

// Image capture source protocol global registration
// ext-image-capture-source-v1

#include <expected>
#include <wayland-server-core.h>

namespace ww {

class SurfaceManager;

namespace image_capture_source {

// Register the ext_image_capture_source_manager_v1 global on the given display.
std::expected<void *, std::error_code>
register_image_capture_source(wl_display *display, ww::SurfaceManager &surface_manager);

} // namespace image_capture_source
} // namespace ww