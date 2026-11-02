#include "wwindows/input/detail/input_translator_impl.hpp"
#include <wwindows/support/log.hpp>
#include <wwindows/input/policy/pointer_mapping.hpp>

#include <wayland-server-protocol.h>

namespace ww {

namespace {

// Map X11/xcb button number → Linux input button code.
// Returns 0 for scroll buttons (4-7), which are handled as axis events.
uint32_t xcb_btn_to_linux(uint8_t btn) { return input::policy::linux_button(btn); }

} // namespace

void InputTranslator::Impl::ptr_frame(wl_resource *p) {
  if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
    wl_pointer_send_frame(p);
}

void InputTranslator::Impl::to_surface_local(wl_resource *surf, int16_t &x,
                                             int16_t &y) const {
  if (!surf || !surfaces)
    return;
  int32_t ox = 0, oy = 0;
  surfaces->surface_local_origin(surf, ox, oy);
  x = static_cast<int16_t>(x + ox);
  y = static_cast<int16_t>(y + oy);
}

void InputTranslator::Impl::on_enter(uint32_t win, int16_t x, int16_t y,
                                     int16_t /*root_x*/, int16_t /*root_y*/) {
  last_local_x = x;
  last_local_y = y;

  if (!display)
    return;
  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() { wl_display_flush_clients(display); }
  } guard{display};
  wl_resource *surf = surfaces->surface_for_window(win);
  if (!surf)
    return;

  // X11 gives window-local coordinates; the surface's own origin may sit at
  // its declared window-geometry origin inside the buffer. Correct once,
  // here, for both the wl_pointer and drag paths below (and the cache
  // begin_drag() replays from).
  to_surface_local(surf, x, y);
  last_local_x = x;
  last_local_y = y;

  // While a wl_data_device drag is active, pointer dispatch is redirected
  // entirely to the drag (see InputTranslator::begin_drag) — ordinary
  // wl_pointer/keyboard-focus-follows-pointer stays frozen at whatever it
  // was when the drag began, exactly as an implicit grab would freeze it.
  if (drag_active) {
    if (drag_target && drag_target != surf && drag_cbs.on_leave)
      drag_cbs.on_leave();
    drag_target = surf;
    if (drag_cbs.on_enter)
      drag_cbs.on_enter(surf, x, y);
    return;
  }

  WW_LOG_DEBUG("[input] pointer enter (x11={}, surface={}, pos={},{})", win,
               static_cast<void *>(surf), x, y);

  // --- pointer leave (old focus) ---
  if (ptr_focus && ptr_focus != surf) {
    const uint32_t ser = wl_display_next_serial(display);
    wl_client *old_client = wl_resource_get_client(ptr_focus);
    for (auto *p : pointers) {
      if (wl_resource_get_client(p) != old_client)
        continue;
      wl_pointer_send_leave(p, ser, ptr_focus);
      ptr_frame(p);
    }
    wl_list_remove(&ptr_focus_listener.link);
  }
  if (ptr_focus != surf) {
    ptr_focus = surf;
    wl_resource_add_destroy_listener(surf, &ptr_focus_listener);
  }

  // The pointer is now over `win`: that is the window the cursor goes on.
  // Applied immediately rather than when the client answers this enter with
  // set_cursor, so there is no flash of the wrong image in between -- over
  // the same client's windows the cursor it last asked for simply carries
  // on, and over anyone else's the window system's default takes over until
  // they ask for theirs (an enter leaves the cursor undefined).
  ptr_focus_window = win;
  if (cursor_client != wl_resource_get_client(surf)) {
    cursor_client = nullptr;
    cursor_hidden = false;
  }
  apply_cursor_to_focus();

  // --- pointer enter (new focus) ---
  {
    const uint32_t ser = wl_display_next_serial(display);
    wl_client *new_client = wl_resource_get_client(surf);
    const wl_fixed_t fx = wl_fixed_from_int(x), fy = wl_fixed_from_int(y);
    for (auto *p : pointers) {
      if (wl_resource_get_client(p) != new_client)
        continue;
      wl_pointer_send_enter(p, ser, surf, fx, fy);
      ptr_frame(p);
    }
  }

  // Keyboard focus is deliberately NOT assigned here. It used to follow
  // the pointer through this same function, which only ever agreed with
  // reality when the window manager's own focus policy happened to be
  // "focus follows mouse" too — anything else (click-to-focus, a
  // keyboard-driven window switch, a closed window's focus going
  // elsewhere) left this compositor's idea of keyboard focus and the X
  // server's actual one free to disagree, and a keyboard-driven switch in
  // particular never crosses a window boundary with the pointer at all,
  // so on_enter would simply never fire for it. See on_focus_in/
  // on_focus_out (InputTranslator_keyboard.cpp), driven by real
  // FocusIn/FocusOut instead, for where keyboard focus is decided now.
}

void InputTranslator::Impl::on_leave(uint32_t win) {
  if (!display)
    return;
  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() { wl_display_flush_clients(display); }
  } guard{display};
  wl_resource *surf = surfaces->surface_for_window(win);
  if (!surf)
    return;

  if (drag_active) {
    if (surf == drag_target) {
      if (drag_cbs.on_leave)
        drag_cbs.on_leave();
      drag_target = nullptr;
    }
    return;
  }

  if (surf != ptr_focus)
    return;

  WW_LOG_DEBUG("[input] pointer leave (x11={}, surface={})", win,
               static_cast<void *>(surf));

  const uint32_t ser = wl_display_next_serial(display);
  wl_client *surf_client = wl_resource_get_client(surf);
  for (auto *p : pointers) {
    if (wl_resource_get_client(p) != surf_client)
      continue;
    wl_pointer_send_leave(p, ser, surf);
    ptr_frame(p);
  }
  wl_list_remove(&ptr_focus_listener.link);
  ptr_focus = nullptr;
  if (ptr_focus_window == win)
    ptr_focus_window = 0;
}

void InputTranslator::Impl::on_motion(uint32_t /*win*/, int16_t x, int16_t y,
                                      int16_t /*root_x*/, int16_t /*root_y*/,
                                      uint32_t time) {
  last_local_x = x;
  last_local_y = y;

  if (!display)
    return;
  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() {
      if (display)
        wl_display_flush_clients(display);
    }
  } guard{display};

  // Same correction as on_enter, applied for whichever surface is actually
  // receiving this motion: the drag target during a drag, else the pointer
  // focus.
  if (wl_resource *target = drag_active ? drag_target : ptr_focus) {
    to_surface_local(target, x, y);
    last_local_x = x;
    last_local_y = y;
  }

  if (drag_active) {
    if (drag_target && drag_cbs.on_motion)
      drag_cbs.on_motion(x, y, time);
    return;
  }

  if (!ptr_focus)
    return;
  // Fires on every mouse move — trace only, never on by default.
  WW_LOG_TRACE("[input] pointer motion (pos={},{}, time={})", x, y, time);
  const wl_fixed_t fx = wl_fixed_from_int(x), fy = wl_fixed_from_int(y);
  wl_client *focus_client = wl_resource_get_client(ptr_focus);
  for (auto *p : pointers) {
    if (wl_resource_get_client(p) != focus_client)
      continue;
    wl_pointer_send_motion(p, time, fx, fy);
    ptr_frame(p);
  }
}

void InputTranslator::Impl::on_button(uint32_t /*win*/, uint8_t btn,
                                      bool pressed, uint32_t time) {
  if (!display)
    return;
  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() {
      if (display)
        wl_display_flush_clients(display);
    }
  } guard{display};

  if (drag_active) {
    // Any button release ends the drag — one drop point, whatever surface
    // (if any) is currently entered. drag_cbs.on_release() is responsible
    // for sending drop/cancelled and calling InputTranslator::end_drag().
    if (!pressed && drag_cbs.on_release)
      drag_cbs.on_release();
    return;
  }

  if (!ptr_focus)
    return;

  // Standard xdg_popup behavior: a button press that lands outside the
  // currently-open popup chain dismisses it. Checked on every press
  // (including what will become a scroll axis below) since a scroll click
  // is still a click landing somewhere.
  if (pressed && surfaces)
    surfaces->dismiss_popups_not_owning(ptr_focus);

  // Buttons 4-7 are scroll axes, not discrete buttons. The button->axis
  // decision lives in input::policy so this dispatch path and its unit
  // tests exercise the same code rather than two copies that can drift.
  if (input::policy::Axis scroll{}; input::policy::scroll_axis(btn, scroll)) {
    if (!pressed)
      return; // fire only on press, not release
    const uint32_t axis = scroll.axis == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                           : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    WW_LOG_TRACE("[input] scroll axis={} value={}",
                 scroll.axis == 0 ? "vert" : "horiz", scroll.value);
    wl_client *focus_client = wl_resource_get_client(ptr_focus);
    for (auto *p : pointers) {
      if (wl_resource_get_client(p) != focus_client)
        continue;
      wl_pointer_send_axis(p, time, axis, wl_fixed_from_double(scroll.value));
      ptr_frame(p);
    }
    return;
  }

  const uint32_t linux_btn = xcb_btn_to_linux(btn);
  if (!linux_btn) {
    WW_LOG_DEBUG("[input] unmapped pointer button {} ignored", btn);
    return;
  }

  WW_LOG_DEBUG("[input] pointer button {} ({}, linux_code=0x{:x})", btn,
               pressed ? "pressed" : "released", linux_btn);

  const uint32_t ser = wl_display_next_serial(display);
  const uint32_t state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                 : WL_POINTER_BUTTON_STATE_RELEASED;
  wl_client *focus_client = wl_resource_get_client(ptr_focus);
  for (auto *p : pointers) {
    if (wl_resource_get_client(p) != focus_client)
      continue;
    wl_pointer_send_button(p, ser, time, linux_btn, state);
    ptr_frame(p);
  }
}

void InputTranslator::Impl::on_cursor_surface_destroy(wl_listener *listener,
                                                      void *) {
  Impl *im = wl_container_of(listener, im, cursor_surface_listener);
  WW_LOG_TRACE("[input] cursor surface destroyed out from under us");
  // SurfaceManager's own destroy_surface() already freed the cursor object it
  // owned for this surface -- just drop our stale reference. Whatever window
  // still shows that cursor keeps it until the client sets another one,
  // which it does on its next enter.
  im->cursor_surface = nullptr;
}

uint32_t InputTranslator::Impl::cursor_for_focus() const {
  if (!ptr_focus || !cursor_client ||
      wl_resource_get_client(ptr_focus) != cursor_client)
    return 0;
  if (cursor_hidden)
    return backend->blank_cursor();
  return cursor_surface ? surfaces->cursor_for_surface(cursor_surface) : 0;
}

void InputTranslator::Impl::apply_cursor_to_focus() {
  if (!backend || !ptr_focus || !ptr_focus_window)
    return;
  backend->set_window_cursor(ptr_focus_window, cursor_for_focus());
}

void InputTranslator::Impl::on_cursor_image_changed() {
  // Fired by SurfaceManager from inside a cursor surface's commit, i.e. a
  // new image exists. Only ever applies a real cursor: "nothing yet" must
  // not blank out the one that is showing.
  if (!backend || !ptr_focus || !ptr_focus_window)
    return;
  if (const uint32_t cursor = cursor_for_focus())
    backend->set_window_cursor(ptr_focus_window, cursor);
}

void InputTranslator::Impl::set_cursor(wl_resource *pointer_res,
                                       wl_resource *surface_res,
                                       int32_t hotspot_x, int32_t hotspot_y) {
  if (!display || !backend || !surfaces)
    return;

  // Only the client that currently owns pointer focus may set the cursor.
  // The protocol ties this to the serial of the enter event the request is
  // in response to; we approximate that with "belongs to the focused
  // client", consistent with how serials are treated as advisory elsewhere
  // in this compositor rather than strictly validated.
  if (!ptr_focus ||
      wl_resource_get_client(pointer_res) != wl_resource_get_client(ptr_focus))
    return;

  cursor_client = wl_resource_get_client(pointer_res);

  // Detach from whatever surface previously held the cursor role, if it's
  // changing — covers both "switching to a different cursor surface" and
  // "hiding the cursor" (surface_res == nullptr).
  if (cursor_surface && cursor_surface != surface_res) {
    surfaces->clear_cursor_surface(cursor_surface);
    wl_list_remove(&cursor_surface_listener.link);
    cursor_surface = nullptr;
  }

  if (!surface_res) {
    cursor_hidden = true;
    apply_cursor_to_focus();
    WW_LOG_DEBUG("[input] set_cursor: cursor hidden");
    return;
  }

  cursor_hidden = false;
  if (cursor_surface != surface_res) {
    cursor_surface = surface_res;
    cursor_surface_listener.notify = on_cursor_surface_destroy;
    wl_resource_add_destroy_listener(surface_res, &cursor_surface_listener);
  }

  surfaces->set_cursor_surface(surface_res, hotspot_x, hotspot_y,
                               [this] { on_cursor_image_changed(); });

  // A surface that already carries content (committed before this request,
  // or re-set with a new hotspot) has its cursor ready now; one that is yet
  // to commit leaves whatever is showing until its commit lands.
  on_cursor_image_changed();

  WW_LOG_DEBUG("[input] set_cursor (surface={}, hotspot=({},{}))",
               static_cast<void *>(surface_res), hotspot_x, hotspot_y);
}

void InputTranslator::add_pointer(wl_resource *resource) {
  impl_->pointers.push_back(resource);
  WW_LOG_DEBUG("[input] pointer resource added (total: {})",
               impl_->pointers.size());
}

void InputTranslator::remove_pointer(wl_resource *resource) {
  std::erase(impl_->pointers, resource);
  if (resource == impl_->ptr_focus)
    impl_->ptr_focus = nullptr;
  WW_LOG_DEBUG("[input] pointer resource removed (total: {})",
               impl_->pointers.size());
}

void InputTranslator::set_cursor(wl_resource *pointer_resource,
                                 wl_resource *surface_resource,
                                 int32_t hotspot_x, int32_t hotspot_y) {
  impl_->set_cursor(pointer_resource, surface_resource, hotspot_x, hotspot_y);
}

} // namespace ww
