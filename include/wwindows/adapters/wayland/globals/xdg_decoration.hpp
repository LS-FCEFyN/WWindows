#pragma once

#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

// Registers zxdg_decoration_manager_v1 v1.
//
// Strategy: always configure server-side decorations.
// The X11 window manager already draws the title bar / border, so CSD would
// produce a double frame.  If a client calls set_mode(CLIENT_SIDE) we still
// respond with a configure(SERVER_SIDE) — this is explicitly allowed by the
// protocol ("The compositor's decision always wins").
VoidResult register_xdg_decoration(wl_display *display);

} // namespace ww