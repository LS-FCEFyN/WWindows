#pragma once

#include <cstdint>
#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

// Registers wp_fractional_scale_manager_v1 v1.
//
// scale_120ths — preferred scale factor expressed in units of 1/120.
//   120  → 1.0×   (96 dpi reference)
//   144  → 1.2×
//   180  → 1.5×
//   240  → 2.0×
//
// Derive the value from screen DPI via compute_scale_120ths() below, or
// pass 120 to stay at 1×.
//
// The scale is a function of the X screen's DPI, which the window system owns
// and can change, so the returned global lets it be updated afterwards.
class FractionalScaleGlobal;
Result<FractionalScaleGlobal *> register_fractional_scale(wl_display *display,
                                                          uint32_t scale_120ths);

// The preferred scale changed: tell every wp_fractional_scale_v1 object a
// client holds. Does nothing if the value is the one already in force.
void update_fractional_scale(FractionalScaleGlobal &global,
                             uint32_t scale_120ths);

// Helper: compute a scale_120ths value from physical screen dimensions.
// Returns 120 (1.0×) if width_mm is 0 or the computed DPI is implausibly low
// (many X servers report 0 mm or 96-dpi-equivalent for virtual screens).
//
//   dpi   = width_px / (width_mm / 25.4)
//   scale = dpi / 96.0          (96 dpi = 1×)
//   result = round(scale * 120), clamped to [120, 480]
uint32_t compute_scale_120ths(int32_t width_px, int32_t width_mm);

} // namespace ww