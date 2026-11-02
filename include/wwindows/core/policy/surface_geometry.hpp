#pragma once

// Pure geometry decisions for the surface layer.
//
// Everything here is called by production code (SurfaceManager_buffers.cpp)
// and covered by tests/core/core_tests.cpp -- the same definitions, not a
// parallel copy. tests/architecture/check_dependencies.py fails the build if
// a policy declared here ever stops being referenced from src/, because an
// unused-but-tested policy reports coverage the compositor does not have.

#include <algorithm>
#include <cstdint>

namespace ww::core::policy {

struct Size {
  uint16_t width{1};
  uint16_t height{1};
};

// Clamp client-supplied buffer dimensions into the range an X11 window can
// actually take. Buffer sizes arrive as int32_t straight from the client; a
// bare narrowing cast wraps (65536 -> 0) and a zero-sized window is a
// protocol error, so every conversion goes through here.
inline Size clamp_size(int width, int height) noexcept {
  return {static_cast<uint16_t>(std::clamp(width, 1, 65535)),
          static_cast<uint16_t>(std::clamp(height, 1, 65535))};
}

} // namespace ww::core::policy
