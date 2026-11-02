#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include "wwindows/adapters/x11/policy/move_resize.hpp"
#include <wwindows/support/log.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <xcb/present.h>
#include <xcb/render.h>
#include <xcb/shape.h>

namespace ww {

uint32_t X11Backend::Impl::create_window_impl(Impl &im, xcb_window_t parent,
                                              int16_t x, int16_t y,
                                              uint16_t width,
                                              uint16_t height) {
  xcb_window_t win = xcb_generate_id(im.conn);
  const bool is_toplevel = (parent == im.screen->root);

  // FOCUS_CHANGE only matters on windows the window manager can actually
  // hand real X input focus to. That's top-levels only: a subsurface's
  // child window is never reparented or otherwise managed by the WM at
  // all (see forget_window's comment), so it is never a focus target in
  // its own right — selecting for the event there would just be inert
  // traffic on every child window a client happens to create.
  const uint32_t event_mask =
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
      XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
      XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
      XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_KEY_PRESS |
      XCB_EVENT_MASK_KEY_RELEASE |
      // PROPERTY_CHANGE on a toplevel is how the window manager's
      // _NET_WM_STATE updates (maximized, fullscreen) reach us.
      (is_toplevel ? static_cast<uint32_t>(XCB_EVENT_MASK_FOCUS_CHANGE |
                                           XCB_EVENT_MASK_PROPERTY_CHANGE)
                   : 0u);
  if (is_toplevel)
    im.window.toplevels.insert(win);

  if (im.window.visual32) {
    // Values must follow the ascending bit order of the mask flags:
    // BACK_PIXEL, BORDER_PIXEL, BIT_GRAVITY, EVENT_MASK, COLORMAP.
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                          XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK |
                          XCB_CW_COLORMAP;
    const uint32_t values[5] = {
        0, // bg pixel transparent
        0, // border pixel transparent
        XCB_GRAVITY_NORTH_WEST, // keep existing contents anchored top-left on resize
        event_mask,
        im.window.colormap32,
    };
    xcb_create_window(im.conn, 32, win, parent, x, y, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, im.window.visual32, mask,
                      values);
    WW_LOG_DEBUG("[x11] create 32-bit {} {} (parent={}, pos=({},{}), size={}x{})",
                 is_toplevel ? "window" : "child window", win, parent, x, y,
                 width, height);
    im.window.window_depths[win] = 32;
  } else {
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK;
    const uint32_t values[3] = {
        im.screen->white_pixel,
        XCB_GRAVITY_NORTH_WEST, // keep existing contents anchored top-left on resize
        event_mask,
    };

    xcb_create_window(im.conn, XCB_COPY_FROM_PARENT, win, parent, x, y,
                      width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      im.screen->root_visual, mask, values);
    WW_LOG_DEBUG("[x11] create 24-bit {} {} (parent={}, pos=({},{}), size={}x{})",
                 is_toplevel ? "window" : "child window", win, parent, x, y,
                 width, height);
    im.window.window_depths[win] = static_cast<uint8_t>(im.screen->root_depth);
  }

  if (im.present.has_dri3_present) {
    const xcb_present_event_t eid =
        static_cast<xcb_present_event_t>(xcb_generate_id(im.conn));
    constexpr uint32_t present_mask =
        XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
        XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY;
    // Fire-and-forget rather than xcb_request_check(): this runs on every
    // window creation, i.e. from inside live Wayland-request dispatch,
    // right when a window manager is likely reacting to the newly mapped
    // window — exactly the situation (see set_window_title's comment)
    // where blocking on an X11 reply can freeze the whole compositor
    // until the WM's server grab clears. Present was already confirmed
    // available and `win` is a window we just created ourselves, so
    // failure here is not a realistic outcome in practice; if it does
    // fail, the async X error still reaches dispatch_one's generic
    // protocol-error log above, just without the window-specific detail
    // the checked version used to add.
    xcb_present_select_input(im.conn, eid, win, present_mask);
    im.present.present_events[win] = eid;
    WW_LOG_DEBUG("[x11-present] select_input requested (window={}, eid={}, mask=0x{:x})",
                 win, eid, present_mask);
  }

  // Destination Pictures only serve the dmabuf Render path, which needs DRI3
  // as well -- Render is also initialised for pointer cursors on servers
  // without DRI3, and those windows must not pay for a Picture they never use.
  if (im.present.has_render && im.present.has_dri3_present) {
    // Eager, fire-and-forget — see X11Backend_render.cpp's file comment
    // for why this can't be lazy the way ensure_window_picture() in a
    // more "obvious" design would do it. The PictFormat matching each
    // visual was already confirmed to exist by init_render(); if for some
    // reason it's 0 (Render came up without a usable one for this
    // specific visual), skip creating a Picture at all rather than send a
    // request certain to fail — present_pixmap_via_render() already
    // degrades gracefully (logs a warning, presents nothing) when a
    // window has no entry in window_pictures.
    const xcb_render_pictformat_t fmt =
        im.window.visual32 ? im.present.render_visual32_pictformat : im.present.render_root_pictformat;
    if (fmt) {
      const xcb_render_picture_t pic = xcb_generate_id(im.conn);
      xcb_render_create_picture(im.conn, pic, static_cast<xcb_drawable_t>(win),
                                fmt, 0, nullptr);
      im.present.window_pictures[win] = pic;
      WW_LOG_DEBUG("[x11-render] destination Picture requested (window={}, picture={}, format={})",
                   win, pic, fmt);
    }
  }

  // WM_PROTOCOLS/WM_DELETE_WINDOW only matters for windows the WM actually
  // manages (children of root). A subsurface's child window is never
  // reparented by the WM — it moves and clips with its parent for free —
  // so it has nothing to negotiate a close with.
  if (is_toplevel && im.window.wm_protocols_atom && im.window.wm_delete_atom) {
    xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, win,
                        im.window.wm_protocols_atom, XCB_ATOM_ATOM, 32, 1,
                        &im.window.wm_delete_atom);
  }
  return win;
}

uint32_t X11Backend::create_window(uint16_t width, uint16_t height) {
  return Impl::create_window_impl(*impl_, impl_->screen->root, 0, 0, width,
                                  height);
}

uint32_t X11Backend::create_child_window(uint32_t parent, int16_t x,
                                         int16_t y, uint16_t width,
                                         uint16_t height) {
  return Impl::create_window_impl(
      *impl_, static_cast<xcb_window_t>(parent), x, y, width, height);
}

void X11Backend::move_window(uint32_t window, int16_t x, int16_t y) {
  if (window == 0)
    return;
  const uint32_t values[2] = {static_cast<uint32_t>(static_cast<int32_t>(x)),
                              static_cast<uint32_t>(static_cast<int32_t>(y))};
  xcb_configure_window(impl_->conn, window,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
  xcb_flush(impl_->conn);
  WW_LOG_DEBUG("[x11] move window {} to ({}, {})", window, x, y);
}

void X11Backend::restack_window(uint32_t window, uint32_t sibling,
                                bool above) {
  if (window == 0)
    return;
  uint16_t mask = 0;
  uint32_t values[2];
  int n = 0;
  if (sibling) {
    mask |= XCB_CONFIG_WINDOW_SIBLING;
    values[n++] = sibling;
  }
  mask |= XCB_CONFIG_WINDOW_STACK_MODE;
  values[n++] = above ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW;
  xcb_configure_window(impl_->conn, window, mask, values);
  xcb_flush(impl_->conn);
  WW_LOG_DEBUG("[x11] restack window {} {} (sibling={})", window,
               above ? "above" : "below", sibling);
}

void X11Backend::destroy_window(uint32_t window) {
  clear_retained_frame(window);
  // A surface can disappear before Present CompleteNotify or the Render
  // round-trip fence arrives. Drop callbacks targeting this window; their
  // owning FrameCallbackState objects cancel and destroy undelivered Wayland
  // callback resources rather than waiting forever for an event after teardown.
  for (auto it = impl_->present.present_frame_callbacks.begin();
       it != impl_->present.present_frame_callbacks.end();) {
    if (it->second.window == window)
      it = impl_->present.present_frame_callbacks.erase(it);
    else
      ++it;
  }
  for (std::size_t i = 0; i < impl_->present.pending_frame_fences.size();) {
    if (impl_->present.pending_frame_fences[i].cancel_on_window_destroy &&
        impl_->present.pending_frame_fences[i].window == window) {
      impl_->present.pending_frame_fences[i] =
          std::move(impl_->present.pending_frame_fences.back());
      impl_->present.pending_frame_fences.pop_back();
    } else {
      ++i;
    }
  }
  if (auto it = impl_->present.present_events.find(window); it != impl_->present.present_events.end()) {
    // Fire-and-forget, same reasoning as create_window_impl's select_input
    // above — and doubly so here, since the window is being destroyed on
    // the very next line regardless of whether this clear succeeds.
    xcb_present_select_input(impl_->conn, it->second, window, 0);
    WW_LOG_DEBUG("[x11-present] cleared select_input (window={}, eid={})",
                 window, it->second);
    impl_->present.present_events.erase(it);
  }

  // Free the cached GC before destroying the window
  if (auto it = impl_->window.gcs.find(window); it != impl_->window.gcs.end()) {
    xcb_free_gc(impl_->conn, it->second);
    impl_->window.gcs.erase(it);
  }

  // Free this window's SHM segments. Unlike the GC/Picture frees above,
  // this also happens in forget_window() below -- see that comment for why.
  if (auto it = impl_->shm.window_shm.find(window); it != impl_->shm.window_shm.end()) {
    free_shm_state(impl_->conn, it->second.buffers[0]);
    free_shm_state(impl_->conn, it->second.buffers[1]);
    impl_->shm.window_shm.erase(it);
  }

  if (auto it = impl_->present.window_pictures.find(window);
      it != impl_->present.window_pictures.end()) {
    // Fire-and-forget, same reasoning as the Present select_input clear
    // just above — the window is being destroyed on the next line
    // regardless of whether this succeeds.
    xcb_render_free_picture(impl_->conn, it->second);
    impl_->present.window_pictures.erase(it);
  }
  impl_->window.toplevels.erase(window);

  impl_->window.window_depths.erase(window);

  // WM_TRANSIENT_FOR (dialog-of-another-toplevel) reparenting: if other
  // windows are transient for the one being destroyed, they would
  // otherwise be left with a WM_TRANSIENT_FOR naming a window that no
  // longer exists. ICCCM 4.1.2.6 asks that a dialog whose owner
  // disappears be re-parented to that owner's own owner — the
  // grandparent, which may itself be "none" — rather than simply
  // dropping the property (which would make the dialog a top-level
  // window in its own right, losing the "dialog of *something*"
  // relationship it never asked to lose). Snapshot the affected children
  // before mutating the map: set_window_transient_for() below can erase
  // an entry (parent == 0 case), which would otherwise invalidate the
  // iterator this loop is walking.
  const uint32_t grandparent = [&] {
    auto it = impl_->window.transient_parents.find(window);
    return it != impl_->window.transient_parents.end() ? it->second : 0u;
  }();
  std::vector<uint32_t> children;
  for (const auto &[child, parent] : impl_->window.transient_parents) {
    if (parent == window)
      children.push_back(child);
  }
  impl_->window.transient_parents.erase(window);
  for (uint32_t child : children) {
    set_window_transient_for(child, grandparent);
    WW_LOG_DEBUG("[x11] window {} destroyed — reparented transient child {} "
                 "to grandparent {}",
                 window, child, grandparent);
  }

  WW_LOG_DEBUG("[x11] destroy window {}", window);
  xcb_destroy_window(impl_->conn, window);
  const auto cleanup_fence = xcb_get_input_focus(impl_->conn);
  impl_->present.pending_frame_fences.push_back(PendingFrameFence{
      window, cleanup_fence.sequence,
      [im = impl_.get(), window]() {
        Impl::release_retired_pixmaps_for_window(*im, window);
      }, false});
  xcb_flush(impl_->conn);
}

void X11Backend::forget_window(uint32_t window) {
  clear_retained_frame(window);
  for (auto it = impl_->present.present_frame_callbacks.begin();
       it != impl_->present.present_frame_callbacks.end();) {
    if (it->second.window == window)
      it = impl_->present.present_frame_callbacks.erase(it);
    else
      ++it;
  }
  for (std::size_t i = 0; i < impl_->present.pending_frame_fences.size();) {
    if (impl_->present.pending_frame_fences[i].cancel_on_window_destroy &&
        impl_->present.pending_frame_fences[i].window == window) {
      impl_->present.pending_frame_fences[i] =
          std::move(impl_->present.pending_frame_fences.back());
      impl_->present.pending_frame_fences.pop_back();
    } else {
      ++i;
    }
  }
  // Same bookkeeping cleanup as destroy_window(), minus any request sent to
  // the server that names the (already-gone) window itself — the window is
  // already gone, cascade-destroyed by X along with a parent this backend
  // destroyed directly.
  //
  // This window's SHM segments are the exception: they're real host
  // memory, addressed by their own segment id rather than the window's, so
  // freeing them (xcb_shm_detach names the segment, not the window) is both
  // safe here and necessary — like the GC and Picture freed below, leaving them
  // mapped would leak real memory for the rest of this backend's life
  // every time a subsurface or popup is cascade-destroyed with its parent,
  // which is routine, frequent traffic.
  if (auto it = impl_->shm.window_shm.find(window); it != impl_->shm.window_shm.end()) {
    free_shm_state(impl_->conn, it->second.buffers[0]);
    free_shm_state(impl_->conn, it->second.buffers[1]);
    impl_->shm.window_shm.erase(it);
  }
  // A GC is a resource of its own: the server does not free it when the
  // drawable it was made for is destroyed, so erasing the map entry alone
  // leaked one per cascade-destroyed window. The request names the GC, not the
  // (gone) window, so it is safe to send here.
  //
  // A Render Picture made for a *window* is the opposite case: the server
  // frees it together with the window (PictureDestroyWindow), so by the time
  // this runs it is already gone. Sending FreePicture anyway raised a
  // BadPicture error for every cascade-destroyed subsurface/popup that had one
  // (seen as "protocol error ... RENDER FreePicture"). Only the map entry is
  // dropped below. (destroy_window() frees the Picture *before* destroying the
  // window, which is the order that is valid.)
  if (auto it = impl_->window.gcs.find(window); it != impl_->window.gcs.end())
    xcb_free_gc(impl_->conn, it->second);
  impl_->present.present_events.erase(window);
  impl_->window.gcs.erase(window);
  impl_->present.window_pictures.erase(window);
  impl_->window.window_depths.erase(window);
  impl_->window.toplevels.erase(window);
  const auto cleanup_fence = xcb_get_input_focus(impl_->conn);
  impl_->present.pending_frame_fences.push_back(PendingFrameFence{
      window, cleanup_fence.sequence,
      [im = impl_.get(), window]() {
        Impl::release_retired_pixmaps_for_window(*im, window);
      }, false});
  xcb_flush(impl_->conn);
  WW_LOG_DEBUG("[x11] forget window {} (already destroyed with its parent)",
               window);
}

void X11Backend::map_window(uint32_t window) {
  WW_LOG_DEBUG("[x11] map window {}", window);
  xcb_map_window(impl_->conn, window);
  xcb_flush(impl_->conn);
}

void X11Backend::unmap_window(uint32_t window) {
  if (!window)
    return;
  WW_LOG_DEBUG("[x11] unmap window {}", window);
  xcb_unmap_window(impl_->conn, window);
  xcb_flush(impl_->conn);
}

void X11Backend::set_window_title(uint32_t window, const char *utf8_title) {
  if (!utf8_title || window == 0)
    return;

  auto &im = *impl_;

  // Deliberately uses only atoms cached at setup time (intern_wm_atoms,
  // intern_clipboard_atoms) plus the core-protocol predefined WM_NAME —
  // never a live xcb_intern_atom_reply() here. This function runs from
  // inside live Wayland-request dispatch (xdg_toplevel.set_title), right
  // after the window has just been mapped — exactly when a real window
  // manager is likely to be reparenting/framing it, which on ICCCM-
  // compliant WMs (confirmed with twm) routinely involves a server grab.
  // While that grab is held, the X server won't service any request on
  // *our* connection either, so a blocking XCB call here would freeze the
  // entire compositor — not just this window — until the WM's grab clears.
  // Avoiding any live round-trip sidesteps that class of hang entirely.
  const xcb_atom_t atom_utf8 = im.clipboard.atom_utf8_string;
  const xcb_atom_t atom_net_wm = im.window.net_wm_name_atom;
  const auto len = static_cast<uint32_t>(std::strlen(utf8_title));

  if (atom_net_wm && atom_utf8)
    xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window, atom_net_wm,
                        atom_utf8, 8, len, utf8_title);
  xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window,
                      static_cast<xcb_atom_t>(XCB_ATOM_WM_NAME),
                      static_cast<xcb_atom_t>(XCB_ATOM_STRING), 8, len,
                      utf8_title);

  xcb_flush(im.conn);
  WW_LOG_DEBUG("[x11] set window {} title to \"{}\"", window, utf8_title);
}

// --- Window identity and hints ----------------------------------------------
//
// Everything below follows set_window_title()'s rule: cached atoms only
// (every property here except _NET_WM_PID is a predefined atom, so there is
// nothing to intern at all), and no request that waits for a reply. These
// run from inside live Wayland-request dispatch, usually while a window
// manager is busy with the window being set up.

void X11Backend::set_window_class(uint32_t window, const char *instance_name,
                                  const char *class_name) {
  if (window == 0)
    return;
  auto &im = *impl_;

  if (!instance_name || !*instance_name || !class_name || !*class_name) {
    xcb_delete_property(im.conn, window, XCB_ATOM_WM_CLASS);
    xcb_flush(im.conn);
    return;
  }

  // ICCCM 4.1.2.5: two NUL-terminated strings back to back, instance first,
  // and the terminators are part of the property's length.
  std::string value;
  value.reserve(std::strlen(instance_name) + std::strlen(class_name) + 2);
  value.append(instance_name).push_back('\0');
  value.append(class_name).push_back('\0');

  xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(value.size()), value.data());
  xcb_flush(im.conn);
  WW_LOG_DEBUG("[x11] set window {} class to \"{}\" / \"{}\"", window,
               instance_name, class_name);
}

void X11Backend::set_window_size_limits(uint32_t window,
                                        const WindowSizeLimits &limits) {
  if (window == 0)
    return;
  auto &im = *impl_;

  if (!limits.any()) {
    xcb_delete_property(im.conn, window, XCB_ATOM_WM_NORMAL_HINTS);
    xcb_flush(im.conn);
    return;
  }

  // WM_SIZE_HINTS (ICCCM 4.1.2.3) is 18 CARD32s: flags, four obsolete
  // position/size fields, then min, max, increments, aspect, base, gravity.
  // Only min and max are ever filled in here.
  constexpr uint32_t kPMinSize = 1u << 4;
  constexpr uint32_t kPMaxSize = 1u << 5;
  // X11 has no "unbounded" for a maximum, only a very large one. Window
  // dimensions are 16-bit on the wire; stay inside the signed range so a
  // window manager doing its arithmetic in int16 cannot wrap.
  constexpr uint32_t kUnbounded = 32767;

  const auto clamp = [](int32_t v) {
    return static_cast<uint32_t>(std::clamp<int32_t>(v, 0, 65535));
  };

  std::array<uint32_t, 18> hints{};
  if (limits.min_width > 0 || limits.min_height > 0) {
    hints[0] |= kPMinSize;
    hints[5] = clamp(limits.min_width);
    hints[6] = clamp(limits.min_height);
  }
  if (limits.max_width > 0 || limits.max_height > 0) {
    hints[0] |= kPMaxSize;
    hints[7] = limits.max_width > 0 ? clamp(limits.max_width) : kUnbounded;
    hints[8] = limits.max_height > 0 ? clamp(limits.max_height) : kUnbounded;
  }

  xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32,
                      static_cast<uint32_t>(hints.size()), hints.data());
  xcb_flush(im.conn);
  WW_LOG_DEBUG("[x11] set window {} size limits (min={}x{}, max={}x{})", window,
               limits.min_width, limits.min_height, limits.max_width,
               limits.max_height);
}

void X11Backend::set_window_transient_for(uint32_t window, uint32_t parent) {
  if (window == 0)
    return;
  auto &im = *impl_;

  if (parent == 0) {
    xcb_delete_property(im.conn, window, XCB_ATOM_WM_TRANSIENT_FOR);
    im.window.transient_parents.erase(window);
  } else {
    const xcb_window_t value = parent;
    xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window,
                        XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1,
                        &value);
    im.window.transient_parents[window] = parent;
  }
  xcb_flush(im.conn);
  WW_LOG_DEBUG("[x11] set window {} transient for {}", window, parent);
}

void X11Backend::set_window_client_process(uint32_t window, uint32_t pid) {
  if (window == 0 || pid == 0)
    return;
  auto &im = *impl_;

  if (im.window.net_wm_pid_atom)
    xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window,
                        im.window.net_wm_pid_atom, XCB_ATOM_CARDINAL, 32, 1, &pid);
  // _NET_WM_PID is only meaningful together with the host it is a pid *on*
  // (EWMH says a window manager must check WM_CLIENT_MACHINE before it
  // acts on the pid).
  if (!im.window.client_machine.empty())
    xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, window,
                        XCB_ATOM_WM_CLIENT_MACHINE, XCB_ATOM_STRING, 8,
                        static_cast<uint32_t>(im.window.client_machine.size()),
                        im.window.client_machine.data());
  xcb_flush(im.conn);
  WW_LOG_DEBUG("[x11] set window {} process to pid {} on \"{}\"", window, pid,
               im.window.client_machine);
}

std::vector<uint8_t> X11Backend::query_pressed_keycodes() {
  auto &im = *impl_;
  std::vector<uint8_t> out;

  // xcb_query_keymap is a core-protocol request (no extension, no
  // capability check needed) returning a 256-bit bitmap: byte N, bit M
  // set means keycode (N*8 + M) is currently physically down. A blocking
  // round trip, but only ever issued on a real keyboard-focus transition
  // (InputTranslator::Impl::on_focus_in) — a human-timescale event, not
  // hot-path — so it costs nothing to wait for, and unlike a request
  // issued mid-window-setup (see set_window_title's comment) there is no
  // plausible window-manager server grab this could collide with: focus
  // transitions happen well after mapping/reparenting settle.
  xcb_query_keymap_cookie_t cookie = xcb_query_keymap(im.conn);
  xcb_query_keymap_reply_t *reply =
      xcb_query_keymap_reply(im.conn, cookie, nullptr);
  if (!reply) {
    WW_LOG_WARN("[x11] query_pressed_keycodes: xcb_query_keymap failed");
    return out;
  }

  for (int byte = 0; byte < 32; ++byte) {
    const uint8_t bits = reply->keys[byte];
    if (!bits)
      continue;
    for (int bit = 0; bit < 8; ++bit) {
      if (bits & (1u << bit))
        out.push_back(static_cast<uint8_t>(byte * 8 + bit));
    }
  }
  ::free(reply);
  return out;
}

void X11Backend::set_input_shape(uint32_t window, bool has_region,
                                 const std::vector<InputRect> &add_rects,
                                 const std::vector<InputRect> &sub_rects) {
  if (window == 0 || !impl_->has_shape)
    return;

  if (!has_region) {
    // No region was ever declared: accept input over the whole window,
    // i.e. no shape restriction at all. This is the *only* case this
    // early return is for — see the has_region == true / empty case
    // just below, which must NOT take this path.
    xcb_shape_mask(impl_->conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, window,
                   0, 0, XCB_PIXMAP_NONE);
    xcb_flush(impl_->conn);
    WW_LOG_TRACE("[x11] input shape reset to whole window (window={})",
                window);
    return;
  }

  if (add_rects.empty() && sub_rects.empty()) {
    // A region *was* declared, and it is empty: per the Wayland spec this
    // means the surface accepts no input at all — the standard idiom for
    // a purely-visual overlay/subsurface that wants clicks and keys to
    // fall through to whatever sits beneath it. SO_SET with a zero-length
    // rectangle list is a real, distinctly-empty shape (confirmed via
    // ShapeGetRectangles against a live server: it reports count=0, versus
    // count=1/whole-window for both "never touched" and the shape_mask(
    // None) reset above) — it is not a no-op, and must not be folded into
    // the has_region == false branch above the way it used to be.
    xcb_shape_rectangles(impl_->conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                         XCB_CLIP_ORDERING_UNSORTED, window, 0, 0, 0,
                         nullptr);
    xcb_flush(impl_->conn);
    WW_LOG_TRACE("[x11] input shape set to empty (window={})", window);
    return;
  }

  std::vector<xcb_rectangle_t> adds;
  adds.reserve(add_rects.size());
  for (const auto &r : add_rects)
    adds.push_back({r.x, r.y, r.width, r.height});

  if (!adds.empty()) {
    // SO_SET with the full additive list *replaces* the input shape with
    // their union — exactly wl_region's "start empty, add rects" semantics
    // collapsed into one call.
    xcb_shape_rectangles(impl_->conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                         XCB_CLIP_ORDERING_UNSORTED, window, 0, 0,
                         static_cast<uint32_t>(adds.size()), adds.data());
  } else {
    // Only subtracts were recorded (a region built from subtract() alone,
    // with no prior add()) — start from "whole window" so there is
    // something to subtract from.
    xcb_shape_mask(impl_->conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, window,
                   0, 0, XCB_PIXMAP_NONE);
  }

  if (!sub_rects.empty()) {
    std::vector<xcb_rectangle_t> subs;
    subs.reserve(sub_rects.size());
    for (const auto &r : sub_rects)
      subs.push_back({r.x, r.y, r.width, r.height});
    xcb_shape_rectangles(impl_->conn, XCB_SHAPE_SO_SUBTRACT,
                         XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         window, 0, 0, static_cast<uint32_t>(subs.size()),
                         subs.data());
  }

  xcb_flush(impl_->conn);
  WW_LOG_DEBUG(
      "[x11] input shape applied (window={}, add_rects={}, sub_rects={})",
      window, add_rects.size(), sub_rects.size());
}

void X11Backend::set_window_state(uint32_t window, WindowState state,
                                  bool enabled) {
  auto &im = *impl_;
  if (window == 0 || !im.window.net_wm_state_atom)
    return;

  // Two atoms for maximize (EWMH splits it per axis), one for fullscreen.
  xcb_atom_t first = XCB_ATOM_NONE;
  xcb_atom_t second = XCB_ATOM_NONE;
  switch (state) {
  case WindowState::Fullscreen:
    first = im.window.net_wm_state_fullscreen_atom;
    break;
  case WindowState::Maximized:
    first = im.window.net_wm_state_maximized_vert_atom;
    second = im.window.net_wm_state_maximized_horz_atom;
    break;
  }
  if (!first) {
    WW_LOG_DEBUG("[x11] no atom for requested window state — ignoring "
                 "(window={})",
                 window);
    return;
  }

  // EWMH _NET_WM_STATE is a *request to the window manager*, not a property
  // we may set ourselves once the window is mapped: the spec is explicit
  // that a mapped window must ask via a client message sent to the root
  // with SubstructureRedirect, so the WM stays the single authority on
  // geometry. Which is the whole point here — the WM is what shrinks a
  // toplevel to its work area (screen minus panels/bars), and asking it to
  // go fullscreen is the only way a client gets the whole screen back.
  //
  // No reply and no confirmation: the WM may refuse outright. Whatever
  // size it settles on arrives later as an ordinary ConfigureNotify, and
  // goes to the client through the same request_resize path as any other
  // resize. Fire-and-forget for the same reason every other request in
  // this file is — this runs from inside live Wayland-request dispatch.
  xcb_client_message_event_t ev{};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.sequence = 0;
  ev.window = static_cast<xcb_window_t>(window);
  ev.type = im.window.net_wm_state_atom;
  ev.data.data32[0] = enabled ? 1u : 0u; // _NET_WM_STATE_ADD / _REMOVE
  ev.data.data32[1] = first;
  ev.data.data32[2] = second;
  ev.data.data32[3] = 1u; // source indication: normal application
  ev.data.data32[4] = 0u;

  xcb_send_event(im.conn, /*propagate=*/0, im.screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char *>(&ev));
  xcb_flush(im.conn);

  WW_LOG_DEBUG("[x11] _NET_WM_STATE {} requested (window={}, atoms={},{})",
               enabled ? "add" : "remove", window, first, second);
}

void X11Backend::Impl::send_net_wm_moveresize(Impl &im, uint32_t window,
                                              uint32_t direction) {
  if (window == 0 || !im.window.net_wm_moveresize_atom)
    return;

  // One-off blocking round trip, same justification as
  // query_pressed_keycodes()'s: this only ever runs in response to a
  // real pointer button press a client is turning into an
  // xdg_toplevel.move/resize request — a human-timescale event, not a
  // hot path — and there is no window-manager grab in flight for it to
  // collide with (unlike set_window_title's mid-setup concern).
  xcb_query_pointer_cookie_t cookie =
      xcb_query_pointer(im.conn, im.screen->root);
  xcb_query_pointer_reply_t *reply =
      xcb_query_pointer_reply(im.conn, cookie, nullptr);

  int16_t root_x = 0;
  int16_t root_y = 0;
  uint32_t button = 0;
  if (reply) {
    root_x = reply->root_x;
    root_y = reply->root_y;
    // EWMH's "button" field wants a single X11 button number. Take the
    // lowest-numbered of the ordinary click buttons currently held —
    // wheel/side buttons never start an interactive move/resize grab, so
    // they're not considered here. Zero (no button reported) is a legal
    // value the spec explicitly allows for a keyboard-initiated request.
    if (reply->mask & XCB_BUTTON_MASK_1)
      button = 1;
    else if (reply->mask & XCB_BUTTON_MASK_2)
      button = 2;
    else if (reply->mask & XCB_BUTTON_MASK_3)
      button = 3;
    ::free(reply);
  } else {
    WW_LOG_WARN("[x11] _NET_WM_MOVERESIZE: xcb_query_pointer failed "
                "(window={})",
                window);
  }

  // Same "ask the window manager, don't do it ourselves" reasoning as
  // set_window_state() above — see its comment for why.
  xcb_client_message_event_t ev{};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.sequence = 0;
  ev.window = static_cast<xcb_window_t>(window);
  ev.type = im.window.net_wm_moveresize_atom;
  ev.data.data32[0] = static_cast<uint32_t>(static_cast<uint16_t>(root_x));
  ev.data.data32[1] = static_cast<uint32_t>(static_cast<uint16_t>(root_y));
  ev.data.data32[2] = direction;
  ev.data.data32[3] = button;
  ev.data.data32[4] = 1u; // source indication: normal application

  xcb_send_event(im.conn, /*propagate=*/0, im.screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char *>(&ev));
  xcb_flush(im.conn);

  WW_LOG_DEBUG("[x11] _NET_WM_MOVERESIZE requested (window={}, "
               "direction={}, root=({},{}), button={})",
               window, direction, root_x, root_y, button);
}

void X11Backend::request_interactive_move(uint32_t window) {
  Impl::send_net_wm_moveresize(*impl_, window,
                               x11::policy::net_wm_moveresize_move_direction);
}

void X11Backend::request_interactive_resize(uint32_t window,
                                            ResizeEdge edge) {
  Impl::send_net_wm_moveresize(
      *impl_, window, x11::policy::net_wm_moveresize_direction(edge));
}

void X11Backend::minimize_window(uint32_t window) {
  auto &im = *impl_;
  if (window == 0 || !im.window.wm_change_state_atom)
    return;

  // ICCCM 4.1.4: WM_CHANGE_STATE(IconicState) is a client's way of
  // asking to be iconified — sent to the root the same way
  // _NET_WM_STATE and _NET_WM_MOVERESIZE are, for the same reason: a
  // mapped window is not allowed to just set this on itself, only ask.
  xcb_client_message_event_t ev{};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.sequence = 0;
  ev.window = static_cast<xcb_window_t>(window);
  ev.type = im.window.wm_change_state_atom;
  ev.data.data32[0] = 3u; // IconicState

  xcb_send_event(im.conn, /*propagate=*/0, im.screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char *>(&ev));
  xcb_flush(im.conn);

  WW_LOG_DEBUG("[x11] WM_CHANGE_STATE(Iconic) requested (window={})",
               window);
}

void X11Backend::resize_window(uint32_t window, uint16_t width, uint16_t height) {
  if (window == 0)
    return;
  uint32_t values[2];
  values[0] = static_cast<uint32_t>(width);
  values[1] = static_cast<uint32_t>(height);
  xcb_configure_window(impl_->conn, window,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       values);
  xcb_flush(impl_->conn);
  WW_LOG_DEBUG("[x11] resize window {} to {}x{}", window, width, height);
}

} // namespace ww
