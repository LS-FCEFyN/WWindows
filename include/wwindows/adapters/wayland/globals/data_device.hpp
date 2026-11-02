#pragma once
#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

class SurfaceManager;
class InputTranslator;
class IWindowBackend;

// Registers wl_data_device_manager v3 on the display:
//   - wl_data_device.set_selection / .selection — the Wayland clipboard,
//     mirrored to the X11 CLIPBOARD selection (see X11Backend's clipboard
//     methods) so copy/paste works with native X11 applications too.
//   - wl_data_device.start_drag — drag-and-drop between WWindows-managed
//     Wayland clients. Scoped to that: there's no XDND bridge, so a drag
//     started over a native X11 window (or vice versa) doesn't interop.
//     The optional drag icon surface is accepted but not rendered.
//
// Must be called after register_core_globals (needs a bound wl_seat's
// InputTranslator) and after the window backend is up.
VoidResult register_data_device_manager(wl_display *display,
                                        SurfaceManager &surface_manager,
                                        InputTranslator &input_translator,
                                        IWindowBackend &backend);

} // namespace ww
