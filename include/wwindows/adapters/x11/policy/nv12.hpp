#pragma once

#include <algorithm>
#include <cstdint>

namespace ww::x11::policy {

// Convert one limited-range BT.601 Y'CbCr sample to the 0xAARRGGBB value
// expected by the XRGB8888 SHM presenter. linux-dmabuf carries no colorimetry
// in this path, so BT.601 limited range is the documented compatibility
// default for linear NV12.
inline uint32_t nv12_to_xrgb(uint8_t y, uint8_t u, uint8_t v) noexcept {
  const int c = std::max(0, static_cast<int>(y) - 16);
  const int d = static_cast<int>(u) - 128;
  const int e = static_cast<int>(v) - 128;
  const auto channel = [](int value) noexcept {
    return static_cast<uint32_t>(std::clamp(value, 0, 255));
  };
  const uint32_t r = channel((298 * c + 409 * e + 128) >> 8);
  const uint32_t g = channel((298 * c - 100 * d - 208 * e + 128) >> 8);
  const uint32_t b = channel((298 * c + 516 * d + 128) >> 8);
  return 0xff000000u | (r << 16) | (g << 8) | b;
}

} // namespace ww::x11::policy
