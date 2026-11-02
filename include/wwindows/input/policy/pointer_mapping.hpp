#pragma once
#include <cstdint>
namespace ww::input::policy {
inline uint32_t linux_button(uint8_t button) noexcept {
  switch (button) { case 1: return 0x110; case 2: return 0x112; case 3: return 0x111; default: return 0; }
}
struct Axis { uint32_t axis{0}; double value{0.0}; };
inline bool scroll_axis(uint8_t button, Axis& out) noexcept {
  if (button < 4 || button > 7) return false;
  const bool vertical = button == 4 || button == 5;
  out.axis = vertical ? 0u : 1u;
  out.value = (button == 4 || button == 6) ? -15.0 : 15.0;
  return true;
}
}
