// Integration tests for the X11 adapter.
//
// These run against a *real* X server (Xvfb, started by
// tests/x11/run_with_xvfb.sh) rather than a mock. An X11 backend is almost
// entirely a conversation with the server -- window creation, restacking,
// property setting, input shaping -- so a mocked XCB layer would mostly
// assert that the code calls the functions it calls. Instead these assert
// what the server actually ended up believing, by querying it back through
// an independent XCB connection.
//
// Every test that inspects server state does so through that second
// connection, not through the backend, so a backend that merely *thinks* it
// created a window cannot pass.
//
// Skipped automatically (CTest SKIP_RETURN_CODE) when Xvfb is unavailable.

// xcb/xkb.h's generated struct fields include one literally named `explicit`,
// a C++ keyword; same workaround as the backend's own XKB code.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#include <xcb/randr.h>
#include "../support/test.hpp"

#include "x11_harness.hpp"

#include <xcb/xcb.h>
#include <unistd.h>
#include <poll.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using ww_test::Harness;
using ww_test::Inspector;
using ww_test::keep_server_alive;

// --- connection -------------------------------------------------------------

WW_TEST_CASE(backend_connects_and_exposes_a_screen) {
  Harness h;
  WW_TEST_CHECK(h.backend->connection() != nullptr);
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
  WW_TEST_CHECK(h.backend->screen() != nullptr);
  WW_TEST_CHECK(h.backend->screen()->width_in_pixels > 0);
  WW_TEST_CHECK(h.backend->screen()->height_in_pixels > 0);
}

WW_TEST_CASE(dmabuf_device_is_absent_without_dri3) {
  Harness h;
  // Xvfb has no DRI3. The contract is that the backend reports no device
  // rather than fabricating one, which is what makes register_linux_dmabuf
  // skip advertising the global instead of accepting buffers it cannot
  // import.
  if (!h.backend->has_dri3())
    WW_TEST_CHECK(!h.backend->dmabuf_main_device().has_value());
  else
    WW_TEST_CHECK(true); // a DRI3-capable server is also a valid environment
}

// --- window lifecycle -------------------------------------------------------

WW_TEST_CASE(create_window_produces_a_real_server_window) {
  Harness h;
  const uint32_t win = h.backend->create_window(320, 240);
  h.sync();

  WW_TEST_CHECK(win != 0);
  WW_TEST_CHECK(h.inspector.window_exists(win));
  const auto g = h.inspector.geometry(win);
  WW_TEST_CHECK(g.valid);
  WW_TEST_CHECK_EQ(g.width, uint16_t{320});
  WW_TEST_CHECK_EQ(g.height, uint16_t{240});
}

WW_TEST_CASE(new_windows_start_unmapped_until_map_window) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.sync();
  WW_TEST_CHECK(!h.inspector.is_mapped(win));

  h.backend->map_window(win);
  h.sync();
  WW_TEST_CHECK(h.inspector.is_mapped(win));
}

WW_TEST_CASE(resize_window_changes_server_side_geometry) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->map_window(win);
  h.backend->resize_window(win, 640, 480);
  h.sync();

  const auto g = h.inspector.geometry(win);
  WW_TEST_CHECK_EQ(g.width, uint16_t{640});
  WW_TEST_CHECK_EQ(g.height, uint16_t{480});
}

WW_TEST_CASE(destroy_window_removes_it_from_the_server) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.sync();
  WW_TEST_CHECK(h.inspector.window_exists(win));

  h.backend->destroy_window(win);
  h.sync();
  WW_TEST_CHECK(!h.inspector.window_exists(win));
}

WW_TEST_CASE(forget_window_leaves_the_server_window_alone) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.sync();

  h.backend->forget_window(win);
  h.sync();

  // forget_window drops local bookkeeping only. This is what the parent
  // teardown path relies on: X11 destroys children with their parent, so a
  // second explicit destroy would be a BadWindow against a recycled id.
  WW_TEST_CHECK(h.inspector.window_exists(win));
  h.backend->destroy_window(win); // clean up through the real path
}

WW_TEST_CASE(set_window_title_sets_both_wm_name_and_net_wm_name) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_title(win, "hello wwindows");
  h.sync();

  const std::string wm_name =
      h.inspector.string_property(win, XCB_ATOM_WM_NAME);
  const std::string net_wm_name =
      h.inspector.string_property(win, h.inspector.atom("_NET_WM_NAME"));

  // Modern window managers read _NET_WM_NAME; older ones read WM_NAME. A
  // title that only lands in one of them is invisible to half of them.
  WW_TEST_CHECK(wm_name == "hello wwindows");
  WW_TEST_CHECK(net_wm_name == "hello wwindows");
}

// --- window identity and hints ----------------------------------------------

WW_TEST_CASE(set_window_class_writes_instance_then_class_nul_terminated) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_class(win, "foot", "Foot");
  h.sync();

  // ICCCM: "instance\0class\0", terminators included in the length.
  const std::string got = h.inspector.string_property(win, XCB_ATOM_WM_CLASS);
  WW_TEST_CHECK(got == std::string("foot\0Foot\0", 10));
}

WW_TEST_CASE(set_window_class_with_an_empty_name_removes_it) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_class(win, "foot", "foot");
  h.backend->set_window_class(win, "", "");
  h.sync();

  WW_TEST_CHECK(h.inspector.string_property(win, XCB_ATOM_WM_CLASS).empty());
}

WW_TEST_CASE(size_limits_land_in_wm_normal_hints) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_size_limits(win, {.min_width = 320,
                                          .min_height = 200,
                                          .max_width = 1024,
                                          .max_height = 768});
  h.sync();

  const auto v = h.inspector.u32_property(win, XCB_ATOM_WM_NORMAL_HINTS);
  WW_TEST_CHECK_EQ(v.size(), std::size_t{18});
  WW_TEST_CHECK_EQ(v[0], uint32_t{(1u << 4) | (1u << 5)}); // PMinSize|PMaxSize
  WW_TEST_CHECK_EQ(v[5], uint32_t{320});
  WW_TEST_CHECK_EQ(v[6], uint32_t{200});
  WW_TEST_CHECK_EQ(v[7], uint32_t{1024});
  WW_TEST_CHECK_EQ(v[8], uint32_t{768});
}

WW_TEST_CASE(a_dimension_with_no_limit_is_not_pinned_to_zero) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  // Only a minimum width, only a maximum height: the unset halves must not
  // read as "at most 0 wide" / "at least 0 high" ... and must not turn
  // into a flag for a limit that was never given.
  h.backend->set_window_size_limits(
      win, {.min_width = 300, .min_height = 0, .max_width = 0, .max_height = 500});
  h.sync();

  const auto v = h.inspector.u32_property(win, XCB_ATOM_WM_NORMAL_HINTS);
  WW_TEST_CHECK_EQ(v.size(), std::size_t{18});
  WW_TEST_CHECK_EQ(v[5], uint32_t{300});
  WW_TEST_CHECK_EQ(v[6], uint32_t{0});
  WW_TEST_CHECK(v[7] > 10000u); // unbounded, not zero
  WW_TEST_CHECK_EQ(v[8], uint32_t{500});
}

WW_TEST_CASE(only_a_minimum_sets_only_the_minimum_flag) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_size_limits(win, {.min_width = 50, .min_height = 60});
  h.sync();

  const auto v = h.inspector.u32_property(win, XCB_ATOM_WM_NORMAL_HINTS);
  WW_TEST_CHECK_EQ(v.size(), std::size_t{18});
  WW_TEST_CHECK_EQ(v[0], uint32_t{1u << 4});
}

WW_TEST_CASE(clearing_size_limits_removes_wm_normal_hints) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_size_limits(win, {.min_width = 50, .min_height = 50});
  h.backend->set_window_size_limits(win, {});
  h.sync();

  WW_TEST_CHECK(h.inspector.u32_property(win, XCB_ATOM_WM_NORMAL_HINTS).empty());
}

WW_TEST_CASE(transient_for_names_the_parent_window_and_can_be_cleared) {
  Harness h;
  const uint32_t parent = h.backend->create_window(100, 100);
  const uint32_t child = h.backend->create_window(50, 50);
  h.backend->set_window_transient_for(child, parent);
  h.sync();

  auto v = h.inspector.u32_property(child, XCB_ATOM_WM_TRANSIENT_FOR);
  WW_TEST_CHECK_EQ(v.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(v[0], parent);

  h.backend->set_window_transient_for(child, 0);
  h.sync();
  WW_TEST_CHECK(h.inspector.u32_property(child, XCB_ATOM_WM_TRANSIENT_FOR).empty());
}

WW_TEST_CASE(destroying_a_transient_parent_reparents_its_child_to_the_grandparent) {
  // ICCCM 4.1.2.6: a dialog whose owner disappears should be re-parented
  // to that owner's own owner, not left pointing at a window that no
  // longer exists. grandparent <- parent <- child; destroying parent
  // should leave child transient for grandparent directly.
  Harness h;
  const uint32_t grandparent = h.backend->create_window(200, 200);
  const uint32_t parent = h.backend->create_window(100, 100);
  const uint32_t child = h.backend->create_window(50, 50);
  h.backend->set_window_transient_for(parent, grandparent);
  h.backend->set_window_transient_for(child, parent);
  h.sync();
  WW_TEST_CHECK_EQ(h.inspector.u32_property(child, XCB_ATOM_WM_TRANSIENT_FOR)
                       .at(0),
                   parent);

  h.backend->destroy_window(parent);
  h.sync();

  WW_TEST_CHECK(!h.inspector.window_exists(parent));
  auto v = h.inspector.u32_property(child, XCB_ATOM_WM_TRANSIENT_FOR);
  WW_TEST_CHECK_EQ(v.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(v[0], grandparent);
}

WW_TEST_CASE(destroying_a_root_transient_parent_leaves_its_child_untransient) {
  // Same as above, but the destroyed window had no parent of its own: the
  // child should end up with WM_TRANSIENT_FOR cleared entirely, becoming
  // an ordinary top-level rather than pointing at window id 0.
  Harness h;
  const uint32_t parent = h.backend->create_window(100, 100);
  const uint32_t child = h.backend->create_window(50, 50);
  h.backend->set_window_transient_for(child, parent);
  h.sync();

  h.backend->destroy_window(parent);
  h.sync();

  WW_TEST_CHECK(h.inspector.u32_property(child, XCB_ATOM_WM_TRANSIENT_FOR).empty());
}

WW_TEST_CASE(destroying_an_untransient_window_touches_no_other_window) {
  // A window nobody is transient for should not cause destroy_window to
  // walk off into unrelated bookkeeping — regression guard for the
  // grandparent reparenting loop misfiring on an unrelated window.
  Harness h;
  const uint32_t unrelated_parent = h.backend->create_window(100, 100);
  const uint32_t unrelated_child = h.backend->create_window(50, 50);
  const uint32_t lonely = h.backend->create_window(30, 30);
  h.backend->set_window_transient_for(unrelated_child, unrelated_parent);
  h.sync();

  h.backend->destroy_window(lonely);
  h.sync();

  WW_TEST_CHECK(!h.inspector.window_exists(lonely));
  WW_TEST_CHECK_EQ(
      h.inspector.u32_property(unrelated_child, XCB_ATOM_WM_TRANSIENT_FOR)
          .at(0),
      unrelated_parent);
}

WW_TEST_CASE(client_process_sets_net_wm_pid_and_client_machine) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->set_window_client_process(win, 4242);
  h.sync();

  const auto pid = h.inspector.u32_property(win, h.inspector.atom("_NET_WM_PID"));
  WW_TEST_CHECK_EQ(pid.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(pid[0], uint32_t{4242});

  char host[256] = {};
  ::gethostname(host, sizeof host - 1);
  WW_TEST_CHECK(h.inspector.string_property(win, XCB_ATOM_WM_CLIENT_MACHINE) ==
                std::string(host));
}

// --- child windows (subsurfaces / popups) -----------------------------------

WW_TEST_CASE(child_window_is_a_real_child_of_its_parent) {
  Harness h;
  const uint32_t parent = h.backend->create_window(400, 300);
  h.backend->map_window(parent);
  const uint32_t child = h.backend->create_child_window(parent, 10, 20, 50, 60);
  h.backend->map_window(child);
  h.sync();

  // The whole subsurface design rests on X11 clipping and moving children
  // natively, which only happens if this is genuinely a child in the tree.
  WW_TEST_CHECK_EQ(h.inspector.parent_of(child), parent);
  const auto g = h.inspector.geometry(child);
  WW_TEST_CHECK_EQ(g.x, int16_t{10});
  WW_TEST_CHECK_EQ(g.y, int16_t{20});
  WW_TEST_CHECK_EQ(g.width, uint16_t{50});
  WW_TEST_CHECK_EQ(g.height, uint16_t{60});
}

WW_TEST_CASE(move_window_repositions_a_child_in_parent_coordinates) {
  Harness h;
  const uint32_t parent = h.backend->create_window(400, 300);
  h.backend->map_window(parent);
  const uint32_t child = h.backend->create_child_window(parent, 0, 0, 20, 20);
  h.backend->move_window(child, 33, 44);
  h.sync();

  const auto g = h.inspector.geometry(child);
  WW_TEST_CHECK_EQ(g.x, int16_t{33});
  WW_TEST_CHECK_EQ(g.y, int16_t{44});
}

WW_TEST_CASE(destroying_a_parent_destroys_its_children) {
  Harness h;
  const uint32_t parent = h.backend->create_window(400, 300);
  const uint32_t child = h.backend->create_child_window(parent, 0, 0, 20, 20);
  h.sync();
  WW_TEST_CHECK(h.inspector.window_exists(child));

  h.backend->destroy_window(parent);
  h.sync();

  // This is the server behavior SurfaceManager relies on when it calls
  // forget_window rather than destroy_window on each child.
  WW_TEST_CHECK(!h.inspector.window_exists(child));
}

WW_TEST_CASE(restack_window_places_relative_to_the_named_sibling) {
  Harness h;
  const uint32_t parent = h.backend->create_window(400, 300);
  h.backend->map_window(parent);
  // Three children, so "just above b" and "top of the whole stack" are
  // different answers. With only two siblings the two are indistinguishable
  // and a backend that ignored the sibling argument would still pass.
  const uint32_t a = h.backend->create_child_window(parent, 0, 0, 20, 20);
  const uint32_t b = h.backend->create_child_window(parent, 0, 0, 20, 20);
  const uint32_t c = h.backend->create_child_window(parent, 0, 0, 20, 20);
  h.backend->map_window(a);
  h.backend->map_window(b);
  h.backend->map_window(c);
  h.sync();

  auto kids = h.inspector.children_of(parent); // bottom-to-top
  WW_TEST_CHECK_EQ(kids.size(), std::size_t{3});
  WW_TEST_CHECK_EQ(kids.back(), c);

  h.backend->restack_window(a, b, /*above=*/true);
  h.sync();

  // Expected: b, a, c -- a sits directly above b, still below c.
  kids = h.inspector.children_of(parent);
  WW_TEST_CHECK_EQ(kids.size(), std::size_t{3});
  WW_TEST_CHECK_EQ(kids[0], b);
  WW_TEST_CHECK_EQ(kids[1], a);
  WW_TEST_CHECK_EQ(kids[2], c);
}

WW_TEST_CASE(restack_below_a_sibling_lowers_beneath_it) {
  Harness h;
  const uint32_t parent = h.backend->create_window(400, 300);
  h.backend->map_window(parent);
  const uint32_t a = h.backend->create_child_window(parent, 0, 0, 20, 20);
  const uint32_t b = h.backend->create_child_window(parent, 0, 0, 20, 20);
  const uint32_t c = h.backend->create_child_window(parent, 0, 0, 20, 20);
  h.backend->map_window(a);
  h.backend->map_window(b);
  h.backend->map_window(c);
  h.sync();

  h.backend->restack_window(c, b, /*above=*/false);
  h.sync();

  // Expected: a, c, b -- c drops directly below b but stays above a.
  const auto kids = h.inspector.children_of(parent);
  WW_TEST_CHECK_EQ(kids.size(), std::size_t{3});
  WW_TEST_CHECK_EQ(kids[0], a);
  WW_TEST_CHECK_EQ(kids[1], c);
  WW_TEST_CHECK_EQ(kids[2], b);
}

WW_TEST_CASE(restack_with_no_sibling_raises_to_the_top) {
  Harness h;
  const uint32_t parent = h.backend->create_window(400, 300);
  h.backend->map_window(parent);
  const uint32_t a = h.backend->create_child_window(parent, 0, 0, 20, 20);
  const uint32_t b = h.backend->create_child_window(parent, 0, 0, 20, 20);
  h.backend->map_window(a);
  h.backend->map_window(b);

  h.backend->restack_window(a, 0, /*above=*/true);
  h.sync();

  const auto kids = h.inspector.children_of(parent);
  WW_TEST_CHECK_EQ(kids.back(), a);
}

// --- pointer cursors --------------------------------------------------------

namespace {

// X protocol errors (BadCursor, BadMatch, ...) never break the connection;
// they arrive as error events on it. Fire-and-forget requests such as the
// cursor ones can therefore only be checked by draining those after a sync.
bool no_x_errors(Harness &h) {
  h.sync();
  bool clean = !xcb_connection_has_error(h.backend->connection());
  while (xcb_generic_event_t *ev = xcb_poll_for_event(h.backend->connection())) {
    if (ev->response_type == 0)
      clean = false;
    ::free(ev);
  }
  return clean;
}

// A w*h cursor image: opaque red with a transparent first column, so the
// alpha channel is exercised as well as the colour.
std::vector<uint32_t> test_cursor_pixels(uint16_t w, uint16_t h) {
  std::vector<uint32_t> px(static_cast<std::size_t>(w) * h, 0xffff0000u);
  for (uint16_t y = 0; y < h; ++y)
    px[static_cast<std::size_t>(y) * w] = 0u;
  return px;
}

} // namespace

WW_TEST_CASE(create_cursor_builds_a_server_side_cursor) {
  Harness h;
  const auto px = test_cursor_pixels(24, 24);
  const uint32_t cursor = h.backend->create_cursor(
      ww::CursorImage{.width = 24, .height = 24, .hotspot_x = 3,
                      .hotspot_y = 4, .stride = 24 * 4, .argb = px.data()});
  // Zero means "the server cannot do ARGB cursors" -- a test server without
  // Render would fail here, which is the point: that is the one thing this
  // path cannot work without.
  WW_TEST_CHECK(cursor != 0);
  WW_TEST_CHECK(no_x_errors(h));
}

WW_TEST_CASE(create_cursor_accepts_a_padded_stride) {
  Harness h;
  // Rows wider than the image, as a wl_shm pool with aligned strides gives.
  std::vector<uint32_t> px(static_cast<std::size_t>(32) * 16, 0xff00ff00u);
  const uint32_t cursor = h.backend->create_cursor(
      ww::CursorImage{.width = 16, .height = 16, .hotspot_x = 0,
                      .hotspot_y = 0, .stride = 32 * 4, .argb = px.data()});
  h.sync();

  WW_TEST_CHECK(cursor != 0);
  WW_TEST_CHECK(no_x_errors(h));
}

WW_TEST_CASE(create_cursor_clamps_a_hotspot_outside_the_image) {
  Harness h;
  const auto px = test_cursor_pixels(8, 8);
  // A hotspot off the image is a BadMatch on the wire; it must be pinned to
  // the image instead of killing the connection (or the cursor).
  const uint32_t cursor = h.backend->create_cursor(
      ww::CursorImage{.width = 8, .height = 8, .hotspot_x = 400,
                      .hotspot_y = -5, .stride = 8 * 4, .argb = px.data()});
  h.sync();

  WW_TEST_CHECK(cursor != 0);
  WW_TEST_CHECK(no_x_errors(h));
}

WW_TEST_CASE(create_cursor_refuses_unusable_images) {
  Harness h;
  const auto px = test_cursor_pixels(8, 8);
  // Larger than any server accepts as a cursor.
  const std::vector<uint32_t> big(static_cast<std::size_t>(300) * 300, 0u);
  WW_TEST_CHECK_EQ(h.backend->create_cursor(ww::CursorImage{
                       .width = 300, .height = 300, .stride = 300 * 4,
                       .argb = big.data()}),
                   uint32_t{0});
  // No pixels, no size, and a stride too small for the width.
  WW_TEST_CHECK_EQ(h.backend->create_cursor(ww::CursorImage{
                       .width = 8, .height = 8, .stride = 8 * 4,
                       .argb = nullptr}),
                   uint32_t{0});
  WW_TEST_CHECK_EQ(h.backend->create_cursor(ww::CursorImage{
                       .width = 0, .height = 8, .stride = 0,
                       .argb = px.data()}),
                   uint32_t{0});
  WW_TEST_CHECK_EQ(h.backend->create_cursor(ww::CursorImage{
                       .width = 8, .height = 8, .stride = 4,
                       .argb = px.data()}),
                   uint32_t{0});
  WW_TEST_CHECK(no_x_errors(h));
}

WW_TEST_CASE(set_window_cursor_applies_and_resets_without_errors) {
  Harness h;
  const uint32_t win = h.backend->create_window(64, 64);
  const auto px = test_cursor_pixels(24, 24);
  const uint32_t cursor = h.backend->create_cursor(
      ww::CursorImage{.width = 24, .height = 24, .stride = 24 * 4,
                      .argb = px.data()});
  WW_TEST_CHECK(cursor != 0);

  h.backend->set_window_cursor(win, cursor);
  WW_TEST_CHECK(no_x_errors(h));

  // 0 restores the default (inherit) -- also a legal CWCursor value.
  h.backend->set_window_cursor(win, 0);
  WW_TEST_CHECK(no_x_errors(h));
}

WW_TEST_CASE(a_freed_cursor_may_still_be_showing_on_a_window) {
  Harness h;
  const uint32_t win = h.backend->create_window(64, 64);
  const auto px = test_cursor_pixels(24, 24);
  const uint32_t cursor = h.backend->create_cursor(
      ww::CursorImage{.width = 24, .height = 24, .stride = 24 * 4,
                      .argb = px.data()});
  h.backend->set_window_cursor(win, cursor);

  // The server reference-counts cursors: freeing the handle must not break
  // the window that still shows it, nor any later request on that window.
  h.backend->free_cursor(cursor);
  h.backend->set_window_cursor(win, 0);
  WW_TEST_CHECK(no_x_errors(h));
}

WW_TEST_CASE(blank_cursor_is_created_once_and_never_freed_by_callers) {
  Harness h;
  const uint32_t blank = h.backend->blank_cursor();
  WW_TEST_CHECK(blank != 0);
  WW_TEST_CHECK_EQ(h.backend->blank_cursor(), blank);

  // A caller that frees it by mistake must not leave the next
  // blank_cursor() handing out a dead id.
  h.backend->free_cursor(blank);
  const uint32_t win = h.backend->create_window(16, 16);
  h.backend->set_window_cursor(win, h.backend->blank_cursor());
  h.sync();
  WW_TEST_CHECK_EQ(h.backend->blank_cursor(), blank);
  WW_TEST_CHECK(no_x_errors(h));
}

// --- presentation -----------------------------------------------------------

WW_TEST_CASE(present_pixels_on_a_mapped_window_does_not_error_the_connection) {
  Harness h;
  const uint32_t win = h.backend->create_window(64, 64);
  h.backend->map_window(win);
  h.sync();

  std::vector<uint32_t> pixels(64 * 64, 0xff00ff00u);
  h.backend->present_pixels(win, 64, 64, 64 * 4, pixels.data());
  h.sync();

  // A malformed PutImage or a mismatched stride would put the connection
  // into an unrecoverable error state.
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
}

WW_TEST_CASE(expose_repaints_backend_owned_pixels_not_mutable_client_memory) {
  Harness h;
  const uint32_t win = h.backend->create_window(64, 64);
  h.backend->map_window(win);
  h.sync();

  constexpr uint32_t committed = 0xff22aa44u;
  constexpr uint32_t reused = 0xffcc3311u;
  std::vector<uint32_t> pixels(64 * 64, committed);
  h.backend->present_pixels(win, 64, 64, 64 * 4, pixels.data());
  h.sync();
  WW_TEST_CHECK_EQ(h.inspector.pixels(win, 64, 64).front() & 0x00ffffffu,
                   committed & 0x00ffffffu);

  // After presentation, the client is free to reuse its source storage. An
  // expose must redraw the image WWindows retained at commit time, not read
  // the now-mutated source memory again.
  std::fill(pixels.begin(), pixels.end(), reused);
  h.backend->repaint_window(win);
  h.sync();
  const auto shown = h.inspector.pixels(win, 64, 64);
  WW_TEST_CHECK(!shown.empty());
  WW_TEST_CHECK_EQ(shown.front() & 0x00ffffffu,
                   committed & 0x00ffffffu);
}

// The pair below are the pixel-level regression test for
// xdg_surface.set_window_geometry: what a client with a drop shadow
// attaches, and what has to end up on the window.
//
// They read the window back through the inspector connection rather than
// trusting the backend, because the bug they guard against produced a
// perfectly healthy X11 connection and perfectly wrong pixels: the whole UI
// displaced by the shadow margin, with the opposite edges cropped off
// against the window bounds.

namespace {

constexpr uint16_t kShadowMargin = 8;
constexpr uint16_t kWindowSide = 64;
constexpr uint16_t kBufferSide = kWindowSide + 2 * kShadowMargin;
constexpr uint32_t kShadowPixel = 0x00112233u;
constexpr uint32_t kContentPixel = 0x0044cc88u;

// A buffer laid out the way a client with client-side decorations lays one
// out: a ring of shadow around the window proper.
std::vector<uint32_t> shadowed_buffer() {
  std::vector<uint32_t> buf(
      static_cast<std::size_t>(kBufferSide) * kBufferSide, kShadowPixel);
  for (uint16_t y = kShadowMargin; y < kShadowMargin + kWindowSide; ++y)
    for (uint16_t x = kShadowMargin; x < kShadowMargin + kWindowSide; ++x)
      buf[static_cast<std::size_t>(y) * kBufferSide + x] = kContentPixel;
  return buf;
}

} // namespace

WW_TEST_CASE(present_pixels_puts_the_geometry_rect_at_the_window_origin) {
  Harness h;
  // The window is sized to the geometry, not to the buffer -- exactly what
  // a toplevel gets, since the window manager sizes it from the configure
  // the client was sent.
  const uint32_t win = h.backend->create_window(kWindowSide, kWindowSide);
  h.backend->map_window(win);
  h.sync();

  const std::vector<uint32_t> buffer = shadowed_buffer();
  h.backend->present_pixels(
      win, kBufferSide, kBufferSide, kBufferSide * 4, buffer.data(),
      ww::BufferRegion{static_cast<int16_t>(kShadowMargin),
                       static_cast<int16_t>(kShadowMargin), kWindowSide,
                       kWindowSide});
  h.sync();

  const auto shown = h.inspector.pixels(win, kWindowSide, kWindowSide);
  WW_TEST_CHECK_EQ(shown.size(),
                   static_cast<std::size_t>(kWindowSide) * kWindowSide);

  std::size_t shadow_pixels = 0;
  for (uint32_t px : shown)
    if ((px & 0x00ffffffu) != kContentPixel)
      ++shadow_pixels;

  // Not one pixel of the client's shadow may reach the window.
  WW_TEST_CHECK_EQ(shadow_pixels, std::size_t{0});
}

WW_TEST_CASE(present_pixels_without_a_geometry_rect_starts_at_the_buffer_origin) {
  Harness h;
  const uint32_t win = h.backend->create_window(kWindowSide, kWindowSide);
  h.backend->map_window(win);
  h.sync();

  const std::vector<uint32_t> buffer = shadowed_buffer();
  h.backend->present_pixels(win, kBufferSide, kBufferSide, kBufferSide * 4,
                            buffer.data());
  h.sync();

  const auto shown = h.inspector.pixels(win, kWindowSide, kWindowSide);
  WW_TEST_CHECK(!shown.empty());

  // The control for the test above: with no geometry declared the buffer's
  // own origin lands at the window's, so the top-left corner is shadow.
  // Without this, that test could pass against a readback that sees nothing
  // at all.
  WW_TEST_CHECK_EQ(shown.front() & 0x00ffffffu, kShadowPixel);
}

// present_pixels used a single process-wide scratch SHM segment, reused for
// every window's frame with no synchronization: memcpy the new pixels in,
// fire an xcb_shm_put_image with send_event=0, move on. Two presents to
// *different* windows issued back-to-back -- no sync() between them, so
// nothing yields to the server in between -- write both windows' pixels into
// that same segment before the server has serviced either PutImage. Whichever
// request the server actually reads the segment for sees whatever the last
// writer left behind, so one window ends up showing the other's content.
// Reported symptom: two side-by-side terminals rendering identically, and a
// redraw burst in one corrupting the other after a focus switch.
WW_TEST_CASE(two_windows_presented_back_to_back_do_not_alias_each_others_pixels) {
  Harness h;
  constexpr uint16_t kSide = 64;
  const uint32_t win_a = h.backend->create_window(kSide, kSide);
  const uint32_t win_b = h.backend->create_window(kSide, kSide);
  h.backend->map_window(win_a);
  h.backend->map_window(win_b);
  h.sync();

  constexpr uint32_t kPixelA = 0x00ff0000u; // red
  constexpr uint32_t kPixelB = 0x000000ffu; // blue
  const std::vector<uint32_t> buf_a(static_cast<std::size_t>(kSide) * kSide,
                                    kPixelA);
  const std::vector<uint32_t> buf_b(static_cast<std::size_t>(kSide) * kSide,
                                    kPixelB);

  // No sync() between these two -- that gap is exactly what the bug needs.
  h.backend->present_pixels(win_a, kSide, kSide, kSide * 4, buf_a.data());
  h.backend->present_pixels(win_b, kSide, kSide, kSide * 4, buf_b.data());
  h.sync();

  const auto shown_a = h.inspector.pixels(win_a, kSide, kSide);
  const auto shown_b = h.inspector.pixels(win_b, kSide, kSide);
  WW_TEST_CHECK_EQ(shown_a.size(), static_cast<std::size_t>(kSide) * kSide);
  WW_TEST_CHECK_EQ(shown_b.size(), static_cast<std::size_t>(kSide) * kSide);

  std::size_t a_wrong = 0, b_wrong = 0;
  for (uint32_t px : shown_a)
    if ((px & 0x00ffffffu) != kPixelA)
      ++a_wrong;
  for (uint32_t px : shown_b)
    if ((px & 0x00ffffffu) != kPixelB)
      ++b_wrong;

  WW_TEST_CHECK_EQ(a_wrong, std::size_t{0});
  WW_TEST_CHECK_EQ(b_wrong, std::size_t{0});
}

// The same shape, but a third present lands back on window A: this catches a
// fix that gives every window its own segment but forgets to also guard a
// *second* frame to the same window racing the first frame's own completion.
WW_TEST_CASE(a_third_present_after_two_others_still_shows_its_own_pixels) {
  Harness h;
  constexpr uint16_t kSide = 64;
  const uint32_t win_a = h.backend->create_window(kSide, kSide);
  const uint32_t win_b = h.backend->create_window(kSide, kSide);
  h.backend->map_window(win_a);
  h.backend->map_window(win_b);
  h.sync();

  constexpr uint32_t kPixelA1 = 0x00ff0000u; // red
  constexpr uint32_t kPixelB = 0x000000ffu;  // blue
  constexpr uint32_t kPixelA2 = 0x0000ff00u; // green -- A's second frame
  const std::vector<uint32_t> buf_a1(static_cast<std::size_t>(kSide) * kSide,
                                     kPixelA1);
  const std::vector<uint32_t> buf_b(static_cast<std::size_t>(kSide) * kSide,
                                    kPixelB);
  const std::vector<uint32_t> buf_a2(static_cast<std::size_t>(kSide) * kSide,
                                     kPixelA2);

  h.backend->present_pixels(win_a, kSide, kSide, kSide * 4, buf_a1.data());
  h.backend->present_pixels(win_b, kSide, kSide, kSide * 4, buf_b.data());
  h.backend->present_pixels(win_a, kSide, kSide, kSide * 4, buf_a2.data());
  h.sync();

  const auto shown_a = h.inspector.pixels(win_a, kSide, kSide);
  const auto shown_b = h.inspector.pixels(win_b, kSide, kSide);

  std::size_t a_wrong = 0, b_wrong = 0;
  for (uint32_t px : shown_a)
    if ((px & 0x00ffffffu) != kPixelA2) // A's *last* frame should have won
      ++a_wrong;
  for (uint32_t px : shown_b)
    if ((px & 0x00ffffffu) != kPixelB)
      ++b_wrong;

  WW_TEST_CHECK_EQ(a_wrong, std::size_t{0});
  WW_TEST_CHECK_EQ(b_wrong, std::size_t{0});
}

// Per-window SHM segments (see Impl::window_shm's comment) are real host
// memory, freed explicitly in both destroy_window() and forget_window()
// rather than just dropped from the map -- unlike a GC or a Picture id,
// forgetting to detach one leaks it for the rest of the backend's life.
// These two aren't leak checks (ASan's LeakSanitizer doesn't track SysV
// shared memory), just confirmation that both teardown paths run cleanly,
// with a segment actually allocated, against a real connection.

WW_TEST_CASE(destroying_a_window_after_presenting_to_it_does_not_error) {
  Harness h;
  const uint32_t win = h.backend->create_window(64, 64);
  h.backend->map_window(win);
  h.sync();

  std::vector<uint32_t> pixels(64 * 64, 0xffabcdefu);
  h.backend->present_pixels(win, 64, 64, 64 * 4, pixels.data());
  h.backend->destroy_window(win);
  h.sync();

  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
  WW_TEST_CHECK(!h.inspector.window_exists(win));
}

WW_TEST_CASE(forgetting_a_window_after_presenting_to_it_does_not_error) {
  Harness h;
  // Mirrors how a cascade-destroyed child is actually reached: a real X
  // child of a parent this test destroys directly, same as
  // destroying_a_parent_destroys_its_children above.
  const uint32_t parent = h.backend->create_window(200, 200);
  const uint32_t child = h.backend->create_child_window(parent, 0, 0, 64, 64);
  h.backend->map_window(child);
  h.sync();

  std::vector<uint32_t> pixels(64 * 64, 0xff123456u);
  h.backend->present_pixels(child, 64, 64, 64 * 4, pixels.data());
  h.backend->forget_window(child); // what the parent teardown path calls
  h.backend->destroy_window(parent);
  h.sync();

  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
  WW_TEST_CHECK(!h.inspector.window_exists(child));
}

// --- root properties -------------------------------------------------------

// --- outgoing _NET_WM_MOVERESIZE / WM_CHANGE_STATE requests -----------------
//
// The reverse direction from the _NET_WM_STATE tests below: here the
// backend is the one sending a ClientMessage to the window manager, so
// the inspector connection plays the window manager's part instead,
// selecting SubstructureRedirect on root the same way a real one would
// to receive it. X11 grants that selection exclusively to one client at
// a time, so MoveResizeListener holds it for the shortest possible scope
// and always releases it on destruction (including when a WW_TEST_CHECK
// failure throws and unwinds through it), so it can never strand a later
// test unable to claim it.

namespace {

class MoveResizeListener {
public:
  explicit MoveResizeListener(xcb_connection_t *backend_conn,
                              xcb_window_t root)
      : root_(root) {
    conn_ = xcb_connect(nullptr, nullptr);
    if (!conn_ || xcb_connection_has_error(conn_))
      throw std::runtime_error("moveresize listener could not connect");
    select(XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT);
    // The backend's own request_interactive_move/resize and
    // minimize_window read atoms interned at X11Backend::create() time;
    // nothing here needs to intern anything itself, but do make sure the
    // selection above has round-tripped through the server before the
    // caller sends the request it's waiting for.
    xcb_flush(backend_conn);
  }
  ~MoveResizeListener() {
    if (conn_) {
      select(0);
      xcb_disconnect(conn_);
    }
  }
  MoveResizeListener(const MoveResizeListener &) = delete;
  MoveResizeListener &operator=(const MoveResizeListener &) = delete;

  [[nodiscard]] xcb_atom_t atom(const char *name) {
    auto *reply = xcb_intern_atom_reply(
        conn_,
        xcb_intern_atom(conn_, 0, static_cast<uint16_t>(std::strlen(name)),
                        name),
        nullptr);
    if (!reply)
      return XCB_ATOM_NONE;
    const xcb_atom_t a = reply->atom;
    ::free(reply);
    return a;
  }

  // Waits for the next ClientMessage on root; response_type == 0 on
  // timeout. A blocking wait on the connection's own fd rather than a
  // sleep-poll loop, so a message that arrives immediately (the normal
  // case) is seen with no added latency.
  [[nodiscard]] xcb_client_message_event_t
  wait_for_client_message(int timeout_ms = 2000) {
    const int fd = xcb_get_file_descriptor(conn_);
    const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (xcb_generic_event_t *ev = xcb_poll_for_event(conn_)) {
        std::unique_ptr<xcb_generic_event_t, void (*)(void *)> guard(ev,
                                                                     &::free);
        // Top bit marks a synthetic (xcb_send_event) event, which is
        // exactly what request_interactive_move/resize and
        // minimize_window send.
        if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE)
          return *reinterpret_cast<xcb_client_message_event_t *>(ev);
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return {};
      const auto remaining_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                 now)
              .count();
      struct pollfd pfd {
        fd, POLLIN, 0
      };
      ::poll(&pfd, 1, static_cast<int>(remaining_ms));
    }
  }

private:
  void select(uint32_t mask) {
    xcb_void_cookie_t c = xcb_change_window_attributes_checked(
        conn_, root_, XCB_CW_EVENT_MASK, &mask);
    xcb_generic_error_t *err = xcb_request_check(conn_, c);
    if (err) {
      ::free(err);
      throw std::runtime_error(
          "could not select SubstructureRedirect on root for the "
          "moveresize listener — is another window manager already "
          "running on this Xvfb?");
    }
  }

  xcb_connection_t *conn_{nullptr};
  xcb_window_t root_{0};
};

} // namespace

WW_TEST_CASE(request_interactive_move_sends_net_wm_moveresize) {
  Harness h;
  const uint32_t win = h.backend->create_window(200, 150);
  h.sync();
  MoveResizeListener wm(h.backend->connection(), h.root());

  h.backend->request_interactive_move(win);
  const auto ev = wm.wait_for_client_message();

  WW_TEST_CHECK_EQ(ev.response_type & 0x7fu, uint32_t{XCB_CLIENT_MESSAGE});
  WW_TEST_CHECK_EQ(ev.window, win);
  WW_TEST_CHECK_EQ(ev.type, wm.atom("_NET_WM_MOVERESIZE"));
  WW_TEST_CHECK_EQ(ev.format, uint8_t{32});
  // direction: _NET_WM_MOVERESIZE_MOVE
  WW_TEST_CHECK_EQ(ev.data.data32[2], uint32_t{8});
}

WW_TEST_CASE(request_interactive_resize_sends_the_requested_direction) {
  Harness h;
  const uint32_t win = h.backend->create_window(200, 150);
  h.sync();
  MoveResizeListener wm(h.backend->connection(), h.root());

  h.backend->request_interactive_resize(win, ww::ResizeEdge::TopLeft);
  const auto ev = wm.wait_for_client_message();

  WW_TEST_CHECK_EQ(ev.window, win);
  WW_TEST_CHECK_EQ(ev.type, wm.atom("_NET_WM_MOVERESIZE"));
  // direction: _NET_WM_MOVERESIZE_SIZE_TOPLEFT
  WW_TEST_CHECK_EQ(ev.data.data32[2], uint32_t{0});
}

WW_TEST_CASE(request_interactive_resize_reports_the_live_pointer_position) {
  Harness h;
  const uint32_t win = h.backend->create_window(200, 150);
  h.sync();

  // Plant the pointer somewhere identifiable before asking, since
  // request_interactive_resize is documented to query it live rather than
  // reconstruct it from event history.
  xcb_warp_pointer(h.inspector.conn(), XCB_NONE, h.root(), 0, 0, 0, 0, 77,
                   55);
  xcb_flush(h.inspector.conn());
  h.sync();

  MoveResizeListener wm(h.backend->connection(), h.root());
  h.backend->request_interactive_resize(win, ww::ResizeEdge::BottomRight);
  const auto ev = wm.wait_for_client_message();

  WW_TEST_CHECK_EQ(ev.window, win);
  WW_TEST_CHECK_EQ(static_cast<int16_t>(ev.data.data32[0]), int16_t{77});
  WW_TEST_CHECK_EQ(static_cast<int16_t>(ev.data.data32[1]), int16_t{55});
}

WW_TEST_CASE(minimize_window_sends_wm_change_state_iconic) {
  Harness h;
  const uint32_t win = h.backend->create_window(200, 150);
  h.sync();
  MoveResizeListener wm(h.backend->connection(), h.root());

  h.backend->minimize_window(win);
  const auto ev = wm.wait_for_client_message();

  WW_TEST_CHECK_EQ(ev.response_type & 0x7fu, uint32_t{XCB_CLIENT_MESSAGE});
  WW_TEST_CHECK_EQ(ev.window, win);
  WW_TEST_CHECK_EQ(ev.type, wm.atom("WM_CHANGE_STATE"));
  // IconicState, per ICCCM 4.1.4.
  WW_TEST_CHECK_EQ(ev.data.data32[0], uint32_t{3});
}

WW_TEST_CASE(move_resize_and_minimize_on_window_zero_send_nothing) {
  // window id 0 can never be a real toplevel; guards against a stray
  // request (e.g. a surface with no backing window yet) turning into a
  // ClientMessage naming the root window's own child slot 0.
  Harness h;
  MoveResizeListener wm(h.backend->connection(), h.root());

  h.backend->request_interactive_move(0);
  h.backend->request_interactive_resize(0, ww::ResizeEdge::Top);
  h.backend->minimize_window(0);
  h.sync();

  const auto ev = wm.wait_for_client_message(/*timeout_ms=*/300);
  WW_TEST_CHECK_EQ(ev.response_type, uint8_t{0}); // nothing arrived
}

// --- generic (EWMH) _NET_WM_STATE ------------------------------------------
//
// These stand in for a real window manager: the inspector connection sets
// _NET_WM_STATE on the toplevel exactly as one would (ChangeProperty of
// ATOM-typed values), which the X server delivers back to the backend's own
// connection as a PropertyNotify because create_window() asked for
// PROPERTY_CHANGE on toplevels.

namespace {

using WmState = ww::IWindowBackend::WmWindowState;

struct WmStateRecorder {
  std::vector<std::pair<uint32_t, WmState>> seen;
};

void set_net_wm_state(Harness &h, uint32_t win,
                      const std::vector<xcb_atom_t> &atoms) {
  xcb_change_property(h.inspector.conn(), XCB_PROP_MODE_REPLACE, win,
                      h.inspector.atom("_NET_WM_STATE"), XCB_ATOM_ATOM, 32,
                      static_cast<uint32_t>(atoms.size()),
                      atoms.empty() ? nullptr : atoms.data());
  xcb_flush(h.inspector.conn());
}

} // namespace

WW_TEST_CASE(net_wm_state_maximized_both_axes_is_reported) {
  Harness h;
  WmStateRecorder r;
  h.backend->set_wm_state_callback(
      [&](uint32_t win, WmState s) { r.seen.emplace_back(win, s); });

  const uint32_t win = h.backend->create_window(400, 300);
  h.backend->map_window(win);
  h.sync();

  set_net_wm_state(h, win,
                   {h.inspector.atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                    h.inspector.atom("_NET_WM_STATE_MAXIMIZED_HORZ")});
  WW_TEST_CHECK(h.pump_until([&] { return !r.seen.empty(); }));
  if (r.seen.empty())
    return;
  WW_TEST_CHECK_EQ(r.seen.back().first, win);
  WW_TEST_CHECK(r.seen.back().second.maximized_horz);
  WW_TEST_CHECK(r.seen.back().second.maximized_vert);
  WW_TEST_CHECK(!r.seen.back().second.fullscreen);
}

WW_TEST_CASE(net_wm_state_fullscreen_is_reported_separately) {
  Harness h;
  WmStateRecorder r;
  h.backend->set_wm_state_callback(
      [&](uint32_t win, WmState s) { r.seen.emplace_back(win, s); });

  const uint32_t win = h.backend->create_window(400, 300);
  h.backend->map_window(win);
  h.sync();

  set_net_wm_state(h, win, {h.inspector.atom("_NET_WM_STATE_FULLSCREEN")});
  WW_TEST_CHECK(h.pump_until([&] { return !r.seen.empty(); }));
  if (r.seen.empty())
    return;
  WW_TEST_CHECK(r.seen.back().second.fullscreen);
  WW_TEST_CHECK(!r.seen.back().second.maximized_horz);
}

WW_TEST_CASE(net_wm_state_removed_reports_all_clear) {
  Harness h;
  WmStateRecorder r;
  h.backend->set_wm_state_callback(
      [&](uint32_t win, WmState s) { r.seen.emplace_back(win, s); });

  const uint32_t win = h.backend->create_window(400, 300);
  h.backend->map_window(win);
  h.sync();

  set_net_wm_state(h, win,
                   {h.inspector.atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                    h.inspector.atom("_NET_WM_STATE_MAXIMIZED_HORZ")});
  WW_TEST_CHECK(h.pump_until([&] { return !r.seen.empty(); }));
  r.seen.clear();

  xcb_delete_property(h.inspector.conn(), win,
                      h.inspector.atom("_NET_WM_STATE"));
  xcb_flush(h.inspector.conn());
  WW_TEST_CHECK(h.pump_until([&] { return !r.seen.empty(); }));
  if (r.seen.empty())
    return;
  WW_TEST_CHECK(!r.seen.back().second.maximized_horz);
  WW_TEST_CHECK(!r.seen.back().second.maximized_vert);
  WW_TEST_CHECK(!r.seen.back().second.fullscreen);
}

WW_TEST_CASE(net_wm_state_on_a_different_window_is_not_reported) {
  Harness h;
  WmStateRecorder r;
  h.backend->set_wm_state_callback(
      [&](uint32_t win, WmState s) { r.seen.emplace_back(win, s); });

  const uint32_t win_a = h.backend->create_window(200, 200);
  const uint32_t win_b = h.backend->create_window(200, 200);
  h.backend->map_window(win_a);
  h.backend->map_window(win_b);
  h.sync();

  set_net_wm_state(h, win_a,
                   {h.inspector.atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                    h.inspector.atom("_NET_WM_STATE_MAXIMIZED_HORZ")});
  WW_TEST_CHECK(h.pump_until([&] { return !r.seen.empty(); }));
  for (const auto &[win, state] : r.seen)
    WW_TEST_CHECK_EQ(win, win_a);
  (void)win_b;
}

WW_TEST_CASE(root_string_property_reads_what_a_window_manager_published) {
  Harness h;
  // What i3 does at startup: put its IPC socket path on the root window.
  const xcb_atom_t atom = h.inspector.atom("WW_TEST_SOCKET_PATH");
  const std::string path = "/run/user/1000/i3/ipc-socket.1234";
  xcb_change_property(h.inspector.conn(), XCB_PROP_MODE_REPLACE,
                      h.backend->screen()->root, atom, XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(path.size()), path.c_str());
  xcb_flush(h.inspector.conn());
  h.sync();

  const auto got = h.backend->root_string_property("WW_TEST_SOCKET_PATH");
  WW_TEST_CHECK(got.has_value());
  if (got)
    WW_TEST_CHECK(*got == path);
}

WW_TEST_CASE(root_string_property_is_empty_when_nobody_set_it) {
  Harness h;
  // Never interned on this server, so the "atom does not exist" path.
  WW_TEST_CHECK(!h.backend->root_string_property("WW_TEST_NEVER_SET_ANYWHERE").has_value());

  // Interned but with no value on the root window.
  (void)h.inspector.atom("WW_TEST_INTERNED_BUT_UNSET");
  WW_TEST_CHECK(!h.backend->root_string_property("WW_TEST_INTERNED_BUT_UNSET").has_value());
}

WW_TEST_CASE(import_dmabuf_without_dri3_reports_failure_exactly_once) {
  Harness h;
  const uint32_t win = h.backend->create_window(64, 64);
  h.sync();

  int calls = 0;
  uint32_t got = 12345;
  // fd -1 is deliberately invalid: on a server without DRI3 the import
  // cannot succeed, and the contract says the callback still fires exactly
  // once, with 0.
  h.backend->import_dmabuf(-1, win, 64, 64, 24, 32, 256, 0, false,
                           [&](uint32_t pixmap) {
                             ++calls;
                             got = pixmap;
                           });
  h.sync();

  WW_TEST_CHECK_EQ(calls, 1);
  WW_TEST_CHECK_EQ(got, uint32_t{0});
}

// --- input shape ------------------------------------------------------------

WW_TEST_CASE(set_input_shape_does_not_error_the_connection) {
  Harness h;
  const uint32_t win = h.backend->create_window(100, 100);
  h.backend->map_window(win);

  h.backend->set_input_shape(win, true, {{0, 0, 50, 50}}, {{10, 10, 5, 5}});
  h.sync();
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));

  // has_region == false means "no region was ever declared": reset to the
  // whole bounding rectangle.
  h.backend->set_input_shape(win, false, {}, {});
  h.sync();
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));

  // has_region == true with empty rects is a *different* request (an
  // explicitly empty region) and must not error either.
  h.backend->set_input_shape(win, true, {}, {});
  h.sync();
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
}

// Regression test for the Firefox/subsurface bug: a client explicitly
// declaring an empty input region (has_region == true, zero rects) — the
// standard idiom for a purely-visual overlay subsurface that wants clicks
// and keys to pass through to whatever is beneath it — must end up with a
// genuinely empty input shape, distinguishable from "no region was ever
// declared" at all. Verified through the independent Inspector connection,
// not just "the call didn't error", per this suite's own convention.
WW_TEST_CASE(an_explicitly_empty_region_produces_a_genuinely_empty_shape) {
  Harness h;
  const uint32_t never_set = h.backend->create_window(100, 100);
  const uint32_t explicitly_empty = h.backend->create_window(100, 100);
  h.backend->map_window(never_set);
  h.backend->map_window(explicitly_empty);

  // never_set: has_region == false, exactly as a surface that never called
  // wl_surface.set_input_region at all.
  h.backend->set_input_shape(never_set, false, {}, {});
  // explicitly_empty: has_region == true with zero rects, exactly as a
  // surface whose set_input_region rects add up to nothing.
  h.backend->set_input_shape(explicitly_empty, true, {}, {});
  h.sync();

  Inspector insp;
  const auto never_set_rects = insp.input_shape_rects(never_set);
  const auto explicitly_empty_rects = insp.input_shape_rects(explicitly_empty);

  // "Never declared" accepts input over the whole window: one rectangle
  // covering it.
  WW_TEST_CHECK_EQ(never_set_rects.size(), std::size_t{1});

  // "Declared and empty" accepts no input anywhere: zero rectangles. Before
  // the fix, this collapsed to the same one-rectangle whole-window shape as
  // never_set above.
  WW_TEST_CHECK_EQ(explicitly_empty_rects.size(), std::size_t{0});
}

// --- clipboard --------------------------------------------------------------

WW_TEST_CASE(set_clipboard_owner_takes_the_x11_selection) {
  Harness h;
  h.backend->set_clipboard_owner({"text/plain;charset=utf-8"});
  h.sync();

  const xcb_atom_t clipboard = h.inspector.atom("CLIPBOARD");
  auto *reply = xcb_get_selection_owner_reply(
      h.inspector.conn(),
      xcb_get_selection_owner(h.inspector.conn(), clipboard), nullptr);
  WW_TEST_CHECK(reply != nullptr);
  const xcb_window_t owner = reply ? reply->owner : 0;
  ::free(reply);

  // A native X11 application pastes by asking whoever owns CLIPBOARD, so
  // ownership has to actually land on the server.
  WW_TEST_CHECK(owner != XCB_NONE);
}

WW_TEST_CASE(empty_clipboard_owner_releases_the_selection) {
  Harness h;
  h.backend->set_clipboard_owner({"text/plain"});
  h.sync();
  h.backend->set_clipboard_owner({});
  h.sync();

  const xcb_atom_t clipboard = h.inspector.atom("CLIPBOARD");
  auto *reply = xcb_get_selection_owner_reply(
      h.inspector.conn(),
      xcb_get_selection_owner(h.inspector.conn(), clipboard), nullptr);
  const xcb_window_t owner = reply ? reply->owner : 0;
  ::free(reply);

  WW_TEST_CHECK_EQ(owner, static_cast<xcb_window_t>(XCB_NONE));
}

WW_TEST_CASE(binary_only_mime_types_still_take_ownership) {
  Harness h;
  // Clipboard bridging is text-only by policy, but taking ownership is not
  // conditional on a text type being present -- the offer simply exposes no
  // text targets.
  h.backend->set_clipboard_owner({"image/png"});
  h.sync();
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
}

// --- root-window property watches ------------------------------------------
//
// watch_root_property exists for the i3-socket-discovery startup race:
// application.cpp reads I3_SOCKET_PATH off the root window once at its own
// startup, but i3 sets that property from its own independent startup
// sequence -- launch this process from i3's own config and the two are
// directly racing each other. These simulate "another process sets a root
// property later" from the independent inspector connection, exactly as a
// real window manager would, entirely separate from the backend's own.

WW_TEST_CASE(watch_root_property_fires_when_another_client_sets_it) {
  Harness h;
  const xcb_window_t root =
      xcb_setup_roots_iterator(xcb_get_setup(h.inspector.conn())).data->root;

  bool fired = false;
  h.backend->watch_root_property(
      "WWINDOWS_TEST_ROOT_PROP",
      [&fired](std::optional<std::string>) { fired = true; });

  xcb_intern_atom_reply_t *atom_reply = xcb_intern_atom_reply(
      h.inspector.conn(),
      xcb_intern_atom(h.inspector.conn(), /*only_if_exists=*/0, 23,
                      "WWINDOWS_TEST_ROOT_PROP"),
      nullptr);
  WW_TEST_CHECK(atom_reply != nullptr);
  if (!atom_reply)
    return;
  const xcb_atom_t atom = atom_reply->atom;
  ::free(atom_reply);

  const char *value = "some value";
  xcb_change_property(h.inspector.conn(), XCB_PROP_MODE_REPLACE, root, atom,
                      XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(std::strlen(value)), value);
  xcb_flush(h.inspector.conn());

  WW_TEST_CHECK(h.pump_until([&fired] { return fired; }));
}

WW_TEST_CASE(a_watched_propertys_new_value_is_delivered_to_the_callback) {
  // The actual application.cpp pattern: a synchronous read at startup that
  // may come back empty, backstopped by a watch that retries once the
  // property genuinely appears -- e.g. i3 setting I3_SOCKET_PATH after this
  // process already looked and found nothing. The callback is handed the
  // new value directly, with no redundant root_string_property() call of
  // its own needed to find out what it is.
  Harness h;
  const xcb_window_t root =
      xcb_setup_roots_iterator(xcb_get_setup(h.inspector.conn())).data->root;

  WW_TEST_CHECK(!h.backend->root_string_property("WWINDOWS_TEST_ROOT_PROP2"));

  std::optional<std::string> seen;
  bool seen_at_least_once = false;
  h.backend->watch_root_property(
      "WWINDOWS_TEST_ROOT_PROP2", [&](std::optional<std::string> value) {
        seen = std::move(value);
        seen_at_least_once = true;
      });

  xcb_intern_atom_reply_t *atom_reply = xcb_intern_atom_reply(
      h.inspector.conn(),
      xcb_intern_atom(h.inspector.conn(), /*only_if_exists=*/0, 24,
                      "WWINDOWS_TEST_ROOT_PROP2"),
      nullptr);
  WW_TEST_CHECK(atom_reply != nullptr);
  if (!atom_reply)
    return;
  const xcb_atom_t atom = atom_reply->atom;
  ::free(atom_reply);

  const char *value = "/tmp/some.sock";
  xcb_change_property(h.inspector.conn(), XCB_PROP_MODE_REPLACE, root, atom,
                      XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(std::strlen(value)), value);
  xcb_flush(h.inspector.conn());

  WW_TEST_CHECK(h.pump_until([&seen] { return seen.has_value(); }));
  WW_TEST_CHECK_EQ(seen.value_or(""), std::string("/tmp/some.sock"));

  // Deleting the property delivers std::nullopt rather than leaving the
  // callback to guess -- indistinguishable, without this, from "unreadable"
  // (format != 8), which root_string_property() also reports as nullopt.
  seen = "unchanged"; // distinct from both nullopt and the value above
  seen_at_least_once = false;
  xcb_delete_property(h.inspector.conn(), root, atom);
  xcb_flush(h.inspector.conn());

  WW_TEST_CHECK(h.pump_until([&seen_at_least_once] { return seen_at_least_once; }));
  WW_TEST_CHECK(!seen.has_value());
}

// --- teardown ---------------------------------------------------------------

WW_TEST_CASE(backend_destruction_closes_the_connection_cleanly) {
  keep_server_alive();
  Inspector inspector;
  uint32_t win = 0;
  {
    auto loop = ww::EventLoop::create();
    WW_TEST_CHECK(loop.has_value());
    auto backend = ww::X11Backend::create(**loop);
    WW_TEST_CHECK(backend.has_value());
    win = (*backend)->create_window(50, 50);
    xcb_flush((*backend)->connection());
  } // backend destroyed here

  // Closing a connection destroys every resource it created, so the window
  // must be gone without anyone having called destroy_window.
  auto *reply = xcb_get_input_focus_reply(
      inspector.conn(), xcb_get_input_focus(inspector.conn()), nullptr);
  ::free(reply);
  WW_TEST_CHECK(!inspector.window_exists(win));
}

// --- keyboard auto-repeat (XKB controls) --------------------------------------

namespace {
// Change the server's keyboard repeat the way `xset r rate` does, from a
// connection that is not the backend's own.
void set_server_repeat(Harness &h, bool enabled, uint16_t delay_ms,
                       uint16_t interval_ms) {
  xcb_connection_t *c = h.inspector.conn();
  ::free(xcb_xkb_use_extension_reply(c, xcb_xkb_use_extension(c, 1, 0),
                                     nullptr));
  const uint8_t per_key_repeat[32] = {};
  xcb_xkb_set_controls(
      c, XCB_XKB_ID_USE_CORE_KBD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      XCB_XKB_BOOL_CTRL_REPEAT_KEYS,
      enabled ? static_cast<uint32_t>(XCB_XKB_BOOL_CTRL_REPEAT_KEYS) : 0u,
      XCB_XKB_BOOL_CTRL_REPEAT_KEYS, delay_ms, interval_ms, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, per_key_repeat);
  h.sync();
}
} // namespace

WW_TEST_CASE(keyboard_repeat_reports_what_the_server_has_configured) {
  Harness h;
  const auto original = h.backend->keyboard_repeat();
  if (!original) { // no XKB on this server: the port says so rather than guess
    WW_TEST_CHECK(true);
    return;
  }

  set_server_repeat(h, true, 300, 20);
  auto reported = h.backend->keyboard_repeat();
  WW_TEST_CHECK(reported.has_value());
  if (reported) {
    WW_TEST_CHECK_EQ(reported->rate_hz, uint32_t{50}); // 1000 ms / 20 ms
    WW_TEST_CHECK_EQ(reported->delay_ms, uint32_t{300});
  }

  // Repeat switched off is a rate of zero, which is how wl_keyboard.repeat_info
  // says "do not repeat".
  set_server_repeat(h, false, 300, 20);
  reported = h.backend->keyboard_repeat();
  WW_TEST_CHECK(reported.has_value());
  if (reported)
    WW_TEST_CHECK_EQ(reported->rate_hz, uint32_t{0});

  // Leave the shared server as found.
  set_server_repeat(h, original->rate_hz != 0,
                    static_cast<uint16_t>(original->delay_ms),
                    static_cast<uint16_t>(original->rate_hz
                                              ? 1000u / original->rate_hz
                                              : 40u));
}

WW_TEST_CASE(a_change_to_the_servers_repeat_setting_reaches_the_callback) {
  Harness h;
  const auto original = h.backend->keyboard_repeat();
  if (!original) {
    WW_TEST_CHECK(true);
    return;
  }

  bool fired = false;
  ww::IWindowBackend::InputCallbacks cbs;
  cbs.on_xkb_repeat_changed = [&fired] { fired = true; };
  h.backend->set_input_callbacks(std::move(cbs));

  set_server_repeat(h, true, 450, 33);
  WW_TEST_CHECK(h.pump_until([&] { return fired; }));

  set_server_repeat(h, original->rate_hz != 0,
                    static_cast<uint16_t>(original->delay_ms),
                    static_cast<uint16_t>(original->rate_hz
                                              ? 1000u / original->rate_hz
                                              : 40u));
}

// --- the screen (RandR) ---------------------------------------------------------

namespace {
struct RootGeometry {
  int32_t w{0}, h{0}, mm_w{0}, mm_h{0};
};

// The server's current root size, asked the authoritative way (not from the
// connection setup data, which goes stale the moment RandR changes anything).
RootGeometry server_root_size(Harness &h) {
  xcb_connection_t *c = h.inspector.conn();
  xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  auto *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, screen->root), nullptr);
  RootGeometry out;
  if (g) {
    out.w = g->width;
    out.h = g->height;
    ::free(g);
  }
  auto *info = xcb_randr_get_screen_info_reply(
      c, xcb_randr_get_screen_info(c, screen->root), nullptr);
  (void)info;
  ::free(info);
  return out;
}

// A real resolution change on the throwaway server, the way `xrandr --mode`
// does it: a new mode is defined, the (only) output is switched to it, and the
// screen is resized to fit. switch_to() returns false (and changes nothing) if
// this X server cannot do it, so a test can skip rather than fail on one.
struct ScreenChanger {
  Harness &h;
  xcb_connection_t *c;
  xcb_window_t root{0};
  xcb_randr_output_t output{0};
  xcb_randr_crtc_t crtc{0};
  xcb_randr_mode_t original_mode{0};
  uint16_t original_w{0}, original_h{0};
  uint32_t original_mm_w{0}, original_mm_h{0};
  xcb_randr_mode_t new_mode{0};
  bool changed{false};

  explicit ScreenChanger(Harness &harness)
      : h(harness), c(harness.inspector.conn()) {
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    root = screen->root;
    original_w = screen->width_in_pixels;
    original_h = screen->height_in_pixels;
    original_mm_w = screen->width_in_millimeters;
    original_mm_h = screen->height_in_millimeters;
  }

  [[nodiscard]] bool switch_to(uint16_t w, uint16_t h_px, uint32_t mm_w,
                               uint32_t mm_h) {
    ::free(xcb_randr_query_version_reply(c, xcb_randr_query_version(c, 1, 2),
                                         nullptr));
    auto *res = xcb_randr_get_screen_resources_current_reply(
        c, xcb_randr_get_screen_resources_current(c, root), nullptr);
    if (!res)
      return false;
    const bool usable = res->num_crtcs > 0 && res->num_outputs > 0;
    if (usable) {
      crtc = xcb_randr_get_screen_resources_current_crtcs(res)[0];
      output = xcb_randr_get_screen_resources_current_outputs(res)[0];
    }
    ::free(res);
    if (!usable)
      return false;

    auto *info = xcb_randr_get_crtc_info_reply(
        c, xcb_randr_get_crtc_info(c, crtc, XCB_CURRENT_TIME), nullptr);
    if (!info)
      return false;
    original_mode = info->mode;
    const xcb_timestamp_t config_time = info->timestamp;
    ::free(info);

    // 60 Hz: 81.62 MHz dot clock over a (w+368) x (h+31) total frame is only
    // exactly 60 Hz at 1152x864; close enough elsewhere for a range check.
    static const char name[] = "wwtest";
    xcb_randr_mode_info_t mode{};
    mode.width = w;
    mode.height = h_px;
    mode.dot_clock = 81620000;
    mode.hsync_start = static_cast<uint16_t>(w + 64);
    mode.hsync_end = static_cast<uint16_t>(w + 184);
    mode.htotal = static_cast<uint16_t>(w + 368);
    mode.vsync_start = static_cast<uint16_t>(h_px + 4);
    mode.vsync_end = static_cast<uint16_t>(h_px + 7);
    mode.vtotal = static_cast<uint16_t>(h_px + 31);
    mode.name_len = sizeof(name) - 1;
    auto *created = xcb_randr_create_mode_reply(
        c, xcb_randr_create_mode(c, root, mode, mode.name_len, name), nullptr);
    if (!created)
      return false;
    new_mode = created->mode;
    ::free(created);

    xcb_generic_error_t *err = xcb_request_check(
        c, xcb_randr_add_output_mode_checked(c, output, new_mode));
    if (err) {
      ::free(err);
      return false;
    }
    auto *set = xcb_randr_set_crtc_config_reply(
        c,
        xcb_randr_set_crtc_config(c, crtc, XCB_CURRENT_TIME, config_time, 0, 0,
                                  new_mode, XCB_RANDR_ROTATION_ROTATE_0, 1,
                                  &output),
        nullptr);
    const bool ok = set && set->status == XCB_RANDR_SET_CONFIG_SUCCESS;
    ::free(set);
    if (!ok)
      return false;
    changed = true;
    xcb_randr_set_screen_size(c, root, w, h_px, mm_w, mm_h);
    h.sync();
    return true;
  }

  void restore() {
    if (!changed)
      return;
    changed = false;
    // Grow the screen first so the output never extends past it.
    xcb_randr_set_screen_size(c, root, original_w, original_h, original_mm_w,
                              original_mm_h);
    auto *info = xcb_randr_get_crtc_info_reply(
        c, xcb_randr_get_crtc_info(c, crtc, XCB_CURRENT_TIME), nullptr);
    const xcb_timestamp_t config_time = info ? info->timestamp : XCB_CURRENT_TIME;
    ::free(info);
    ::free(xcb_randr_set_crtc_config_reply(
        c,
        xcb_randr_set_crtc_config(c, crtc, XCB_CURRENT_TIME, config_time, 0, 0,
                                  original_mode, XCB_RANDR_ROTATION_ROTATE_0, 1,
                                  &output),
        nullptr));
    xcb_randr_delete_output_mode(c, output, new_mode);
    xcb_randr_destroy_mode(c, new_mode);
    h.sync();
  }

  ~ScreenChanger() { restore(); }
};
} // namespace

WW_TEST_CASE(screen_geometry_matches_the_server) {
  Harness h;
  const auto g = h.backend->screen_geometry();
  const RootGeometry server = server_root_size(h);
  WW_TEST_CHECK_EQ(g.width_px, server.w);
  WW_TEST_CHECK_EQ(g.height_px, server.h);
}

WW_TEST_CASE(a_randr_screen_change_reaches_the_callback) {
  Harness h;
  const auto before = h.backend->screen_geometry();

  bool fired = false;
  ww::X11Backend::ScreenGeometry seen;
  h.backend->set_screen_changed_callback(
      [&](const ww::X11Backend::ScreenGeometry &g) {
        fired = true;
        seen = g;
      });

  ScreenChanger changer(h);
  const uint16_t new_w = static_cast<uint16_t>(before.width_px - 128);
  const uint16_t new_h = static_cast<uint16_t>(before.height_px - 96);
  if (!changer.switch_to(new_w, new_h, 300, 200)) {
    // This X server cannot change modes (no RandR 1.2 output): nothing to
    // observe. The backend's tracking is still covered by the getter test.
    WW_TEST_CHECK(true);
    return;
  }

  // The server sends a notification for the CRTC change and another once the
  // screen has been resized; wait for the one that carries the new size.
  WW_TEST_CHECK(h.pump_until(
      [&] { return fired && seen.width_px == static_cast<int32_t>(new_w); }));
  WW_TEST_CHECK_EQ(seen.width_px, static_cast<int32_t>(new_w));
  WW_TEST_CHECK_EQ(seen.height_px, static_cast<int32_t>(new_h));
  WW_TEST_CHECK_EQ(seen.width_mm, int32_t{300});
  WW_TEST_CHECK_EQ(seen.height_mm, int32_t{200});
  // The mode that was created runs at about 60 Hz.
  WW_TEST_CHECK(seen.refresh_mhz >= 55000 && seen.refresh_mhz <= 65000);
  WW_TEST_CHECK(h.backend->screen_geometry() == seen);

  // Putting it back is a change too, and is reported.
  fired = false;
  changer.restore();
  WW_TEST_CHECK(h.pump_until(
      [&] { return fired && seen.width_px == before.width_px; }));
  WW_TEST_CHECK_EQ(seen.width_px, before.width_px);
  WW_TEST_CHECK_EQ(seen.height_px, before.height_px);
}
