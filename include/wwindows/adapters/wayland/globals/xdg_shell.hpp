#pragma once

#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

class SurfaceManager;

// Register xdg-shell global
VoidResult register_xdg_shell(wl_display *display,
                              SurfaceManager &surface_manager);

} // namespace ww