#pragma once
#include <cstdint>
namespace ww::wayland::policy {
uint32_t compute_scale_120ths(int32_t width_px, int32_t width_mm) noexcept;
constexpr uint32_t kMinScale120 = 120;
constexpr uint32_t kMaxScale120 = 480;
}
