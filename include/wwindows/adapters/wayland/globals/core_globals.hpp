#pragma once

#include <cstdint>
#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

class SurfaceManager;
class InputTranslator;

// What the wl_output reports. The window system owns every number in it (the
// X screen's size, physical size and refresh rate); this is only the last
// value it reported, replaced via update_output() when it changes.
struct OutputState {
  int32_t width_px;
  int32_t height_px;
  int32_t width_mm;
  int32_t height_mm;
  // Refresh rate in mHz, as wl_output.mode wants it. 0 means the window
  // system could not say (X without RandR, or a virtual screen reporting 0),
  // which is announced as 60 Hz because a client must be given some number.
  uint32_t refresh_mhz{0};

  friend bool operator==(const OutputState &, const OutputState &) = default;
};

// The wl_output global and the wl_output resources clients hold on it.
// Owned by the display (freed with it).
class OutputGlobal;

// Register core Wayland globals on the given display:
//   wl_compositor, wl_seat (wired to InputTranslator), wl_output
// Returns the wl_output global so its state can be updated later.
Result<OutputGlobal *> register_core_globals(wl_display *display,
                                             SurfaceManager &surface_manager,
                                             InputTranslator &input_translator,
                                             const OutputState &output_state);

// The window system changed the screen: replace what wl_output reports and
// tell every client that already holds one (geometry, mode, done). A change
// that alters nothing a client is told sends nothing.
void update_output(OutputGlobal &output, const OutputState &state);

} // namespace ww