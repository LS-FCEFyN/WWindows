#pragma once

#include "wwindows/ports/window_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ww::detail {

struct ScaledPixels {
  uint16_t width{0};
  uint16_t height{0};
  uint32_t stride{0};
  std::vector<uint8_t> bytes;
};

// Downsample the 32-bit wl_shm formats WWindows advertises to its 1:1 X11
// output. Wayland buffer_scale is an integer scale, so nearest-neighbour
// sampling preserves exact channel bytes and needs no format-specific math.
// The source stride may include padding; the output is tightly packed.
inline ScaledPixels scale_shm_32(const void *source, uint16_t width,
                                 uint16_t height, uint32_t source_stride,
                                 int32_t scale) {
  const int32_t safe_scale = std::max<int32_t>(scale, 1);
  ScaledPixels out;
  out.width = static_cast<uint16_t>((static_cast<uint32_t>(width) +
                                     static_cast<uint32_t>(safe_scale) - 1u) /
                                    static_cast<uint32_t>(safe_scale));
  out.height = static_cast<uint16_t>((static_cast<uint32_t>(height) +
                                      static_cast<uint32_t>(safe_scale) - 1u) /
                                     static_cast<uint32_t>(safe_scale));
  out.stride = static_cast<uint32_t>(out.width) * 4u;
  out.bytes.resize(static_cast<std::size_t>(out.stride) * out.height);
  const auto *src = static_cast<const uint8_t *>(source);
  for (uint32_t y = 0; y < out.height; ++y) {
    const uint32_t sy = std::min<uint32_t>(y * static_cast<uint32_t>(safe_scale),
                                           static_cast<uint32_t>(height) - 1u);
    for (uint32_t x = 0; x < out.width; ++x) {
      const uint32_t sx = std::min<uint32_t>(x * static_cast<uint32_t>(safe_scale),
                                             static_cast<uint32_t>(width) - 1u);
      std::memcpy(out.bytes.data() + static_cast<std::size_t>(y) * out.stride +
                      static_cast<std::size_t>(x) * 4u,
                  src + static_cast<std::size_t>(sy) * source_stride +
                      static_cast<std::size_t>(sx) * 4u,
                  4u);
    }
  }
  return out;
}

// Stretch (or shrink) a rectangle of a 32-bit wl_shm image to an arbitrary
// dst_width x dst_height, nearest-neighbour. Used to keep a stale client
// frame covering the whole window while an interactive resize is still in
// flight: the window manager has already grown the X11 window, the client has
// not yet drawn at the new size, and the alternative is a transparent or
// black strip.
//
// `region` is in source pixels and selects the part of the buffer to use
// (the xdg window geometry, so a client-drawn shadow is never stretched onto
// the screen); it must be declared. It is clamped to the source_width x
// source_height buffer before anything is read, so a stale or oversized
// region can never read past the buffer: what overlaps the buffer is scaled,
// and a region that does not overlap it at all yields an empty result. Every
// output pixel is a verbatim copy of one source pixel -- nothing is blended,
// so channel bytes survive exactly and the result does not depend on the
// pixel format. The source stride may include padding; the output is tightly
// packed. A zero-sized target or an undeclared region also yields an empty
// result.
inline ScaledPixels scale_shm_32_to_size(const void *source,
                                         uint32_t source_stride,
                                         uint32_t source_width,
                                         uint32_t source_height,
                                         BufferRegion region,
                                         uint16_t dst_width,
                                         uint16_t dst_height) {
  ScaledPixels out;
  if (!source || !region.declared() || dst_width == 0 || dst_height == 0 ||
      source_width == 0 || source_height == 0)
    return out;

  // Intersect the region with the buffer, in 64-bit so no sum can wrap.
  const int64_t left = std::max<int64_t>(region.x, 0);
  const int64_t top = std::max<int64_t>(region.y, 0);
  const int64_t right = std::min<int64_t>(
      static_cast<int64_t>(region.x) + region.width, source_width);
  const int64_t bottom = std::min<int64_t>(
      static_cast<int64_t>(region.y) + region.height, source_height);
  if (right <= left || bottom <= top)
    return out;
  const uint64_t region_width = static_cast<uint64_t>(right - left);
  const uint64_t region_height = static_cast<uint64_t>(bottom - top);
  const uint32_t origin_x = static_cast<uint32_t>(left);
  const uint32_t origin_y = static_cast<uint32_t>(top);

  out.width = dst_width;
  out.height = dst_height;
  out.stride = static_cast<uint32_t>(dst_width) * 4u;
  out.bytes.resize(static_cast<std::size_t>(out.stride) * dst_height);

  // Column sampling is the same for every row: compute it once.
  std::vector<uint32_t> column_byte_offset(dst_width);
  for (uint32_t x = 0; x < dst_width; ++x) {
    const uint64_t sx = static_cast<uint64_t>(x) * region_width / dst_width;
    column_byte_offset[x] = static_cast<uint32_t>((origin_x + sx) * 4u);
  }

  const auto *src = static_cast<const uint8_t *>(source);
  for (uint32_t y = 0; y < dst_height; ++y) {
    const uint64_t sy = static_cast<uint64_t>(y) * region_height / dst_height;
    const uint8_t *src_row =
        src + static_cast<std::size_t>(origin_y + sy) * source_stride;
    uint8_t *dst_row =
        out.bytes.data() + static_cast<std::size_t>(y) * out.stride;
    for (uint32_t x = 0; x < dst_width; ++x)
      std::memcpy(dst_row + static_cast<std::size_t>(x) * 4u,
                  src_row + column_byte_offset[x], 4u);
  }
  return out;
}

inline BufferRegion scale_region(BufferRegion source, int32_t scale) {
  if (!source.declared() || scale <= 1)
    return source;
  const int64_t s = scale;
  const int64_t x = source.x / s;
  const int64_t y = source.y / s;
  const int64_t right = (static_cast<int64_t>(source.x) + source.width + s - 1) / s;
  const int64_t bottom = (static_cast<int64_t>(source.y) + source.height + s - 1) / s;
  return BufferRegion{
      static_cast<int16_t>(std::clamp<int64_t>(x, 0, INT16_MAX)),
      static_cast<int16_t>(std::clamp<int64_t>(y, 0, INT16_MAX)),
      static_cast<uint16_t>(std::clamp<int64_t>(right - x, 0, UINT16_MAX)),
      static_cast<uint16_t>(std::clamp<int64_t>(bottom - y, 0, UINT16_MAX))};
}

inline uint16_t logical_size(uint16_t pixels, int32_t scale) {
  const uint32_t s = static_cast<uint32_t>(std::max<int32_t>(scale, 1));
  return static_cast<uint16_t>((static_cast<uint32_t>(pixels) + s - 1u) / s);
}

} // namespace ww::detail
