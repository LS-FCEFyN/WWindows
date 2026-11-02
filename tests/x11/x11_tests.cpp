#include <wwindows/adapters/x11/policy/clipboard_mime.hpp>
#include <wwindows/adapters/x11/policy/move_resize.hpp>
#include "../support/test.hpp"
#include <string>
#include <vector>
WW_TEST_CASE(text_mime_policy_is_strict) {
  WW_TEST_CHECK(ww::x11::policy::is_text_mime("text/plain;charset=utf-8"));
  WW_TEST_CHECK(ww::x11::policy::is_text_mime("UTF8_STRING"));
  WW_TEST_CHECK(!ww::x11::policy::is_text_mime("image/png"));
}
WW_TEST_CASE(preferred_text_mime_returns_first_supported_value) {
  std::vector<std::string> values{"application/json", "text/plain", "UTF8_STRING"};
  WW_TEST_CHECK(ww::x11::policy::preferred_text_mime(values) == "text/plain");
}
WW_TEST_CASE(preferred_text_mime_returns_empty_for_binary_only_selection) {
  std::vector<std::string> values{"image/png", "application/octet-stream"};
  WW_TEST_CHECK(ww::x11::policy::preferred_text_mime(values).empty());
}
WW_TEST_CASE(net_wm_moveresize_direction_matches_wm_spec_table) {
  using ww::ResizeEdge;
  using ww::x11::policy::net_wm_moveresize_direction;
  // https://specifications.freedesktop.org/wm-spec/latest/ar01s04.html
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::TopLeft), 0u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::Top), 1u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::TopRight), 2u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::Right), 3u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::BottomRight), 4u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::Bottom), 5u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::BottomLeft), 6u);
  WW_TEST_CHECK_EQ(net_wm_moveresize_direction(ResizeEdge::Left), 7u);
}
WW_TEST_CASE(net_wm_moveresize_direction_maps_none_to_bottom_right) {
  // xdg_toplevel.resize's "none" edge has no wm-spec equivalent; the
  // policy picks BottomRight (4) as the least-surprising default rather
  // than silently misinterpreting the bit pattern as TopLeft (0).
  WW_TEST_CHECK_EQ(
      ww::x11::policy::net_wm_moveresize_direction(ww::ResizeEdge::None), 4u);
}
WW_TEST_CASE(net_wm_moveresize_move_direction_is_eight) {
  // https://specifications.freedesktop.org/wm-spec/latest/ar01s04.html —
  // _NET_WM_MOVERESIZE_MOVE is the fixed value 8.
  WW_TEST_CHECK_EQ(ww::x11::policy::net_wm_moveresize_move_direction, 8u);
}
