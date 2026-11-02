// wl_region — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <cstdint>
#include <limits>

namespace ww {

namespace {
// wl_region.add/subtract take int32_t; X11 (and InputRect) use
// int16_t/uint16_t. Clamp rather than truncate so a pathological or
// off-screen rectangle degrades gracefully instead of wrapping around.
int16_t clamp_to_i16(int32_t v) {
  if (v < std::numeric_limits<int16_t>::min())
    return std::numeric_limits<int16_t>::min();
  if (v > std::numeric_limits<int16_t>::max())
    return std::numeric_limits<int16_t>::max();
  return static_cast<int16_t>(v);
}
uint16_t clamp_to_u16(int32_t v) {
  if (v < 0)
    return 0;
  if (v > std::numeric_limits<uint16_t>::max())
    return std::numeric_limits<uint16_t>::max();
  return static_cast<uint16_t>(v);
}

void region_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
void region_add(wl_client *, wl_resource *r, int32_t x, int32_t y,
                int32_t width, int32_t height) {
  auto *d = static_cast<RegionData *>(wl_resource_get_user_data(r));
  if (!d || width <= 0 || height <= 0)
    return;
  d->add_rects.push_back({clamp_to_i16(x), clamp_to_i16(y),
                          clamp_to_u16(width), clamp_to_u16(height)});
}
void region_subtract(wl_client *, wl_resource *r, int32_t x, int32_t y,
                     int32_t width, int32_t height) {
  auto *d = static_cast<RegionData *>(wl_resource_get_user_data(r));
  if (!d || width <= 0 || height <= 0)
    return;
  d->sub_rects.push_back({clamp_to_i16(x), clamp_to_i16(y),
                          clamp_to_u16(width), clamp_to_u16(height)});
}
} // namespace

const struct wl_region_interface region_iface = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

void region_resource_destroy(wl_resource *r) {
  delete static_cast<RegionData *>(wl_resource_get_user_data(r));
}

} // namespace ww
