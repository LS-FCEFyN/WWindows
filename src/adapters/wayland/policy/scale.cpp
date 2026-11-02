#include <wwindows/adapters/wayland/policy/scale.hpp>
#include <algorithm>
#include <cmath>
namespace ww::wayland::policy {
uint32_t compute_scale_120ths(int32_t width_px, int32_t width_mm) noexcept {
  if (width_mm <= 50) return kMinScale120;
  const double dpi = (static_cast<double>(width_px) * 25.4) /
                     static_cast<double>(width_mm);
  const double scale = dpi / 96.0;
  const long rounded = std::lround(scale * 120.0);
  const long lo = static_cast<long>(kMinScale120);
  const long hi = static_cast<long>(kMaxScale120);
  return static_cast<uint32_t>(std::clamp(rounded, lo, hi));
}
}
