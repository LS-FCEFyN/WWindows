// Behavioral tests for the input translation layer.
//
// InputTranslator turns raw backend input callbacks into Wayland pointer and
// keyboard events. It depends only on IWindowBackend and the narrow
// ISurfaceInteractions port, so both can be faked -- no X server, no real
// client, no compositor state machine.
//
// Events are asserted by reading the bytes the compositor actually wrote to
// the client socket. wl_pointer.enter has no observable effect on any API
// this process can reach, so the wire is the only honest place to look.

#include "../fakes/fake_surface_interactions.hpp"
#include "../fakes/fake_window_backend.hpp"
#include "../fakes/wayland_fixture.hpp"
#include "../support/test.hpp"

#include <wwindows/input/input_translator.hpp>

#include <wayland-server-protocol.h>

#include <algorithm>
#include <cstring>

using ww_test::FakeSurfaceInteractions;
using ww_test::FakeWindowBackend;
using ww_test::WaylandFixture;

namespace {

// wl_pointer event opcodes (wayland.xml).
constexpr uint16_t kPointerEnter = 0;
constexpr uint16_t kPointerLeave = 1;
constexpr uint16_t kPointerMotion = 2;
constexpr uint16_t kPointerButton = 3;
constexpr uint16_t kPointerAxis = 4;

// wl_keyboard event opcodes.
constexpr uint16_t kKeyboardEnter = 1;
constexpr uint16_t kKeyboardRepeatInfo = 5;
constexpr uint16_t kKeyboardLeave = 2;
constexpr uint16_t kKeyboardKey = 3;
constexpr uint16_t kKeyboardModifiersOpcode = 4;

constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;

constexpr uint32_t kWindow = 42;

// Mirrors the production destruction order in src/application.cpp:
// InputTranslator is destroyed before the backend, and the wl_display before
// the translator, because focus surfaces carry destroy listeners pointing
// into Impl. Declaring the fixture last makes it die first.
struct Harness {
  FakeWindowBackend backend;
  FakeSurfaceInteractions surfaces;
  ww::InputTranslator input{backend, surfaces};
  WaylandFixture wl; // destroyed first

  wl_resource *surface{nullptr};
  wl_resource *pointer{nullptr};
  wl_resource *keyboard{nullptr};

  Harness() {
    input.set_display(wl.display());
    surface = wl.surface();
    surfaces.bind(kWindow, surface);
    pointer = wl.pointer();
    keyboard = wl.keyboard();
    input.add_pointer(pointer);
    input.add_keyboard(keyboard);
  }

  // The backend delivers input by invoking the callbacks InputTranslator
  // registered in its constructor -- exactly how X11Backend drives it.
  void enter(uint32_t win = kWindow, int16_t x = 5, int16_t y = 6,
             int16_t root_x = 105, int16_t root_y = 106) {
    backend.input_cbs.on_enter(win, x, y, root_x, root_y);
  }
  void leave(uint32_t win = kWindow) { backend.input_cbs.on_leave(win); }
  // Real X11 keyboard focus arriving/leaving -- see
  // IWindowBackend::InputCallbacks::on_focus_in's doc comment. Deliberately
  // separate from enter()/leave(), which now drive *pointer* focus only.
  void focus_in(uint32_t win = kWindow) { backend.input_cbs.on_focus_in(win); }
  void focus_out(uint32_t win = kWindow) {
    backend.input_cbs.on_focus_out(win);
  }
  void motion(int16_t x, int16_t y, int16_t root_x = 0, int16_t root_y = 0,
              uint32_t time = 1000) {
    backend.input_cbs.on_motion(kWindow, x, y, root_x, root_y, time);
  }
  void button(uint8_t btn, bool pressed, uint32_t time = 2000) {
    backend.input_cbs.on_button(kWindow, btn, pressed, time);
  }
  void key(uint8_t keycode, bool pressed, uint32_t time = 3000) {
    backend.input_cbs.on_key(kWindow, keycode, pressed, time);
  }
};

} // namespace

// --- wiring -----------------------------------------------------------------

WW_TEST_CASE(input_callbacks_are_registered_on_construction) {
  Harness h;
  // Registration happens in the constructor; nothing else wires it up.
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_motion));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_button));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_key));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_enter));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_leave));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_expose));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_focus_in));
  WW_TEST_CHECK(static_cast<bool>(h.backend.input_cbs.on_focus_out));
}

WW_TEST_CASE(expose_is_forwarded_to_the_surface_port) {
  Harness h;
  h.backend.input_cbs.on_expose(kWindow);
  WW_TEST_CHECK_EQ(h.surfaces.expose_calls.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(h.surfaces.expose_calls.front(), kWindow);
}

// --- pointer focus ----------------------------------------------------------

WW_TEST_CASE(pointer_enter_sends_enter_to_the_client) {
  Harness h;
  h.enter();
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerEnter),
                   std::size_t{1});
}

WW_TEST_CASE(pointer_enter_carries_surface_local_coordinates) {
  Harness h;
  h.enter(kWindow, 11, 22);

  const auto events = h.wl.events_for(h.pointer);
  WW_TEST_CHECK(!events.empty());
  const auto &e = events.front();
  WW_TEST_CHECK_EQ(e.opcode, kPointerEnter);
  // args: serial (u32), surface (object id), x (fixed), y (fixed)
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(e, 2), wl_fixed_from_int(11));
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(e, 3), wl_fixed_from_int(22));
}

WW_TEST_CASE(pointer_enter_adds_the_surfaces_window_geometry_origin) {
  Harness h;
  // The surface declared a (26,22) window geometry, so its X11 window's
  // top-left sits at surface-local (26,22): a raw window-local (11,22) is
  // (37,44) in the surface's own coordinates.
  h.surfaces.set_origin(h.surface, 26, 22);
  h.enter(kWindow, 11, 22);

  const auto events = h.wl.events_for(h.pointer);
  WW_TEST_CHECK(!events.empty());
  const auto &e = events.front();
  WW_TEST_CHECK_EQ(e.opcode, kPointerEnter);
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(e, 2), wl_fixed_from_int(37));
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(e, 3), wl_fixed_from_int(44));
}

WW_TEST_CASE(pointer_motion_adds_the_focused_surfaces_window_geometry_origin) {
  Harness h;
  h.surfaces.set_origin(h.surface, 26, 22);
  h.enter();
  h.motion(11, 22);

  const auto events = h.wl.events_for(h.pointer);
  const WaylandFixture::Event *motion = nullptr;
  for (const auto &e : events)
    if (e.opcode == kPointerMotion)
      motion = &e;
  WW_TEST_CHECK(motion != nullptr);
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(*motion, 1), wl_fixed_from_int(37));
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(*motion, 2), wl_fixed_from_int(44));
}

WW_TEST_CASE(pointer_enter_on_an_unmapped_window_sends_nothing) {
  Harness h;
  h.enter(/*win=*/999); // no surface bound to this id
  WW_TEST_CHECK(h.wl.events_for(h.pointer).empty());
}

WW_TEST_CASE(pointer_leave_sends_leave_to_the_client) {
  Harness h;
  h.enter();
  h.leave();
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerLeave),
                   std::size_t{1});
}

WW_TEST_CASE(leave_for_a_window_that_never_had_focus_is_ignored) {
  Harness h;
  h.leave(); // no preceding enter
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerLeave),
                   std::size_t{0});
}

WW_TEST_CASE(moving_between_surfaces_leaves_the_old_one_before_entering) {
  Harness h;
  wl_resource *second = h.wl.surface();
  h.surfaces.bind(7, second);

  h.enter(kWindow);
  h.enter(7);

  const auto ops = h.wl.opcodes_for(h.pointer);
  // enter(first) ... leave(first) ... enter(second): the leave must appear
  // between the two enters, or the client believes it still has the pointer.
  std::size_t first_enter = ops.size(), leave = ops.size(),
              second_enter = ops.size();
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (ops[i] == kPointerEnter && first_enter == ops.size())
      first_enter = i;
    else if (ops[i] == kPointerLeave && leave == ops.size())
      leave = i;
    else if (ops[i] == kPointerEnter && leave != ops.size() &&
             second_enter == ops.size())
      second_enter = i;
  }
  WW_TEST_CHECK(first_enter < leave);
  WW_TEST_CHECK(leave < second_enter);
}

WW_TEST_CASE(re_entering_the_same_surface_does_not_re_leave_it) {
  Harness h;
  h.enter();
  h.enter(); // same window again
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerLeave),
                   std::size_t{0});
}

// --- motion -----------------------------------------------------------------

WW_TEST_CASE(motion_is_delivered_with_the_backend_timestamp) {
  Harness h;
  h.enter();
  h.motion(30, 40, 0, 0, /*time=*/1234);

  const auto events = h.wl.events_for(h.pointer);
  const ww_test::WaylandFixture::Event *motion = nullptr;
  for (const auto &e : events)
    if (e.opcode == kPointerMotion)
      motion = &e;
  // `events` is a named local, not a temporary: taking a pointer into the
  // result of events_for() directly would dangle once the loop ended.
  WW_TEST_CHECK(motion != nullptr);
  // args: time (u32), x (fixed), y (fixed)
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*motion, 0), uint32_t{1234});
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(*motion, 1), wl_fixed_from_int(30));
  WW_TEST_CHECK_EQ(WaylandFixture::arg_i32(*motion, 2), wl_fixed_from_int(40));
}

WW_TEST_CASE(motion_without_pointer_focus_sends_nothing) {
  Harness h;
  h.motion(1, 2); // never entered
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerMotion),
                   std::size_t{0});
}

// --- buttons and scroll -----------------------------------------------------

WW_TEST_CASE(buttons_are_translated_to_linux_input_codes) {
  Harness h;
  h.enter();
  h.button(1, true);
  h.button(2, true);
  h.button(3, true);

  std::vector<uint32_t> codes;
  for (const auto &e : h.wl.events_for(h.pointer))
    if (e.opcode == kPointerButton)
      codes.push_back(WaylandFixture::arg_u32(e, 2)); // serial, time, button

  WW_TEST_CHECK_EQ(codes.size(), std::size_t{3});
  // X11 orders buttons left/middle/right; Linux input codes do not.
  WW_TEST_CHECK_EQ(codes[0], kBtnLeft);
  WW_TEST_CHECK_EQ(codes[1], kBtnMiddle);
  WW_TEST_CHECK_EQ(codes[2], kBtnRight);
}

WW_TEST_CASE(scroll_buttons_become_axis_events_not_button_events) {
  Harness h;
  h.enter();
  h.button(4, true); // wheel up

  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerAxis), std::size_t{1});
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerButton),
                   std::size_t{0});
}

WW_TEST_CASE(scroll_axis_direction_and_orientation_are_correct) {
  Harness h;
  h.enter();
  h.button(4, true); // up    -> vertical, negative
  h.button(5, true); // down  -> vertical, positive
  h.button(6, true); // left  -> horizontal, negative
  h.button(7, true); // right -> horizontal, positive

  std::vector<std::pair<uint32_t, int32_t>> axes;
  for (const auto &e : h.wl.events_for(h.pointer))
    if (e.opcode == kPointerAxis) // args: time, axis, value(fixed)
      axes.emplace_back(WaylandFixture::arg_u32(e, 1),
                        WaylandFixture::arg_i32(e, 2));

  WW_TEST_CHECK_EQ(axes.size(), std::size_t{4});
  WW_TEST_CHECK_EQ(axes[0].first, uint32_t{WL_POINTER_AXIS_VERTICAL_SCROLL});
  WW_TEST_CHECK(axes[0].second < 0);
  WW_TEST_CHECK_EQ(axes[1].first, uint32_t{WL_POINTER_AXIS_VERTICAL_SCROLL});
  WW_TEST_CHECK(axes[1].second > 0);
  WW_TEST_CHECK_EQ(axes[2].first, uint32_t{WL_POINTER_AXIS_HORIZONTAL_SCROLL});
  WW_TEST_CHECK(axes[2].second < 0);
  WW_TEST_CHECK_EQ(axes[3].first, uint32_t{WL_POINTER_AXIS_HORIZONTAL_SCROLL});
  WW_TEST_CHECK(axes[3].second > 0);
}

WW_TEST_CASE(scroll_release_produces_no_second_axis_event) {
  Harness h;
  h.enter();
  h.button(4, true);
  h.button(4, false); // X11 sends a release for wheel "buttons" too

  // A wheel notch is one discrete event; emitting a second on release would
  // double every scroll.
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerAxis), std::size_t{1});
}

WW_TEST_CASE(button_press_asks_the_surface_port_to_dismiss_popups) {
  Harness h;
  h.enter();
  h.button(1, true);

  WW_TEST_CHECK_EQ(h.surfaces.dismiss_calls.size(), std::size_t{1});
  WW_TEST_CHECK(h.surfaces.dismiss_calls.front() == h.surface);
}

WW_TEST_CASE(button_release_does_not_dismiss_popups) {
  Harness h;
  h.enter();
  h.button(1, true);
  h.surfaces.dismiss_calls.clear();
  h.button(1, false);

  // Dismissal is a press-time decision; a release is the tail of a click
  // that already made it.
  WW_TEST_CHECK(h.surfaces.dismiss_calls.empty());
}

// --- keyboard ---------------------------------------------------------------

WW_TEST_CASE(pointer_enter_alone_does_not_grant_keyboard_focus) {
  // The historical behavior (keyboard focus following the pointer through
  // on_enter) only ever agreed with reality under a "focus follows mouse"
  // window-manager policy. Anything else -- click-to-focus, a
  // keyboard-driven window switch -- left it free to disagree with X's own
  // idea of who has keyboard focus. Pointer focus and keyboard focus are
  // now tracked independently; hovering a surface must not, by itself,
  // grant it wl_keyboard focus.
  Harness h;
  h.enter();
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardEnter),
                   std::size_t{0});
}

WW_TEST_CASE(real_focus_in_grants_keyboard_focus) {
  Harness h;
  h.focus_in();
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardEnter),
                   std::size_t{1});
}

WW_TEST_CASE(a_redundant_focus_in_for_the_current_surface_is_a_no_op) {
  Harness h;
  h.focus_in();
  h.focus_in(); // e.g. focus briefly bounces through a grab and settles back

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardEnter),
                   std::size_t{1});
}

WW_TEST_CASE(focus_out_for_the_focused_surface_sends_leave) {
  Harness h;
  h.focus_in();
  h.focus_out();

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardLeave),
                   std::size_t{1});
}

WW_TEST_CASE(focus_out_for_a_surface_that_is_not_focused_is_ignored) {
  Harness h;
  wl_resource *second = h.wl.surface();
  h.surfaces.bind(7, second);
  h.focus_in(kWindow);

  h.focus_out(7); // never had focus -- a stale/mismatched event

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardLeave),
                   std::size_t{0});
}

WW_TEST_CASE(keyboard_focus_change_notifies_the_registered_callback) {
  // Declared before the harness on purpose: the callback also fires during
  // display teardown (destroying the focus surface clears focus and reports
  // null), so anything it captures must outlive the harness. Declaring it
  // after was a use-after-free that only ASan caught.
  std::vector<wl_resource *> focus_changes;
  Harness h;
  h.input.set_keyboard_focus_changed_callback(
      [&](wl_resource *s) { focus_changes.push_back(s); });

  h.focus_in();

  // DataDeviceManager relies on this to know which client's wl_data_device
  // should receive the current selection.
  WW_TEST_CHECK_EQ(focus_changes.size(), std::size_t{1});
  WW_TEST_CHECK(focus_changes.front() == h.surface);
}

WW_TEST_CASE(moving_focus_leaves_the_previous_keyboard_surface) {
  Harness h;
  wl_resource *second = h.wl.surface();
  h.surfaces.bind(7, second);

  h.focus_in(kWindow);
  h.focus_in(7);

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardLeave),
                   std::size_t{1});
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardEnter),
                   std::size_t{2});
}

WW_TEST_CASE(key_events_are_delivered_to_the_focused_keyboard) {
  Harness h;
  h.focus_in();
  h.key(/*x11 keycode=*/38, true); // 'a' on a standard layout

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKey),
                   std::size_t{1});
}

WW_TEST_CASE(key_events_do_not_require_a_prior_focus_in_to_be_dispatched) {
  // The actual regression test for a real bug: on_key used to bail out
  // entirely unless wl_keyboard focus had already been assigned by
  // something else first, and once assigned, dispatch to that cached
  // value rather than to whichever window the X11 event itself named. X
  // only ever delivers a key event to whichever window currently holds
  // real input focus, so the event's own window is authoritative on its
  // own -- correct with no other state needing to have been set up first,
  // and immune to that cached value ever going stale (which it provably
  // could: nothing before this fix resynced it except a pointer crossing
  // a window boundary, which a keyboard-driven focus change never does).
  Harness h;
  h.key(38, true); // no focus_in() at all
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKey),
                   std::size_t{1});
}

WW_TEST_CASE(key_events_follow_wayland_pressed_key_state_without_duplicates) {
  // XKB detectable auto-repeat reports additional KeyPress events while the
  // same physical key remains down. Wayland clients receive repeat_info and
  // the compositor must not turn those into duplicate press transitions.
  // Nor may a stray duplicate release escape after the key is already up.
  Harness h;
  h.focus_in();
  h.key(38, true, 10);
  h.key(38, true, 11);  // repeat: ignored, key is already logically down
  h.key(38, false, 12);
  h.key(38, false, 13); // duplicate release: ignored

  const auto events = h.wl.events_for(h.keyboard);
  std::vector<const WaylandFixture::Event *> key_events;
  for (const auto &e : events)
    if (e.opcode == kKeyboardKey)
      key_events.push_back(&e);
  WW_TEST_CHECK_EQ(key_events.size(), std::size_t{2});
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*key_events[0], 3),
                   uint32_t{WL_KEYBOARD_KEY_STATE_PRESSED});
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*key_events[1], 3),
                   uint32_t{WL_KEYBOARD_KEY_STATE_RELEASED});
}

WW_TEST_CASE(keyboard_enter_lists_keys_already_physically_down) {
  Harness h;
  // Standard X11 keycode 38 is evdev key 30 ('a'). A focus-enter snapshot is
  // state only: clients must not synthesize a keypress from this array.
  h.backend.pressed_keycodes = {38};
  h.focus_in();

  const auto events = h.wl.events_for(h.keyboard);
  const WaylandFixture::Event *enter = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardEnter)
      enter = &e;
  WW_TEST_CHECK(enter != nullptr);
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*enter, 2), uint32_t{4});
  uint32_t evdev_key{};
  std::memcpy(&evdev_key, enter->args.data() + 12, sizeof(evdev_key));
  WW_TEST_CHECK_EQ(evdev_key, uint32_t{30});
}

WW_TEST_CASE(key_events_use_evdev_keycodes_not_x11_keycodes) {
  Harness h;
  h.focus_in();
  h.key(38, true);

  const auto events = h.wl.events_for(h.keyboard);
  const ww_test::WaylandFixture::Event *key = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardKey)
      key = &e;
  WW_TEST_CHECK(key != nullptr);
  // args: serial, time, key, state. X11 keycodes are evdev + 8, and Wayland
  // wants the evdev value back.
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*key, 2), uint32_t{38 - 8});
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*key, 3),
                   uint32_t{WL_KEYBOARD_KEY_STATE_PRESSED});
}

WW_TEST_CASE(key_for_an_unknown_window_sends_nothing) {
  Harness h;
  h.backend.input_cbs.on_key(/*win=*/999, 38, true, 0); // never bound
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKey),
                   std::size_t{0});
}

// --- resource removal -------------------------------------------------------

WW_TEST_CASE(removed_pointers_stop_receiving_events) {
  Harness h;
  h.input.remove_pointer(h.pointer);
  h.enter();
  WW_TEST_CHECK(h.wl.events_for(h.pointer).empty());
}

WW_TEST_CASE(removed_keyboards_stop_receiving_events) {
  Harness h;
  h.input.remove_keyboard(h.keyboard);
  h.focus_in();
  // The keymap and repeat_info sent at bind time are still on the wire, so
  // the assertion is about focus events specifically.
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardEnter),
                   std::size_t{0});
}

// --- cursor -----------------------------------------------------------------

WW_TEST_CASE(a_committed_cursor_is_applied_to_the_window_under_the_pointer) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();

  h.input.set_cursor(h.pointer, cursor, 4, 5);
  // The client's buffer lands afterwards, as it normally does.
  h.surfaces.commit_cursor(cursor, 501);

  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{501});
  // The hotspot travels to SurfaceManager, where it is baked into the cursor.
  WW_TEST_CHECK_EQ(h.surfaces.cursor_set_calls.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(h.surfaces.cursor_set_calls.front().hotspot_x, int32_t{4});
  WW_TEST_CHECK_EQ(h.surfaces.cursor_set_calls.front().hotspot_y, int32_t{5});
}

WW_TEST_CASE(no_overlay_window_is_created_for_a_cursor) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  h.input.set_cursor(h.pointer, cursor, 0, 0);
  h.surfaces.commit_cursor(cursor, 501);

  // The pointer image is the window system's own cursor, not a window the
  // compositor has to keep under the pointer.
  WW_TEST_CHECK(!h.backend.called("create_window"));
  WW_TEST_CHECK(!h.backend.called("move_window"));
}

WW_TEST_CASE(a_cursor_that_was_already_committed_applies_straight_away) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  // The client attached and committed before it called set_cursor.
  h.surfaces.cursor_handles[cursor] = 502;

  h.input.set_cursor(h.pointer, cursor, 0, 0);

  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{502});
}

WW_TEST_CASE(each_new_cursor_image_replaces_the_previous_one) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  h.input.set_cursor(h.pointer, cursor, 0, 0);

  // An animated cursor: one surface, a new image on every frame.
  h.surfaces.commit_cursor(cursor, 601);
  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{601});
  h.surfaces.commit_cursor(cursor, 602);
  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{602});
}

WW_TEST_CASE(a_cursor_surface_with_no_image_yet_keeps_the_current_cursor) {
  Harness h;
  h.enter();
  wl_resource *first = h.wl.surface();
  wl_resource *second = h.wl.surface();
  h.input.set_cursor(h.pointer, first, 0, 0);
  h.surfaces.commit_cursor(first, 701);

  // Switching surfaces before the new one has committed must not flash the
  // default arrow in between.
  h.input.set_cursor(h.pointer, second, 0, 0);
  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{701});

  h.surfaces.commit_cursor(second, 702);
  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{702});
}

WW_TEST_CASE(set_cursor_from_an_unfocused_client_is_refused) {
  Harness h;
  wl_resource *cursor = h.wl.surface();
  // No enter has happened, so nobody owns pointer focus.
  h.input.set_cursor(h.pointer, cursor, 0, 0);

  WW_TEST_CHECK(h.surfaces.cursor_set_calls.empty());
  WW_TEST_CHECK(!h.backend.called("set_window_cursor"));
}

WW_TEST_CASE(hiding_the_cursor_clears_the_role_and_applies_the_blank_cursor) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  h.input.set_cursor(h.pointer, cursor, 0, 0);
  h.surfaces.commit_cursor(cursor, 801);

  h.input.set_cursor(h.pointer, nullptr, 0, 0);

  WW_TEST_CHECK_EQ(h.surfaces.cursor_cleared_calls.size(), std::size_t{1});
  WW_TEST_CHECK(h.surfaces.cursor_cleared_calls.front() == cursor);
  WW_TEST_CHECK(h.backend.blank_cursor_id != 0);
  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), h.backend.blank_cursor_id);
}

WW_TEST_CASE(a_hidden_cursor_comes_back_when_the_client_sets_one_again) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  h.input.set_cursor(h.pointer, nullptr, 0, 0);

  h.input.set_cursor(h.pointer, cursor, 0, 0);
  h.surfaces.commit_cursor(cursor, 811);

  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{811});
}

WW_TEST_CASE(entering_the_window_again_reapplies_the_clients_cursor) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  h.input.set_cursor(h.pointer, cursor, 0, 0);
  h.surfaces.commit_cursor(cursor, 901);

  h.leave();
  h.enter();

  // The client has not answered the new enter yet; the cursor it last asked
  // for carries on rather than reverting to the default for a moment.
  WW_TEST_CHECK_EQ(h.backend.cursor_of(kWindow), uint32_t{901});
}

WW_TEST_CASE(cursor_changes_do_not_touch_a_window_the_pointer_has_left) {
  Harness h;
  h.enter();
  wl_resource *cursor = h.wl.surface();
  h.input.set_cursor(h.pointer, cursor, 0, 0);
  h.surfaces.commit_cursor(cursor, 911);

  h.leave();
  h.backend.window_cursors.clear();
  // An animation frame arriving after the pointer left: the window the
  // pointer used to be over must not be addressed (it may be gone).
  h.surfaces.commit_cursor(cursor, 912);

  WW_TEST_CHECK(!h.backend.called("blank_cursor"));
  WW_TEST_CHECK(h.backend.window_cursors.empty());
}

WW_TEST_CASE(destroying_the_translator_releases_the_cursor_role) {
  FakeSurfaceInteractions surfaces;
  FakeWindowBackend backend;
  WaylandFixture wl;
  wl_resource *surface = wl.surface();
  wl_resource *pointer = wl.pointer();
  surfaces.bind(kWindow, surface);
  wl_resource *cursor = wl.surface();
  {
    ww::InputTranslator input{backend, surfaces};
    input.set_display(wl.display());
    input.add_pointer(pointer);
    backend.input_cbs.on_enter(kWindow, 1, 1, 1, 1);
    input.set_cursor(pointer, cursor, 0, 0);
  }

  // SurfaceManager outlives the translator and holds a callback into it; a
  // late commit must find the role gone, not a dangling `this`.
  WW_TEST_CHECK_EQ(surfaces.cursor_cleared_calls.size(), std::size_t{1});
  backend.clear_calls();
  surfaces.commit_cursor(cursor, 1001);
  WW_TEST_CHECK(!backend.called("set_window_cursor"));
}

// --- drag redirect ----------------------------------------------------------

WW_TEST_CASE(begin_drag_synthesizes_an_enter_for_the_current_surface) {
  Harness h;
  h.enter(kWindow, 9, 10);

  wl_resource *entered = nullptr;
  int16_t ex = -1, ey = -1;
  ww::InputTranslator::DragCallbacks cbs;
  cbs.on_enter = [&](wl_resource *s, int16_t x, int16_t y) {
    entered = s;
    ex = x;
    ey = y;
  };
  h.input.begin_drag(cbs);

  // The drag starts from a press on a surface the pointer already sits over,
  // so the target is known immediately rather than at the next motion.
  WW_TEST_CHECK(entered == h.surface);
  WW_TEST_CHECK_EQ(ex, int16_t{9});
  WW_TEST_CHECK_EQ(ey, int16_t{10});
}

WW_TEST_CASE(drag_positions_include_the_targets_window_geometry_origin) {
  Harness h;
  h.surfaces.set_origin(h.surface, 26, 22);
  h.enter(kWindow, 1, 2);

  int16_t enter_x = -1, enter_y = -1, motion_x = -1, motion_y = -1;
  ww::InputTranslator::DragCallbacks cbs;
  cbs.on_enter = [&](wl_resource *, int16_t x, int16_t y) {
    enter_x = x;
    enter_y = y;
  };
  cbs.on_motion = [&](int16_t x, int16_t y, uint32_t) {
    motion_x = x;
    motion_y = y;
  };
  h.input.begin_drag(cbs);
  // Synthesized enter replays the already-corrected pointer position.
  WW_TEST_CHECK_EQ(enter_x, int16_t{27});
  WW_TEST_CHECK_EQ(enter_y, int16_t{24});

  h.motion(11, 22);
  WW_TEST_CHECK_EQ(motion_x, int16_t{37});
  WW_TEST_CHECK_EQ(motion_y, int16_t{44});
}

WW_TEST_CASE(drag_suppresses_ordinary_pointer_dispatch) {
  Harness h;
  h.enter();
  h.input.begin_drag({});

  h.motion(1, 2);
  h.button(1, false);

  // An implicit grab freezes ordinary wl_pointer dispatch for the duration.
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerMotion),
                   std::size_t{0});
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerButton),
                   std::size_t{0});
}

WW_TEST_CASE(drag_motion_and_release_reach_the_drag_callbacks) {
  Harness h;
  h.enter();

  int motions = 0;
  bool released = false;
  ww::InputTranslator::DragCallbacks cbs;
  cbs.on_motion = [&](int16_t, int16_t, uint32_t) { ++motions; };
  cbs.on_release = [&]() { released = true; };
  h.input.begin_drag(cbs);

  h.motion(3, 4);
  h.button(1, false);

  WW_TEST_CHECK_EQ(motions, 1);
  WW_TEST_CHECK(released);
}

WW_TEST_CASE(drag_leaving_a_surface_fires_on_leave) {
  Harness h;
  wl_resource *second = h.wl.surface();
  h.surfaces.bind(7, second);
  h.enter(kWindow);

  int leaves = 0;
  ww::InputTranslator::DragCallbacks cbs;
  cbs.on_leave = [&]() { ++leaves; };
  h.input.begin_drag(cbs);

  h.enter(7); // drag moved onto a different surface

  WW_TEST_CHECK_EQ(leaves, 1);
}

WW_TEST_CASE(end_drag_resumes_ordinary_pointer_dispatch) {
  Harness h;
  h.enter();
  h.input.begin_drag({});
  h.input.end_drag();

  h.motion(1, 2);

  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerMotion),
                   std::size_t{1});
}

WW_TEST_CASE(end_drag_without_an_active_drag_is_harmless) {
  Harness h;
  h.input.end_drag();
  h.enter();
  WW_TEST_CHECK_EQ(h.wl.count_events(h.pointer, kPointerEnter),
                   std::size_t{1});
}

// --- degenerate setup -------------------------------------------------------

WW_TEST_CASE(events_before_set_display_are_dropped_not_crashed) {
  // WWindows::create() constructs the translator before the Wayland display
  // exists, so every handler must tolerate a null display.
  FakeWindowBackend backend;
  FakeSurfaceInteractions surfaces;
  ww::InputTranslator input{backend, surfaces};

  backend.input_cbs.on_enter(1, 0, 0, 0, 0);
  backend.input_cbs.on_motion(1, 0, 0, 0, 0, 0);
  backend.input_cbs.on_button(1, 1, true, 0);
  backend.input_cbs.on_key(1, 38, true, 0);
  backend.input_cbs.on_leave(1);

  WW_TEST_CHECK(true); // surviving without a crash is the assertion
}

WW_TEST_CASE(destroying_the_translator_clears_backend_callbacks) {
  FakeWindowBackend backend;
  FakeSurfaceInteractions surfaces;
  {
    ww::InputTranslator input{backend, surfaces};
    WW_TEST_CHECK(static_cast<bool>(backend.input_cbs.on_motion));
  }
  // The backend outlives the translator, so stale callbacks would call into
  // a destroyed Impl on the next input event.
  WW_TEST_CHECK(!backend.input_cbs.on_motion);
}

// --- modifier resync on real focus change -----------------------------------
//
// The regression test for a real bug: a window manager's own key binding
// (e.g. i3's MOD+Shift+<n>) actively grabs the whole keyboard once it
// fires (see XGrabKey), which redirects the rest of that key-down --
// typically including the modifiers' own release -- to the grabbing
// client instead of whichever window has focus. xkb_state only ever
// learns about key transitions this compositor personally observed, so it
// was left believing Shift (or Ctrl, or Mod4...) was still held long
// after the physical key came back up, corrupting every subsequent
// keystroke sent to whatever regained focus.

WW_TEST_CASE(focus_in_resyncs_modifiers_from_the_live_keyboard_state) {
  Harness h;
  // x11 keycode 50 = Shift_L in a standard evdev-based keymap.
  h.key(50, true); // Shift pressed while this surface had focus...
  h.focus_out();
  // ...and its release stolen by a window-manager grab: on_key never
  // fires for it. xkb_state is left believing Shift is still down.
  h.backend.pressed_keycodes.clear(); // ground truth: nothing is held

  h.focus_in(); // real focus returns

  // send_modifiers is called as part of every wl_keyboard.enter; find the
  // wl_keyboard.modifiers event that followed it and check depressed==0.
  constexpr uint16_t kKeyboardModifiers = 4;
  const auto events = h.wl.events_for(h.keyboard);
  const ww_test::WaylandFixture::Event *mods = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardModifiers)
      mods = &e; // last one wins -- the one from this focus_in
  WW_TEST_CHECK(mods != nullptr);
  // args: serial, mods_depressed, mods_latched, mods_locked, group.
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*mods, 1), uint32_t{0});
}

WW_TEST_CASE(focus_in_resync_preserves_a_modifier_genuinely_still_held) {
  Harness h;
  h.key(37, true); // Ctrl_L, still physically held
  h.backend.pressed_keycodes = {37};
  h.focus_out();

  h.focus_in();

  constexpr uint16_t kKeyboardModifiers = 4;
  const auto events = h.wl.events_for(h.keyboard);
  const ww_test::WaylandFixture::Event *mods = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardModifiers)
      mods = &e;
  WW_TEST_CHECK(mods != nullptr);
  // Ctrl is real modifier index 2 in the default xkb keymap -> bit 2 (0x4).
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*mods, 1), uint32_t{0x4});
}

WW_TEST_CASE(query_pressed_keycodes_is_consulted_on_every_real_focus_change) {
  Harness h;
  h.focus_in();
  WW_TEST_CHECK(std::count(h.backend.calls.begin(), h.backend.calls.end(),
                          "query_pressed_keycodes") >= 1);
}

// --- self-healing focus: a key event is enough on its own -------------------
//
// The regression test for the actual "focus lost, cannot be regained" bug: a
// window manager is not obligated to re-send FocusIn for a window it
// believes is already focused (e.g. switching back to a workspace after a
// hidden window closed, without the WM re-announcing focus it thinks never
// changed). If nothing else can ever re-establish wl_keyboard focus, it's
// stuck forever with no event left to fix it. A key event landing on a
// window proves beyond doubt that the window holds real input focus right
// now, whatever did or didn't happen before it, so on_key must be able to
// (re)establish focus by itself, unconditionally, with no dependency on a
// dedicated FocusIn ever arriving.

WW_TEST_CASE(a_key_event_establishes_focus_with_no_prior_focus_in) {
  Harness h;
  h.key(38, true); // no focus_in() at all -- the exact scenario that broke

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardEnter),
                   std::size_t{1});
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKey),
                   std::size_t{1});
}

WW_TEST_CASE(a_key_event_sends_enter_before_key_never_key_alone) {
  // Protocol correctness, not just "something arrived": a client that
  // receives wl_keyboard.key for a surface it was never sent
  // wl_keyboard.enter for has no established keyboard-focus surface to
  // attribute the key to at all, per the Wayland protocol's own
  // enter/leave contract. Ordering matters, not just presence.
  Harness h;
  h.key(38, true);

  const auto events = h.wl.events_for(h.keyboard);
  int enter_index = -1, key_index = -1;
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].opcode == kKeyboardEnter && enter_index < 0)
      enter_index = static_cast<int>(i);
    if (events[i].opcode == kKeyboardKey && key_index < 0)
      key_index = static_cast<int>(i);
  }
  WW_TEST_CHECK(enter_index >= 0);
  WW_TEST_CHECK(key_index >= 0);
  WW_TEST_CHECK(enter_index < key_index);
}

WW_TEST_CASE(a_key_event_corrects_focus_stuck_on_a_different_surface) {
  // A closer model of the real bug: kbd_focus is left pointing somewhere
  // that no longer reflects reality (here, simulated directly, since the
  // fake backend has no window-manager-quirk simulation of its own), and
  // a key event for the *actual* current window must still win.
  Harness h;
  wl_resource *stale = h.wl.surface();
  h.surfaces.bind(7, stale);
  h.focus_in(7); // kbd_focus is now "stale", not kWindow's surface

  h.key(38, true); // arrives on kWindow -- X says *this* is who has focus

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardLeave),
                   std::size_t{1}); // stale's focus was cleaned up
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKey),
                   std::size_t{1});
}

// --- real XKB state notify ---------------------------------------------

WW_TEST_CASE(xkb_state_changed_updates_modifiers_without_another_key_event) {
  // Unlike resync_modifiers() (a poll at focus-transition points), this is
  // the server's own real-time push. It must reach the client immediately,
  // even when the modifier's core KeyRelease was consumed by a WM grab.
  Harness h;
  h.focus_in(); // establish a keyboard resource with a known baseline

  // Ctrl is real modifier index 2 in the default xkb keymap -> bit 2 (0x4).
  h.backend.input_cbs.on_xkb_state_changed(/*depressed=*/0x4, /*latched=*/0,
                                           /*locked=*/0, /*group=*/0);

  constexpr uint16_t kKeyboardModifiers = 4;
  const auto events = h.wl.events_for(h.keyboard);
  const WaylandFixture::Event *mods = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardModifiers)
      mods = &e;
  WW_TEST_CHECK(mods != nullptr);
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*mods, 1), uint32_t{0x4});
}

WW_TEST_CASE(
    authoritative_key_events_do_not_publish_a_stale_modifier_snapshot) {
  Harness h;
  h.focus_in();
  h.backend.input_cbs.on_xkb_state_changed(/*depressed=*/0x1, /*latched=*/0,
                                           /*locked=*/0, /*group=*/0);
  const auto modifiers_before =
      h.wl.count_events(h.keyboard, kKeyboardModifiersOpcode);

  // With server StateNotify active, an ordinary non-modifier key doesn't
  // change modifier state. The key callback must not emit a redundant snapshot
  // from a potentially older local state.
  h.key(38, true, 30);
  WW_TEST_CHECK_EQ(
      h.wl.count_events(h.keyboard, kKeyboardModifiersOpcode),
      modifiers_before);
}

WW_TEST_CASE(authoritative_modifier_release_is_not_resurrected_by_next_key) {
  Harness h;
  h.focus_in();
  h.key(50, true); // Shift_L: this keydown is observed locally

  // The X server tells us Shift is down, then tells us the release occurred
  // while a WM grab held the keyboard. WWindows never received the core
  // KeyRelease, so its event-tracked pressed-key set still contains Shift.
  h.backend.input_cbs.on_xkb_state_changed(/*depressed=*/0x1, /*latched=*/0,
                                           /*locked=*/0, /*group=*/0);
  h.backend.input_cbs.on_xkb_state_changed(/*depressed=*/0, /*latched=*/0,
                                           /*locked=*/0, /*group=*/0);
  h.key(38, true); // must not mix update_key() back into the mask-only state

  constexpr uint16_t kKeyboardModifiers = 4;
  const auto events = h.wl.events_for(h.keyboard);
  const WaylandFixture::Event *mods = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardModifiers)
      mods = &e;
  WW_TEST_CHECK(mods != nullptr);
  WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*mods, 1), uint32_t{0});
}

WW_TEST_CASE(xkb_state_notify_repairs_a_swallowed_modifier_release) {
  Harness h;
  h.focus_in();
  h.key(50, true, 10); // Shift press reached the Wayland client

  // Simulate the matching XKB StateNotify when the WM grab consumes Shift's
  // core KeyRelease. The causal key release must be reconciled before the
  // authoritative modifier snapshot is sent.
  h.backend.input_cbs.on_xkb_key_transition(50, false, 11);
  h.backend.input_cbs.on_xkb_state_changed(/*depressed=*/0, /*latched=*/0,
                                           /*locked=*/0, /*group=*/0);

  const auto events = h.wl.events_for(h.keyboard);
  std::vector<const WaylandFixture::Event *> key_events;
  std::size_t release_index = events.size();
  std::size_t final_modifiers_index = events.size();
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].opcode == kKeyboardKey) {
      key_events.push_back(&events[i]);
      if (WaylandFixture::arg_u32(events[i], 3) ==
          WL_KEYBOARD_KEY_STATE_RELEASED)
        release_index = i;
    }
    if (events[i].opcode == kKeyboardModifiersOpcode)
      final_modifiers_index = i;
  }
  WW_TEST_CHECK_EQ(key_events.size(), std::size_t{2});
  WW_TEST_CHECK(release_index < final_modifiers_index);
  if (key_events.size() == 2)
    WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*key_events.back(), 3),
                     uint32_t{WL_KEYBOARD_KEY_STATE_RELEASED});
}

WW_TEST_CASE(keyboard_keymap_string_is_consulted_when_backend_provides_none) {
  // The fake backend returns nullopt (no real X session to ask), which
  // must not prevent keyboard input from working at all -- it's the
  // signal to fall back to a default keymap, not a hard failure.
  Harness h;
  WW_TEST_CHECK(std::count(h.backend.calls.begin(), h.backend.calls.end(),
                          "keyboard_keymap_string") >= 1);
  h.key(38, true);
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKey),
                   std::size_t{1});
}

// --- keyboard auto-repeat is the window system's setting ----------------------

namespace {
// The (rate, delay) of the last wl_keyboard.repeat_info sent to `kbd`.
std::pair<int32_t, int32_t> last_repeat_info(Harness &h, wl_resource *kbd) {
  std::pair<int32_t, int32_t> out{-1, -1};
  for (const auto &e : h.wl.events_for(kbd)) {
    if (e.opcode != kKeyboardRepeatInfo)
      continue;
    out = {WaylandFixture::arg_i32(e, 0), WaylandFixture::arg_i32(e, 1)};
  }
  return out;
}
} // namespace

WW_TEST_CASE(repeat_info_falls_back_to_a_default_when_the_backend_cannot_say) {
  Harness h; // the fake reports no repeat setting
  WW_TEST_CHECK(last_repeat_info(h, h.keyboard) ==
                (std::pair<int32_t, int32_t>{25, 200}));
}

WW_TEST_CASE(a_changed_server_repeat_setting_is_sent_to_every_keyboard) {
  Harness h;
  wl_resource *second = h.wl.keyboard();
  h.input.add_keyboard(second);

  h.backend.key_repeat = ww::IWindowBackend::KeyRepeat{40, 300};
  h.backend.input_cbs.on_xkb_repeat_changed();

  WW_TEST_CHECK(last_repeat_info(h, h.keyboard) ==
                (std::pair<int32_t, int32_t>{40, 300}));
  WW_TEST_CHECK(last_repeat_info(h, second) ==
                (std::pair<int32_t, int32_t>{40, 300}));
}

WW_TEST_CASE(a_keyboard_bound_later_gets_the_current_repeat_setting) {
  Harness h;
  h.backend.key_repeat = ww::IWindowBackend::KeyRepeat{33, 450};
  h.backend.input_cbs.on_xkb_repeat_changed();

  wl_resource *late = h.wl.keyboard();
  h.input.add_keyboard(late);
  WW_TEST_CHECK(last_repeat_info(h, late) ==
                (std::pair<int32_t, int32_t>{33, 450}));
}

WW_TEST_CASE(repeat_switched_off_in_the_server_is_announced_as_rate_zero) {
  Harness h;
  h.backend.key_repeat = ww::IWindowBackend::KeyRepeat{0, 500};
  h.backend.input_cbs.on_xkb_repeat_changed();
  WW_TEST_CHECK_EQ(last_repeat_info(h, h.keyboard).first, int32_t{0});
}

WW_TEST_CASE(an_unrelated_xkb_controls_change_sends_nothing) {
  // ControlsNotify also fires for controls this compositor does not announce
  // (sticky keys, ...). The setting re-read is unchanged, so no client gets a
  // redundant repeat_info.
  Harness h;
  h.backend.key_repeat = ww::IWindowBackend::KeyRepeat{40, 300};
  h.backend.input_cbs.on_xkb_repeat_changed();
  const auto before = h.wl.count_events(h.keyboard, kKeyboardRepeatInfo);

  h.backend.input_cbs.on_xkb_repeat_changed();
  h.backend.input_cbs.on_xkb_repeat_changed();

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardRepeatInfo), before);
}
