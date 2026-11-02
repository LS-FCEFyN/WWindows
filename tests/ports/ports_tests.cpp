#include <wwindows/ports/surface_interactions.hpp>
#include <wwindows/ports/window_backend.hpp>
#include <wwindows/core/detail/pixel_scale.hpp>
#include <wwindows/adapters/x11/policy/nv12.hpp>
#include "../support/test.hpp"

namespace {

struct FakeSurfaceInteractions final : ww::ISurfaceInteractions {
  wl_resource *surface{reinterpret_cast<wl_resource *>(0x1)};
  uint32_t last_window{0};
  bool dismissed{false};
  bool exposed{false};
  bool cursor_set{false};
  bool cursor_cleared{false};

  wl_resource *surface_for_window(uint32_t window) const override {
    return window == last_window ? surface : nullptr;
  }
  void dismiss_popups_not_owning(wl_resource *) override { dismissed = true; }
  void on_expose(uint32_t) override { exposed = true; }
  void set_cursor_surface(wl_resource *, int32_t, int32_t,
                          CursorChangedCallback) override {
    cursor_set = true;
  }
  void clear_cursor_surface(wl_resource *) override { cursor_cleared = true; }
  uint32_t cursor_for_surface(wl_resource *) const override { return 7; }
  void surface_local_origin(wl_resource *, int32_t &x,
                            int32_t &y) const override {
    x = 0;
    y = 0;
  }
};

} // namespace

WW_TEST_CASE(surface_interaction_port_is_implementable) {
  FakeSurfaceInteractions fake;
  fake.last_window = 42;
  WW_TEST_CHECK(fake.surface_for_window(42) == fake.surface);
  WW_TEST_CHECK(fake.surface_for_window(7) == nullptr);
  fake.dismiss_popups_not_owning(fake.surface);
  fake.on_expose(42);
  fake.set_cursor_surface(fake.surface, 1, 2, {});
  WW_TEST_CHECK_EQ(fake.cursor_for_surface(fake.surface), uint32_t{7});
  fake.clear_cursor_surface(fake.surface);
  WW_TEST_CHECK(fake.dismissed && fake.exposed && fake.cursor_set &&
                fake.cursor_cleared);
}

WW_TEST_CASE(input_rect_is_small_and_explicit) {
  ww::InputRect rect{.x = -2, .y = 3, .width = 40, .height = 50};
  WW_TEST_CHECK(rect.x == -2 && rect.y == 3 && rect.width == 40 &&
                rect.height == 50);
}

WW_TEST_CASE(integer_buffer_scale_downsamples_padded_argb_rows) {
  // Four 4-byte pixels per row, with four bytes of source padding. The
  // output samples the top-left pixel of each 2x2 source block.
  const uint8_t source[] = {
      1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255, 99, 99, 99, 99,
      5, 0, 0, 255, 6, 0, 0, 255, 7, 0, 0, 255, 8, 0, 0, 255, 99, 99, 99, 99,
      9, 0, 0, 255, 10, 0, 0, 255, 11, 0, 0, 255, 12, 0, 0, 255, 99, 99, 99, 99,
      13, 0, 0, 255, 14, 0, 0, 255, 15, 0, 0, 255, 16, 0, 0, 255, 99, 99, 99, 99};
  const auto scaled = ww::detail::scale_shm_32(source, 4, 4, 20, 2);
  WW_TEST_CHECK(scaled.width == 2 && scaled.height == 2 && scaled.stride == 8);
  WW_TEST_CHECK(scaled.bytes.size() == 16);
  WW_TEST_CHECK(scaled.bytes[0] == 1 && scaled.bytes[4] == 3);
  WW_TEST_CHECK(scaled.bytes[8] == 9 && scaled.bytes[12] == 11);
}

WW_TEST_CASE(integer_buffer_scale_converts_geometry_to_logical_pixels) {
  const ww::BufferRegion source{.x = 4, .y = 6, .width = 20, .height = 10};
  const auto logical = ww::detail::scale_region(source, 2);
  WW_TEST_CHECK(logical.x == 2 && logical.y == 3);
  WW_TEST_CHECK(logical.width == 10 && logical.height == 5);
  WW_TEST_CHECK(ww::detail::logical_size(101, 2) == 51);
}

WW_TEST_CASE(nv12_conversion_handles_neutral_black_white_and_red) {
  WW_TEST_CHECK(ww::x11::policy::nv12_to_xrgb(16, 128, 128) == 0xff000000u);
  WW_TEST_CHECK(ww::x11::policy::nv12_to_xrgb(235, 128, 128) == 0xffffffffu);
  WW_TEST_CHECK(ww::x11::policy::nv12_to_xrgb(81, 90, 240) == 0xffff0000u);
}

// --- scaling a buffer region to an arbitrary target size --------------------
// Used while a client's frame is stale during an interactive resize: the old
// image is stretched to the size the window manager now wants.

WW_TEST_CASE(scale_to_size_upscales_with_nearest_neighbour_blocks) {
  const uint32_t src[] = {0xff000001u, 0xff000002u, 0xff000003u, 0xff000004u};
  const auto out = ww::detail::scale_shm_32_to_size(
      src, 8, 2, 2, ww::BufferRegion{0, 0, 2, 2}, 4, 4);
  WW_TEST_CHECK(out.width == 4 && out.height == 4 && out.stride == 16);
  WW_TEST_CHECK(out.bytes.size() == 64);
  const uint32_t expected[] = {1, 1, 2, 2, 1, 1, 2, 2, 3, 3, 4, 4, 3, 3, 4, 4};
  for (int i = 0; i < 16; ++i) {
    uint32_t px = 0;
    std::memcpy(&px, out.bytes.data() + i * 4, 4);
    WW_TEST_CHECK_EQ(px, 0xff000000u | expected[i]);
  }
}

WW_TEST_CASE(scale_to_size_handles_non_integer_ratios_without_blending) {
  // 3 -> 5 columns: source column = floor(x * 3 / 5) = 0,0,1,1,2.
  const uint32_t src[] = {10, 20, 30};
  const auto out = ww::detail::scale_shm_32_to_size(
      src, 12, 3, 1, ww::BufferRegion{0, 0, 3, 1}, 5, 1);
  WW_TEST_CHECK(out.width == 5 && out.height == 1);
  const uint32_t expected[] = {10, 10, 20, 20, 30};
  for (int i = 0; i < 5; ++i) {
    uint32_t px = 0;
    std::memcpy(&px, out.bytes.data() + i * 4, 4);
    WW_TEST_CHECK_EQ(px, expected[i]); // only source values: never averaged
  }
}

WW_TEST_CASE(scale_to_size_downscales_and_honours_crop_and_row_padding) {
  // 4x2 buffer, stride 20 (4 bytes of padding per row); crop to x=1..2.
  uint32_t rows[2][5] = {{1, 2, 3, 4, 99}, {5, 6, 7, 8, 99}};
  const auto out = ww::detail::scale_shm_32_to_size(
      rows, 20, 4, 2, ww::BufferRegion{1, 0, 2, 2}, 1, 1);
  WW_TEST_CHECK(out.width == 1 && out.height == 1 && out.stride == 4);
  uint32_t px = 0;
  std::memcpy(&px, out.bytes.data(), 4);
  WW_TEST_CHECK_EQ(px, uint32_t{2}); // top-left of the cropped region
}

WW_TEST_CASE(scale_to_size_with_the_same_size_is_an_exact_copy) {
  const uint32_t src[] = {7, 8, 9, 10};
  const auto out = ww::detail::scale_shm_32_to_size(
      src, 8, 2, 2, ww::BufferRegion{0, 0, 2, 2}, 2, 2);
  WW_TEST_CHECK(std::memcmp(out.bytes.data(), src, sizeof(src)) == 0);
}

WW_TEST_CASE(scale_to_size_of_an_empty_target_is_empty_not_a_crash) {
  const uint32_t src[] = {1};
  const auto out = ww::detail::scale_shm_32_to_size(
      src, 4, 1, 1, ww::BufferRegion{0, 0, 1, 1}, 0, 0);
  WW_TEST_CHECK(out.bytes.empty());
}

// The helper is public inline code: a region that does not fit the buffer
// (stale window geometry, a buffer that shrank) must never be read past the
// buffer's edge. It is clamped to the buffer, and what remains is scaled.
WW_TEST_CASE(scale_to_size_clamps_a_region_that_overhangs_the_buffer) {
  // 2x2 buffer; the region asks for 4x4 starting at (1, 1): only the single
  // bottom-right pixel (value 4) is actually inside the buffer.
  const uint32_t src[] = {1, 2, 3, 4};
  const auto out = ww::detail::scale_shm_32_to_size(
      src, 8, 2, 2, ww::BufferRegion{1, 1, 4, 4}, 2, 2);
  WW_TEST_CHECK(out.width == 2 && out.height == 2);
  for (int i = 0; i < 4; ++i) {
    uint32_t px = 0;
    std::memcpy(&px, out.bytes.data() + i * 4, 4);
    WW_TEST_CHECK_EQ(px, uint32_t{4});
  }
}

WW_TEST_CASE(scale_to_size_with_a_region_wholly_outside_the_buffer_is_empty) {
  const uint32_t src[] = {1, 2, 3, 4};
  WW_TEST_CHECK(ww::detail::scale_shm_32_to_size(
                    src, 8, 2, 2, ww::BufferRegion{2, 0, 2, 2}, 4, 4)
                    .bytes.empty());
  WW_TEST_CHECK(ww::detail::scale_shm_32_to_size(
                    src, 8, 2, 2, ww::BufferRegion{0, 5, 2, 2}, 4, 4)
                    .bytes.empty());
}

WW_TEST_CASE(scale_to_size_clamps_a_negative_region_origin) {
  const uint32_t src[] = {1, 2, 3, 4};
  // (-1,-1) 2x2 overlaps the buffer only at its top-left pixel.
  const auto out = ww::detail::scale_shm_32_to_size(
      src, 8, 2, 2, ww::BufferRegion{-1, -1, 2, 2}, 2, 2);
  WW_TEST_CHECK(out.width == 2 && out.height == 2);
  uint32_t px = 0;
  std::memcpy(&px, out.bytes.data(), 4);
  WW_TEST_CHECK_EQ(px, uint32_t{1});
}
