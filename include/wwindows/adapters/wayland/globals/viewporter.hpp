#pragma once

#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

// Registers wp_viewporter v1.
Result<void *> register_viewporter(wl_display *display);

} // namespace ww