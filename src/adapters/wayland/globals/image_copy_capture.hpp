#pragma once

// Image copy capture protocol global registration
// ext-image-copy-capture-v1

#include <expected>
#include <wayland-server-core.h>

namespace ww {

class SurfaceManager;

namespace image_copy_capture {

// Register the ext_image_copy_capture_manager_v1 global on the given display.
Result<void *> register_image_copy_capture(wl_display *display, SurfaceManager &surface_manager);

} // namespace image_copy_capture
} // namespace ww