#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include <wwindows/support/frame_diag.hpp>
#include <wwindows/support/log.hpp>

#include <cstdlib>
#include <utility>
#include <xcb/present.h>
#include <xcb/dri3.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <string>
#include <xcb/xcbext.h> // xcb_poll_for_reply, xcb_discard_reply
#include <xcb/randr.h>
#include <xcb/xfixes.h>
// xcb/xkb.h's generated struct fields include one literally named
// `explicit` (xcb_xkb_set_explicit_t / xcb_xkb_set_map_values_t), a
// reserved word in C++ — the header is C-only as shipped. Scoped
// rename around just this include, same workaround other C++
// codebases pulling in this header use.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

namespace ww {
namespace {

// Names the request that an X protocol error refers to. Extension major
// opcodes are assigned per server, so they are resolved against this
// connection's own extension table rather than hard-coded.
std::string describe_x_request(xcb_connection_t *conn, uint8_t major,
                               uint16_t minor) {
  struct Known {
    xcb_extension_t *id;
    const char *name;
    const char *(*minor_name)(uint16_t);
  };
  static const Known kKnown[] = {
      {&xcb_render_id, "RENDER",
       [](uint16_t m) -> const char * {
         switch (m) {
         case 0: return "QueryVersion";
         case 1: return "QueryPictFormats";
         case 4: return "CreatePicture";
         case 5: return "ChangePicture";
         case 6: return "SetPictureClipRectangles";
         case 7: return "FreePicture";
         case 8: return "Composite";
         case 27: return "CreateCursor";
         case 28: return "SetPictureTransform";
         case 30: return "SetPictureFilter";
         default: return "?";
         }
       }},
      {&xcb_shm_id, "MIT-SHM",
       [](uint16_t m) -> const char * {
         switch (m) {
         case 0: return "QueryVersion";
         case 1: return "Attach";
         case 2: return "Detach";
         case 3: return "PutImage";
         case 4: return "GetImage";
         case 5: return "CreatePixmap";
         case 6: return "AttachFd";
         case 7: return "CreateSegment";
         default: return "?";
         }
       }},
      {&xcb_dri3_id, "DRI3",
       [](uint16_t m) -> const char * {
         switch (m) {
         case 0: return "QueryVersion";
         case 1: return "Open";
         case 2: return "PixmapFromBuffer";
         case 3: return "BufferFromPixmap";
         case 4: return "FenceFromFD";
         case 6: return "GetSupportedModifiers";
         case 7: return "PixmapFromBuffers";
         case 8: return "BuffersFromPixmap";
         default: return "?";
         }
       }},
      {&xcb_present_id, "Present",
       [](uint16_t m) -> const char * {
         switch (m) {
         case 0: return "QueryVersion";
         case 1: return "Pixmap";
         case 2: return "NotifyMSC";
         case 3: return "SelectInput";
         case 4: return "QueryCapabilities";
         default: return "?";
         }
       }},
      {&xcb_xfixes_id, "XFIXES", [](uint16_t) -> const char * { return "?"; }},
      {&xcb_shape_id, "SHAPE", [](uint16_t) -> const char * { return "?"; }},
      {&xcb_xkb_id, "XKEYBOARD", [](uint16_t) -> const char * { return "?"; }},
  };
  if (major < 128)
    return "core request opcode " + std::to_string(major);
  for (const auto &k : kKnown) {
    const auto *data = xcb_get_extension_data(conn, k.id);
    if (data && data->present && data->major_opcode == major)
      return std::string{k.name} + " " + k.minor_name(minor) + " (minor " +
             std::to_string(minor) + ")";
  }
  return "extension major " + std::to_string(major) + " minor " +
         std::to_string(minor);
}

} // namespace
} // namespace ww

namespace ww {

namespace {

// Free helpers that need no access to Impl's private fields.

void log_present_configure(xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_present_configure_notify_event_t *>(ev);
  WW_LOG_TRACE("[x11-present] configure (window={}, size={}x{}, pixmap={}x{}, full_seq={})",
               e->window, e->width, e->height, e->pixmap_width, e->pixmap_height,
               e->full_sequence);
}

void log_present_complete(xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_present_complete_notify_event_t *>(ev);
  // Copy out of the packed struct first: the logging macros bind their
  // arguments by reference, and you cannot bind a reference directly to a
  // packed field.
  const auto window = e->window;
  const auto serial = e->serial;
  const auto kind = static_cast<uint32_t>(e->kind);
  const auto mode = static_cast<uint32_t>(e->mode);
  const auto msc = e->msc;
  const auto ust = e->ust;
  const auto full_sequence = e->full_sequence;
  // Fires once per presented frame — trace only, this is vsync-rate noise.
  WW_LOG_TRACE("[x11-present] complete (window={}, serial={}, kind={}, mode={}, msc={}, ust={}, full_seq={})",
               window, serial, kind, mode, msc, ust, full_sequence);
}

void log_present_idle(xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_present_idle_notify_event_t *>(ev);
  WW_LOG_TRACE("[x11-present] idle (window={}, serial={}, pixmap={}, full_seq={})",
               e->window, e->serial, e->pixmap, e->full_sequence);
}

} // namespace

void X11Backend::Impl::dispatch_client_message(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_client_message_event_t *>(ev);
  if (im.window.wm_protocols_atom && e->type == im.window.wm_protocols_atom) {
    if (static_cast<xcb_atom_t>(e->data.data32[0]) == im.window.wm_delete_atom) {
      WW_LOG_DEBUG("[x11] WM_DELETE_WINDOW received for window {}", e->window);
      if (im.close_cb)
        im.close_cb(e->window);
    }
  }
}

void X11Backend::Impl::dispatch_configure_notify(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_configure_notify_event_t *>(ev);
  WW_LOG_DEBUG("[x11] ConfigureNotify (window={}, size={}x{}, pos={},{})",
               e->window, e->width, e->height, e->x, e->y);
  if (im.on_resize)
    im.on_resize(e->window, e->width, e->height);
}

void X11Backend::Impl::dispatch_enter_notify(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_enter_notify_event_t *>(ev);
  WW_LOG_TRACE("[x11] EnterNotify (window={}, pos={},{}, mode={})", e->event,
               e->event_x, e->event_y, static_cast<int>(e->mode));
  if (e->mode == XCB_NOTIFY_MODE_NORMAL && im.input.input_cbs.on_enter)
    im.input.input_cbs.on_enter(e->event, e->event_x, e->event_y, e->root_x,
                          e->root_y);
}

void X11Backend::Impl::dispatch_leave_notify(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_leave_notify_event_t *>(ev);
  WW_LOG_TRACE("[x11] LeaveNotify (window={}, mode={})", e->event,
               static_cast<int>(e->mode));
  if (e->mode == XCB_NOTIFY_MODE_NORMAL && im.input.input_cbs.on_leave)
    im.input.input_cbs.on_leave(e->event);
}

void X11Backend::Impl::dispatch_motion_notify(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_motion_notify_event_t *>(ev);
  if (im.input.input_cbs.on_motion)
    im.input.input_cbs.on_motion(e->event, e->event_x, e->event_y, e->root_x,
                           e->root_y, e->time);
}

void X11Backend::Impl::dispatch_button_press(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_button_press_event_t *>(ev);
  if (im.input.input_cbs.on_button)
    im.input.input_cbs.on_button(e->event, e->detail, true, e->time);
}

void X11Backend::Impl::dispatch_button_release(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_button_release_event_t *>(ev);
  if (im.input.input_cbs.on_button)
    im.input.input_cbs.on_button(e->event, e->detail, false, e->time);
}

void X11Backend::Impl::dispatch_key_press(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_key_press_event_t *>(ev);
  if (im.input.input_cbs.on_key)
    im.input.input_cbs.on_key(e->event, e->detail, true, e->time);
}

void X11Backend::Impl::dispatch_key_release(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_key_release_event_t *>(ev);
  if (im.input.input_cbs.on_key)
    im.input.input_cbs.on_key(e->event, e->detail, false, e->time);
}

void X11Backend::Impl::dispatch_expose(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_expose_event_t *>(ev);
  // Expose events arrive in bursts (one per damaged rect); trace-only.
  WW_LOG_TRACE("[x11] Expose (window={}, rect={}x{}@{},{} count={})",
               e->window, e->width, e->height, e->x, e->y, e->count);
  if (e->count == 0 && im.input.input_cbs.on_expose) // count==0: last in sequence
    im.input.input_cbs.on_expose(e->window);
}

void X11Backend::Impl::dispatch_present_generic(Impl &im, xcb_generic_event_t *ev) {
  auto *ge = reinterpret_cast<xcb_ge_generic_event_t *>(ev);
  if (ge->extension != im.present.present_opcode)
    return;

  auto *pev = reinterpret_cast<xcb_present_generic_event_t *>(ev);
  switch (pev->evtype) {
  case XCB_PRESENT_EVENT_CONFIGURE_NOTIFY:
    log_present_configure(ev);
    break;
  case XCB_PRESENT_EVENT_COMPLETE_NOTIFY: {
    log_present_complete(ev);
    const auto *complete =
        reinterpret_cast<const xcb_present_complete_notify_event_t *>(ev);
    auto callback = im.present.present_frame_callbacks.find(complete->serial);
    if (callback != im.present.present_frame_callbacks.end()) {
      auto done = std::move(callback->second.callback);
      im.present.present_frame_callbacks.erase(callback);
      if (done)
        done();
    }
    break;
  }
  case XCB_PRESENT_EVENT_IDLE_NOTIFY: {
    log_present_idle(ev);
    const auto *idle =
        reinterpret_cast<const xcb_present_idle_notify_event_t *>(ev);
    // Present tells us when a pixmap is no longer being read by the server.
    // Retained snapshots can be replaced before that point, but must not be
    // freed until they leave this in-flight set.
    im.present.present_in_flight_pixmaps.erase(idle->pixmap);
    if (im.present.retired_retained_pixmaps.contains(idle->pixmap))
      X11Backend::Impl::free_pixmap_resource(im, idle->pixmap);
    break;
  }
  default:
    WW_LOG_DEBUG("[x11-present] unhandled generic event (evtype={})",
                 pev->evtype);
    break;
  }
}

void X11Backend::Impl::dispatch_focus_in(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_focus_in_event_t *>(ev);
  WW_LOG_TRACE("[x11] FocusIn (window={}, mode={}, detail={})", e->event,
               static_cast<int>(e->mode), static_cast<int>(e->detail));
  // NotifyGrab/NotifyUngrab bracket an active keyboard grab itself (see
  // IWindowBackend::query_pressed_keycodes' doc comment for what that is
  // and why it matters here) rather than a real focus change the window
  // manager intends — filtered out exactly like Enter/LeaveNotify's own
  // mode check above, for the same reason.
  if (e->mode == XCB_NOTIFY_MODE_NORMAL && im.input.input_cbs.on_focus_in)
    im.input.input_cbs.on_focus_in(e->event);
}

void X11Backend::Impl::dispatch_focus_out(Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_focus_out_event_t *>(ev);
  WW_LOG_TRACE("[x11] FocusOut (window={}, mode={}, detail={})", e->event,
               static_cast<int>(e->mode), static_cast<int>(e->detail));
  if (e->mode == XCB_NOTIFY_MODE_NORMAL && im.input.input_cbs.on_focus_out)
    im.input.input_cbs.on_focus_out(e->event);
}

void X11Backend::Impl::deliver_xkb_state_notify(
    Impl &im, const PendingXkbStateNotify &state) {
  // Reconcile the causal key transition first. If its core event was delivered,
  // the input translator already updated the logical key set and this is a
  // no-op. If a grab swallowed a modifier release, it emits the missing
  // wl_keyboard.key release before the modifier snapshot below.
  if (state.keycode != 0 && im.input.input_cbs.on_xkb_key_transition &&
      (state.event_type == XCB_KEY_PRESS ||
       state.event_type == XCB_KEY_RELEASE)) {
    im.input.input_cbs.on_xkb_key_transition(
        state.keycode, state.event_type == XCB_KEY_PRESS, state.time);
  }
  if (im.input.input_cbs.on_xkb_state_changed)
    im.input.input_cbs.on_xkb_state_changed(
        state.depressed_mods, state.latched_mods, state.locked_mods,
        state.effective_group);
}

void X11Backend::Impl::flush_pending_xkb_state_notifies(Impl &im) {
  while (!im.input.pending_xkb_state_notifies.empty()) {
    const PendingXkbStateNotify state =
        im.input.pending_xkb_state_notifies.front();
    im.input.pending_xkb_state_notifies.pop_front();
    deliver_xkb_state_notify(im, state);
  }
}

void X11Backend::Impl::dispatch_randr_screen_change(Impl &im,
                                                    xcb_generic_event_t *ev) {
  const auto *e = reinterpret_cast<const xcb_randr_screen_change_notify_event_t *>(ev);
  const ScreenGeometry now{e->width, e->height, e->mwidth, e->mheight,
                           query_refresh_mhz(im)};
  // One logical change (an `xrandr --output ... --mode ...`) can raise
  // several notifications; only a real difference is worth passing on.
  if (now == im.screen_state.geometry)
    return;
  im.screen_state.geometry = now;
  WW_LOG_INFO("[x11] screen is now {}x{}px, {}x{}mm, {} mHz", now.width_px,
              now.height_px, now.width_mm, now.height_mm, now.refresh_mhz);
  if (im.screen_state.on_changed)
    im.screen_state.on_changed(now);
}

void X11Backend::Impl::dispatch_xkb_event(Impl &im, xcb_generic_event_t *ev) {
  // Every XKB sub-event (StateNotify, MapNotify, NewKeyboardNotify, ...)
  // shares im.input.xkb_base_event as its response_type; xkbType is what
  // actually discriminates them. Every one of these structs starts with
  // the same {response_type, xkbType, sequence, time, deviceID} prefix,
  // so reading xkbType/deviceID off any of them (xcb_xkb_state_notify_event_t
  // here, arbitrarily) before knowing which one it is is safe.
  const auto *any = reinterpret_cast<xcb_xkb_state_notify_event_t *>(ev);
  if (static_cast<int32_t>(any->deviceID) != im.input.xkb_device_id)
    return; // some other input device; core keyboard only

  if (any->xkbType == XCB_XKB_STATE_NOTIFY) {
    WW_LOG_TRACE("[x11] XkbStateNotify (base={:#x}, latched={:#x}, "
                "locked={:#x}, group={})",
                any->baseMods, any->latchedMods, any->lockedMods, any->group);
    const PendingXkbStateNotify state{
        .time = any->time,
        .keycode = any->keycode,
        .event_type = any->eventType,
        .depressed_mods = any->baseMods,
        .latched_mods = any->latchedMods,
        .locked_mods = any->lockedMods,
        .effective_group = any->group,
    };
    if (state.keycode != 0 &&
        (state.event_type == XCB_KEY_PRESS ||
         state.event_type == XCB_KEY_RELEASE)) {
      // Keep ordering with the core event that caused this notification. It
      // may already have been queued before the XKB event or may be consumed
      // by a passive grab and never reach this connection; both cases are
      // resolved in dispatch_one()/process_events().
      im.input.pending_xkb_state_notifies.push_back(state);
    } else {
      // Programmatic state changes have no paired core key event. Deliver any
      // older pending snapshots first, then this authoritative state.
      flush_pending_xkb_state_notifies(im);
      deliver_xkb_state_notify(im, state);
    }
    return;
  }

  if (any->xkbType == XCB_XKB_MAP_NOTIFY ||
      any->xkbType == XCB_XKB_NEW_KEYBOARD_NOTIFY) {
    flush_pending_xkb_state_notifies(im);
    WW_LOG_DEBUG("[x11] Xkb{}Notify — keymap changed",
                any->xkbType == XCB_XKB_MAP_NOTIFY ? "Map" : "NewKeyboard");
    if (im.input.input_cbs.on_xkb_keymap_changed)
      im.input.input_cbs.on_xkb_keymap_changed();
    return;
  }

  if (any->xkbType == XCB_XKB_CONTROLS_NOTIFY) {
    // Auto-repeat rate/delay (or repeat being toggled) -- and other XKB
    // controls such as sticky keys, which the handler's re-read simply finds
    // unchanged and ignores.
    WW_LOG_DEBUG("[x11] XkbControlsNotify");
    if (im.input.input_cbs.on_xkb_repeat_changed)
      im.input.input_cbs.on_xkb_repeat_changed();
  }
}

void X11Backend::Impl::dispatch_one(Impl &im, xcb_generic_event_t *ev) {
  if (ev->response_type == 0) {
    {
      const auto *diag_err = reinterpret_cast<const xcb_generic_error_t *>(ev);
      ::ww::diag::note(0, ::ww::diag::Ev::XError,
                       static_cast<uint32_t>(diag_err->error_code) |
                           (static_cast<uint32_t>(diag_err->major_code) << 8) |
                           (static_cast<uint32_t>(diag_err->minor_code) << 16));
    }
    auto *err = reinterpret_cast<xcb_generic_error_t *>(ev);
    WW_LOG_ERROR("[x11] protocol error: code={} major={} minor={} seq={} "
                 "request={} resource_id={}",
                 err->error_code, err->major_code, err->minor_code,
                 err->sequence,
                 describe_x_request(im.conn, err->major_code, err->minor_code),
                 err->resource_id);

    // Does this belong to a still-pending DRI3 import? err->sequence is
    // the raw 16-bit wire sequence; request_sequence is libxcb's
    // wrap-extended (32-bit) cookie sequence, so only the low 16 bits are
    // comparable — see PendingDmabufImport's comment for why we're
    // matching this asynchronously at all rather than blocking on it.
    for (size_t i = 0; i < im.present.pending_dmabuf_imports.size(); ++i) {
      auto &pending = im.present.pending_dmabuf_imports[i];
      if ((pending.request_sequence & 0xffffu) != err->sequence)
        continue;

      WW_LOG_WARN("[x11] DRI3 import failed (pixmap={}, error_code={}, "
                  "major={}, minor={})",
                  pending.pixmap, err->error_code, err->major_code,
                  err->minor_code);
      // The fence's reply is now moot — nothing will ever call
      // xcb_poll_for_reply for it, so tell libxcb to drop it when it
      // arrives instead of holding onto it for the life of the
      // connection.
      xcb_discard_reply(im.conn, pending.fence_sequence);
      PendingDmabufImport resolved = std::move(pending);
      im.present.pending_dmabuf_imports[i] =
          std::move(im.present.pending_dmabuf_imports.back());
      im.present.pending_dmabuf_imports.pop_back();
      finish_dmabuf_import(im, std::move(resolved), /*ok=*/false);
      break;
    }

    // A failed MIT-SHM PutImage (e.g. BadValue from a client stride that
    // makes the source rectangle exceed the declared row width) is dropped by
    // the server and never followed by XCB_SHM_COMPLETION. Release exactly
    // the segment that request used and complete its frame, otherwise the
    // client's wl_surface.frame callback is stranded forever. Matched on the
    // request's sequence number so other windows' in-flight frames are left
    // alone (the server may still be reading those segments).
    if (im.shm.has_shm && err->major_code == im.shm.shm_major_opcode &&
        err->minor_code == XCB_SHM_PUT_IMAGE) {
      for (auto &[window, wshm] : im.shm.window_shm) {
        for (auto &buffer : wshm.buffers) {
          if (!buffer.in_flight ||
              (buffer.put_sequence & 0xffffu) != err->sequence)
            continue;
          WW_LOG_WARN("[x11] MIT-SHM PutImage failed (window={}, "
                      "error_code={}); completing the frame without "
                      "presenting it",
                      window, err->error_code);
          buffer.in_flight = false;
          auto presented = std::move(buffer.on_presented);
          buffer.on_presented = {};
          if (presented)
            presented();
          if (wshm.pending.present) {
            PendingShmFrame pending = std::move(wshm.pending);
            wshm.pending = PendingShmFrame{};
            if (im.shm.present_pending_shm_frame)
              im.shm.present_pending_shm_frame(window, std::move(pending));
          }
          return;
        }
      }
    }
    return;
  }

  const auto code =
      static_cast<uint8_t>(ev->response_type & ~0x80u);

  if (im.shm.has_shm &&
      code == static_cast<uint8_t>(im.shm.shm_first_event + XCB_SHM_COMPLETION)) {
    const auto *completion =
        reinterpret_cast<const xcb_shm_completion_event_t *>(ev);
    for (auto &[window, wshm] : im.shm.window_shm) {
      (void)window;
      for (auto &buffer : wshm.buffers) {
        if (buffer.seg != completion->shmseg || !buffer.in_flight)
          continue;
        buffer.in_flight = false;
        ::ww::diag::note(window, ::ww::diag::Ev::ShmDone);
        auto presented = std::move(buffer.on_presented);
        buffer.on_presented = {};
        if (presented)
          presented();
        if (wshm.pending.present) {
          // Move the pending bytes out before calling present_pixels(), which
          // may access this WindowShm entry again and clear/replace pending.
          PendingShmFrame pending = std::move(wshm.pending);
          wshm.pending = PendingShmFrame{};
          ::ww::diag::note(window, ::ww::diag::Ev::ShmPendingFlushed);
          if (im.shm.present_pending_shm_frame)
            im.shm.present_pending_shm_frame(window, std::move(pending));
        }
        return;
      }
    }
    WW_LOG_TRACE("[x11] unmatched SHM completion (segment={})",
                 completion->shmseg);
    return;
  }

  if (im.clipboard.has_xfixes &&
      code == static_cast<uint8_t>(im.clipboard.xfixes_first_event +
                                   XCB_XFIXES_SELECTION_NOTIFY)) {
    dispatch_xfixes_selection_notify(im, ev);
    return;
  }

  if (im.screen_state.has_randr &&
      code == static_cast<uint8_t>(im.screen_state.randr_first_event +
                                   XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
    dispatch_randr_screen_change(im, ev);
    return;
  }

  // XKB shares a single base event number across all of its sub-events
  // (StateNotify, MapNotify, ...) rather than one per sub-type the way
  // XFixes does above — dispatch_xkb_event does its own further
  // discrimination via the event's xkbType field.
  if (im.input.has_xkb && code == im.input.xkb_base_event) {
    dispatch_xkb_event(im, ev);
    return;
  }

  switch (code) {
  case XCB_CLIENT_MESSAGE:
    dispatch_client_message(im, ev);
    break;
  case XCB_SELECTION_REQUEST:
    dispatch_selection_request(im, ev);
    break;
  case XCB_SELECTION_CLEAR:
    dispatch_selection_clear(im, ev);
    break;
  case XCB_DESTROY_NOTIFY:
    dispatch_destroy_notify(im, ev);
    break;
  case XCB_SELECTION_NOTIFY:
    dispatch_selection_notify(im, ev);
    break;
  case XCB_CONFIGURE_NOTIFY:
    dispatch_configure_notify(im, ev);
    break;
  case XCB_ENTER_NOTIFY:
    dispatch_enter_notify(im, ev);
    break;
  case XCB_LEAVE_NOTIFY:
    dispatch_leave_notify(im, ev);
    break;
  case XCB_MOTION_NOTIFY:
    dispatch_motion_notify(im, ev);
    break;
  case XCB_BUTTON_PRESS:
    dispatch_button_press(im, ev);
    break;
  case XCB_BUTTON_RELEASE:
    dispatch_button_release(im, ev);
    break;
  case XCB_KEY_PRESS:
  case XCB_KEY_RELEASE: {
    const bool pressed = code == XCB_KEY_PRESS;
    uint8_t keycode = 0;
    uint32_t time = 0;
    if (pressed) {
      const auto *key = reinterpret_cast<xcb_key_press_event_t *>(ev);
      keycode = key->detail;
      time = key->time;
    } else {
      const auto *key = reinterpret_cast<xcb_key_release_event_t *>(ev);
      keycode = key->detail;
      time = key->time;
    }

    std::deque<PendingXkbStateNotify> matching_states;
    while (!im.input.pending_xkb_state_notifies.empty()) {
      const PendingXkbStateNotify state =
          im.input.pending_xkb_state_notifies.front();
      if (state.keycode == keycode && state.time == time &&
          state.event_type == code) {
        // Keep the matching snapshot and any later snapshots queued. XKB's
        // event stream is ordered, so the match is the next causal state
        // update after all earlier unmatched notifications have been handled.
        while (!im.input.pending_xkb_state_notifies.empty()) {
          const auto &next = im.input.pending_xkb_state_notifies.front();
          if (next.keycode != keycode || next.time != time ||
              next.event_type != code)
            break;
          matching_states.push_back(next);
          im.input.pending_xkb_state_notifies.pop_front();
        }
        break;
      }

      // The corresponding core event did not precede this event on our
      // connection (usually because a grab consumed it). Reconcile that
      // state before delivering a later, unrelated key event.
      im.input.pending_xkb_state_notifies.pop_front();
      deliver_xkb_state_notify(im, state);
    }

    if (pressed)
      dispatch_key_press(im, ev);
    else
      dispatch_key_release(im, ev);

    // Wayland requires a modifier change caused by a key event to follow that
    // key event. If XKB delivered its notification first, emit it now.
    for (const auto &state : matching_states)
      deliver_xkb_state_notify(im, state);
    break;
  }
  case XCB_FOCUS_IN:
    flush_pending_xkb_state_notifies(im);
    dispatch_focus_in(im, ev);
    break;
  case XCB_FOCUS_OUT:
    flush_pending_xkb_state_notifies(im);
    dispatch_focus_out(im, ev);
    break;
  case XCB_EXPOSE:
    dispatch_expose(im, ev);
    break;
  case XCB_PROPERTY_NOTIFY:
    dispatch_property_notify(im, ev);
    break;
  case XCB_GE_GENERIC:
    dispatch_present_generic(im, ev);
    break;
  default:
    break;
  }
}

void X11Backend::Impl::dispatch_property_notify(Impl &im,
                                                xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_property_notify_event_t *>(ev);
  dispatch_clipboard_property_notify(im, e);

  // Root-window watches registered via watch_root_property() -- checked
  // first and independently of the toplevel _NET_WM_STATE handling below,
  // since these fire on the root window itself, which is never a managed
  // toplevel (im.window.toplevels.count(e->window) below is always false for it,
  // so this can never double-fire into that path).
  if (e->window == im.screen->root) {
    const auto it = im.root_property_watches.find(e->atom);
    if (it != im.root_property_watches.end() && it->second) {
      // A delete needs no round trip -- the event itself already says the
      // value is gone. A new/changed value is read fresh rather than
      // trusting anything in the event, which carries no value of its
      // own (PropertyNotify never does).
      if (e->state == XCB_PROPERTY_DELETE)
        it->second(std::nullopt);
      else
        it->second(Impl::read_root_string_property(im, e->atom));
    }
  }

  if (!im.on_wm_state || im.window.net_wm_state_atom == XCB_ATOM_NONE ||
      e->atom != im.window.net_wm_state_atom || !im.window.toplevels.count(e->window))
    return;

  if (e->state == XCB_PROPERTY_DELETE) {
    // A window manager that withdraws the property is saying "none of
    // these apply any more".
    im.on_wm_state(e->window, {});
    return;
  }

  // Length in 32-bit units: a handful of atoms at most, but leave headroom.
  const xcb_get_property_cookie_t cookie = xcb_get_property(
      im.conn, /*_delete=*/0, e->window, im.window.net_wm_state_atom, XCB_ATOM_ATOM,
      0, 64);
  xcb_flush(im.conn);
  im.window.pending_wm_state_reads.push_back({cookie.sequence, e->window});
}

void X11Backend::Impl::poll_pending_wm_state_reads(Impl &im) {
  // In request order: replies come back in order, so stopping at the first
  // one that is not ready keeps older reads from being applied after newer.
  size_t done = 0;
  for (; done < im.window.pending_wm_state_reads.size(); ++done) {
    const auto pending = im.window.pending_wm_state_reads[done];
    void *reply_ptr = nullptr;
    xcb_generic_error_t *error_ptr = nullptr;
    if (!xcb_poll_for_reply(im.conn, pending.sequence, &reply_ptr, &error_ptr))
      break;
    ::free(error_ptr); // window vanished in between: nothing to report

    auto *reply = static_cast<xcb_get_property_reply_t *>(reply_ptr);
    if (reply && im.on_wm_state && im.window.toplevels.count(pending.window)) {
      IWindowBackend::WmWindowState state;
      if (reply->format == 32 && reply->type == XCB_ATOM_ATOM) {
        const auto *atoms = static_cast<const xcb_atom_t *>(
            xcb_get_property_value(reply));
        const int n = xcb_get_property_value_length(reply) /
                      static_cast<int>(sizeof(xcb_atom_t));
        for (int i = 0; i < n; ++i) {
          if (atoms[i] == XCB_ATOM_NONE)
            continue;
          if (atoms[i] == im.window.net_wm_state_fullscreen_atom)
            state.fullscreen = true;
          else if (atoms[i] == im.window.net_wm_state_maximized_horz_atom)
            state.maximized_horz = true;
          else if (atoms[i] == im.window.net_wm_state_maximized_vert_atom)
            state.maximized_vert = true;
        }
      }
      // A property of the wrong shape counts as "none of them set".
      im.on_wm_state(pending.window, state);
    }
    ::free(reply);
  }
  im.window.pending_wm_state_reads.erase(im.window.pending_wm_state_reads.begin(),
                                  im.window.pending_wm_state_reads.begin() +
                                      static_cast<std::ptrdiff_t>(done));
}

void X11Backend::Impl::poll_pending_dmabuf_imports(Impl &im) {
  // Iterate backwards so swap-and-pop removal (matching dispatch_one's
  // error-match removal above) doesn't skip the entry that gets moved
  // into the current slot.
  for (size_t i = im.present.pending_dmabuf_imports.size(); i-- > 0;) {
    auto &pending = im.present.pending_dmabuf_imports[i];
    void *reply_ptr = nullptr;
    xcb_generic_error_t *error_ptr = nullptr;
    if (!xcb_poll_for_reply(im.conn, pending.fence_sequence, &reply_ptr,
                            &error_ptr))
      continue; // fence hasn't come back yet — try again next pass

    // The fence itself (xcb_get_input_focus) essentially cannot fail, but
    // free whichever of reply/error came back defensively either way.
    ::free(reply_ptr);
    ::free(error_ptr);

    // Fence resolved and this entry is still here, i.e. dispatch_one
    // never matched an error to request_sequence — by X11's strict
    // per-connection request ordering, the import necessarily succeeded.
    PendingDmabufImport resolved = std::move(pending);
    im.present.pending_dmabuf_imports[i] = std::move(im.present.pending_dmabuf_imports.back());
    im.present.pending_dmabuf_imports.pop_back();
    WW_LOG_DEBUG("[x11] DRI3 import confirmed ok (pixmap={})", resolved.pixmap);
    finish_dmabuf_import(im, std::move(resolved), /*ok=*/true);
  }
}

void X11Backend::Impl::poll_pending_frame_fences(Impl &im) {
  for (std::size_t i = 0; i < im.present.pending_frame_fences.size();) {
    auto &pending = im.present.pending_frame_fences[i];
    void *reply = nullptr;
    xcb_generic_error_t *error = nullptr;
    if (!xcb_poll_for_reply(im.conn, pending.sequence, &reply, &error)) {
      ++i;
      continue;
    }
    ::free(reply);
    ::free(error);
    auto callback = std::move(pending.callback);
    im.present.pending_frame_fences[i] = std::move(im.present.pending_frame_fences.back());
    im.present.pending_frame_fences.pop_back();
    if (callback)
      callback();
  }
}

int X11Backend::Impl::drain_stranded(Impl &im) {
  // xcb calls that read the socket (xcb_poll_for_reply and the blocking
  // *_reply helpers) can move events into xcb's internal queue without the
  // fd becoming readable again, so poll() would sleep with work pending.
  int stranded = 0;
  while (xcb_generic_event_t *ev = xcb_poll_for_queued_event(im.conn)) {
    ++stranded;
    dispatch_one(im, ev);
    std::free(ev);
  }
  if (stranded) {
    ::ww::diag::note(0, ::ww::diag::Ev::Stranded,
                     static_cast<uint32_t>(stranded));
    WW_LOG_WARN("[diag] {} X event(s) were stranded in xcb's queue while the "
                "loop was idle (poll() would not have woken for them)",
                stranded);
    process_events(im);
  }
  return stranded;
}

void X11Backend::Impl::process_events(Impl &im) {
  while (xcb_generic_event_t *ev = xcb_poll_for_event(im.conn)) {
    dispatch_one(im, ev);
    std::free(ev);
  }
  // A passive grab can consume the core key event associated with an XKB
  // StateNotify. Flush those snapshots even when no matching KeyPress /
  // KeyRelease reached WWindows, so modifier state cannot stay stale until
  // the user's next key.
  flush_pending_xkb_state_notifies(im);
  // Non-blocking check for a GetProperty fired earlier by
  // dispatch_selection_notify — see PendingPropertyRead's comment.
  poll_pending_property_read(im);
  // Non-blocking atom-name replies for a native owner's TARGETS list.
  poll_pending_clipboard_targets(im);
  // Non-blocking property reads for ICCCM MULTIPLE requests.
  poll_pending_clipboard_multiple_reads(im);
  // Non-blocking check for any DRI3 imports fired earlier by
  // import_dmabuf() — see PendingDmabufImport's comment.
  poll_pending_dmabuf_imports(im);
  // Non-blocking check for any _NET_WM_STATE reads fired from a
  // PropertyNotify above.
  poll_pending_wm_state_reads(im);
  // Complete Render-based frames after the X server has processed the queued
  // composite request (without blocking inside event dispatch).
  poll_pending_frame_fences(im);
  // Notify caller that we've processed pending X events so they can
  // perform follow-up actions (e.g. flush Wayland clients).
  if (im.events_processed_cb)
    im.events_processed_cb();
}

} // namespace ww
