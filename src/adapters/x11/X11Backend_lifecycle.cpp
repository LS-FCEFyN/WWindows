#include "wwindows/core/event_loop.hpp"
#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include <wwindows/support/frame_diag.hpp>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <wwindows/support/log.hpp>

#include <sys/shm.h>
#include <unistd.h>
#include <xcb/render.h>
#include <xcb/shape.h>
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
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

namespace ww {

void X11Backend::Impl::arm_diag_tick(EventLoop &loop, Impl *im) {
  im->diag_timer = loop.schedule_at(
      IResizeTimer::Clock::now() + std::chrono::milliseconds{500},
      [&loop, im] {
        im->diag_timer = 0;
        drain_stranded(*im);
        expire_stale_incr_transfers(*im);
        ::ww::diag::tick();
        arm_diag_tick(loop, im);
      });
}

X11Backend::X11Backend() : impl_{std::make_unique<Impl>()} {
  impl_->shm.present_pending_shm_frame =
      [this](uint32_t window, PendingShmFrame frame) {
        present_pixels(window, frame.width, frame.height, frame.stride,
                       frame.pixels.data(), frame.src,
                       std::move(frame.on_presented));
      };
}

X11Backend::~X11Backend() {
  if (impl_->loop && impl_->diag_timer)
    impl_->loop->cancel(impl_->diag_timer);
  if (impl_->conn) {
    // Unwind any in-flight clipboard transfer before the connection goes
    // away — the pipe fds are ours to close and the EventLoop needs to
    // stop watching them.
    for (auto &[fd, pending] : impl_->clipboard.pending_sends) {
      if (impl_->loop)
        impl_->loop->remove(fd);
      ::close(fd);
      (void)pending;
    }
    impl_->clipboard.pending_sends.clear();
    impl_->clipboard.receive_queue.clear();

    if (impl_->clipboard.selection_window)
      xcb_destroy_window(impl_->conn, impl_->clipboard.selection_window);

    if (impl_->window.colormap32)
      xcb_free_colormap(impl_->conn, impl_->window.colormap32);

    // Free any cached GCs
    for (auto &[win, gc] : impl_->window.gcs)
      xcb_free_gc(impl_->conn, gc);

    // Free any cached Render Pictures (window destinations + BGR-order
    // dmabuf sources) — normally already gone via destroy_window()/
    // free_pixmap(), this only catches whatever outlived those.
    for (auto &[win, pic] : impl_->present.window_pictures)
      xcb_render_free_picture(impl_->conn, pic);
    for (auto &[pixmap, src] : impl_->present.render_source_pictures)
      xcb_render_free_picture(impl_->conn, src.picture);

    // Free any per-window SHM segments that outlived destroy_window()/
    // forget_window() — normally none do, this only catches whatever a
    // window still open at shutdown left behind.
    for (auto &[win, wshm] : impl_->shm.window_shm) {
      free_shm_state(impl_->conn, wshm.buffers[0]);
      free_shm_state(impl_->conn, wshm.buffers[1]);
    }

    if (impl_->present.drm_fd >= 0) {
      ::close(impl_->present.drm_fd);
      impl_->present.drm_fd = -1;
    }

    xcb_disconnect(impl_->conn);
    WW_LOG_INFO("[x11] disconnected");
  }
}

// Interns WM_PROTOCOLS / WM_DELETE_WINDOW so ClientMessage close requests
// from the window manager can be recognised later.
void X11Backend::Impl::intern_wm_atoms(Impl &im) {
  // Fire all three requests before waiting on any reply — avoids three
  // serial round-trips to the X server. Safe to block here: this runs
  // during X11Backend::create(), before the event loop (and therefore
  // before any window a window manager could be reacting to) exists.
  xcb_intern_atom_cookie_t c1 = xcb_intern_atom(im.conn, 0, 12, "WM_PROTOCOLS");
  xcb_intern_atom_cookie_t c2 = xcb_intern_atom(im.conn, 0, 16, "WM_DELETE_WINDOW");
  xcb_intern_atom_cookie_t c3 = xcb_intern_atom(im.conn, 0, 12, "_NET_WM_NAME");
  xcb_intern_atom_cookie_t c4 =
      xcb_intern_atom(im.conn, 0, 13, "_NET_WM_STATE");
  xcb_intern_atom_cookie_t c5 =
      xcb_intern_atom(im.conn, 0, 24, "_NET_WM_STATE_FULLSCREEN");
  xcb_intern_atom_cookie_t c6 =
      xcb_intern_atom(im.conn, 0, 28, "_NET_WM_STATE_MAXIMIZED_VERT");
  xcb_intern_atom_cookie_t c7 =
      xcb_intern_atom(im.conn, 0, 28, "_NET_WM_STATE_MAXIMIZED_HORZ");
  xcb_intern_atom_cookie_t c8 = xcb_intern_atom(im.conn, 0, 11, "_NET_WM_PID");
  xcb_intern_atom_cookie_t c9 =
      xcb_intern_atom(im.conn, 0, 18, "_NET_WM_MOVERESIZE");
  xcb_intern_atom_cookie_t c10 =
      xcb_intern_atom(im.conn, 0, 15, "WM_CHANGE_STATE");
  xcb_flush(im.conn);

  xcb_intern_atom_reply_t *r1 = xcb_intern_atom_reply(im.conn, c1, nullptr);
  im.window.wm_protocols_atom = r1 ? r1->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  ::free(r1);

  xcb_intern_atom_reply_t *r2 = xcb_intern_atom_reply(im.conn, c2, nullptr);
  im.window.wm_delete_atom = r2 ? r2->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  ::free(r2);

  xcb_intern_atom_reply_t *r3 = xcb_intern_atom_reply(im.conn, c3, nullptr);
  im.window.net_wm_name_atom = r3 ? r3->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  ::free(r3);

  const auto take = [&](xcb_intern_atom_cookie_t c) -> xcb_atom_t {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(im.conn, c, nullptr);
    const xcb_atom_t a = r ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    ::free(r);
    return a;
  };
  im.window.net_wm_state_atom = take(c4);
  im.window.net_wm_state_fullscreen_atom = take(c5);
  im.window.net_wm_state_maximized_vert_atom = take(c6);
  im.window.net_wm_state_maximized_horz_atom = take(c7);
  im.window.net_wm_pid_atom = take(c8);
  im.window.net_wm_moveresize_atom = take(c9);
  im.window.wm_change_state_atom = take(c10);

  // WM_CLIENT_MACHINE is this machine's name for every window we create --
  // a Wayland client is always local -- so read it once here rather than
  // per window.
  char host[256] = {};
  if (::gethostname(host, sizeof host - 1) == 0)
    im.window.client_machine = host;
}

// Finds a TrueColor 32-bit visual (if any) and allocates a colormap for it,
// enabling ARGB windows for correct alpha compositing.
void X11Backend::Impl::find_argb_visual(Impl &im) {
  xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(im.screen);
  for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
    if (depth_iter.data->depth == 32) {
      xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
        if (visual_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
          im.window.visual32 = visual_iter.data->visual_id;
          break;
        }
      }
    }
  }
  if (im.window.visual32) {
    im.window.colormap32 = xcb_generate_id(im.conn);
    xcb_create_colormap(im.conn, XCB_COLORMAP_ALLOC_NONE, im.window.colormap32, im.screen->root, im.window.visual32);
    WW_LOG_INFO("[x11] found 32-bit ARGB visual (id={}), colormap={}",
                im.window.visual32, im.window.colormap32);
  } else {
    WW_LOG_WARN("[x11] no 32-bit TrueColor visual available — windows will "
                "use the default depth, no per-pixel alpha");
  }
}

Result<std::unique_ptr<X11Backend>> X11Backend::create(EventLoop &loop) {
  auto backend = std::unique_ptr<X11Backend>(new X11Backend());
  auto &im = *backend->impl_;
  im.loop = &loop;

  int screen_num = 0;
  im.conn = xcb_connect(nullptr, &screen_num);
  if (xcb_connection_has_error(im.conn)) {
    WW_LOG_ERROR("[x11] xcb_connect failed (error={})",
                 xcb_connection_has_error(im.conn));
    return std::unexpected(
        Error::x11_connect("Failed to connect to X11 server"));
  }

  const xcb_setup_t *setup = xcb_get_setup(im.conn);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screen_num; ++i)
    xcb_screen_next(&iter);
  im.screen = iter.data;

  // So root-window property changes -- watch_root_property()'s whole
  // reason to exist -- actually reach this connection at all.
  // PropertyNotify is a broadcast-style event type (unlike e.g.
  // SUBSTRUCTURE_REDIRECT), so selecting for it here doesn't contend with
  // whatever the real window manager has already selected on the same
  // window for its own purposes.
  {
    const uint32_t mask = XCB_CW_EVENT_MASK;
    const uint32_t value = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(im.conn, im.screen->root, mask, &value);
  }

  Impl::intern_wm_atoms(im);
  Impl::find_argb_visual(im);

  // wl_region → X11 Shape extension (XShapeCombineRectangles on the input
  // shape). Query availability once; set_input_shape() no-ops if absent.
  {
    const xcb_query_extension_reply_t *shape_ext =
        xcb_get_extension_data(im.conn, &xcb_shape_id);
    im.has_shape = shape_ext && shape_ext->present;
    if (!im.has_shape)
      WW_LOG_WARN("[x11] Shape extension unavailable — wl_region input "
                  "regions will be ignored (surfaces stay click-through "
                  "over their whole rectangle)");
  }

  // Force MIT-SHM's extension data to be resolved now, during setup,
  // rather than lazily on first use. Every xcb_shm_* wrapper function
  // (xcb_shm_attach, xcb_shm_put_image, ...) calls xcb_get_extension_data()
  // internally to look up MIT-SHM's opcode; libxcb caches that result for
  // the life of the connection, but only once *something* has asked. If
  // nothing asks until the first real wl_shm buffer commit, that commit —
  // which happens from inside live Wayland-request dispatch, i.e.
  // possibly while a window manager is mid-reparent and holding the
  // server — pays for the (otherwise invisible) blocking round-trip at
  // exactly the wrong moment, same class of hang as set_window_title's
  // atom lookups used to be. Warming the cache here (before the event
  // loop, and therefore before any window a WM could be reacting to,
  // exists) is free insurance: wl_shm is unconditionally advertised
  // regardless of what this reports, so there is no fallback path to wire
  // up either way — this call exists purely to force the resolution.
  {
    const xcb_query_extension_reply_t *shm_ext =
        xcb_get_extension_data(im.conn, &xcb_shm_id);
    im.shm.has_shm = shm_ext && shm_ext->present;
    if (im.shm.has_shm) {
      im.shm.shm_first_event = shm_ext->first_event;
      im.shm.shm_major_opcode = shm_ext->major_opcode;
    } else {
      WW_LOG_WARN("[x11] MIT-SHM extension not reported by the X server — "
                  "wl_shm buffer presentation will fail at runtime");
    }
  }

  // --- Clipboard setup ------------------------------------------------
  // A hidden 1x1 window used both as our CLIPBOARD owner window (when a
  // Wayland client holds the selection) and as the requestor window for
  // ConvertSelection (when reading a native X11 app's clipboard). It is
  // never mapped — it exists purely as an addressable window id for the
  // selection protocol, same idea as the override-redirect cursor window.
  im.clipboard.selection_window = xcb_generate_id(im.conn);
  {
    const uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    const uint32_t values[] = {1, XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(im.conn, XCB_COPY_FROM_PARENT, im.clipboard.selection_window,
                      im.screen->root, -1, -1, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, im.screen->root_visual,
                      mask, values);
  }
  Impl::intern_clipboard_atoms(im);

  {
    const xcb_query_extension_reply_t *xfixes_ext =
        xcb_get_extension_data(im.conn, &xcb_xfixes_id);
    im.clipboard.has_xfixes = xfixes_ext && xfixes_ext->present;
    if (im.clipboard.has_xfixes) {
      im.clipboard.xfixes_first_event = xfixes_ext->first_event;
      xcb_xfixes_query_version(im.conn, XCB_XFIXES_MAJOR_VERSION,
                               XCB_XFIXES_MINOR_VERSION);
      xcb_xfixes_select_selection_input(
          im.conn, im.clipboard.selection_window, im.clipboard.atom_clipboard,
          XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
              XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
              XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
      WW_LOG_DEBUG("[x11] XFixes selection tracking enabled for CLIPBOARD");
    } else {
      WW_LOG_WARN("[x11] XFixes unavailable — WWindows will only notice "
                  "CLIPBOARD ownership changes it initiates itself, not "
                  "ones between two other X11 applications");
    }
  }

  // The screen. The connection's setup data is current at connect time (and
  // is all there is without RandR); after that RandR is what says it changed.
  im.screen_state.geometry = X11Backend::ScreenGeometry{
      im.screen->width_in_pixels, im.screen->height_in_pixels,
      im.screen->width_in_millimeters, im.screen->height_in_millimeters, 0};
  {
    const xcb_query_extension_reply_t *randr_ext =
        xcb_get_extension_data(im.conn, &xcb_randr_id);
    im.screen_state.has_randr = randr_ext && randr_ext->present;
    if (im.screen_state.has_randr) {
      im.screen_state.randr_first_event = randr_ext->first_event;
      // A client has to negotiate a version before using the extension.
      free(xcb_randr_query_version_reply(
          im.conn, xcb_randr_query_version(im.conn, 1, 2), nullptr));
      xcb_randr_select_input(im.conn, im.screen->root,
                             XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
      im.screen_state.geometry.refresh_mhz = Impl::query_refresh_mhz(im);
      WW_LOG_DEBUG("[x11] RandR screen tracking enabled ({}x{}px, {} mHz)",
                   im.screen_state.geometry.width_px,
                   im.screen_state.geometry.height_px,
                   im.screen_state.geometry.refresh_mhz);
    } else {
      WW_LOG_WARN("[x11] RandR unavailable — the screen's size and refresh "
                  "rate are what they were at startup, and are never updated");
    }
  }

  // --- XKB: the server's real keymap and live modifier/group state ----
  // See keyboard_keymap_string's doc comment (window_backend.hpp) for why
  // this matters: a compositor that fabricates its own default keymap
  // instead disagrees with the person's actual session configuration —
  // layout, and any personal setxkbmap customization — for every key.
  {
    uint8_t base_event = 0;
    const int ok = xkb_x11_setup_xkb_extension(
        im.conn, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
        XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, &base_event,
        nullptr);
    const int32_t device_id =
        ok ? xkb_x11_get_core_keyboard_device_id(im.conn) : -1;
    im.input.has_xkb = ok && device_id != -1;
    if (im.input.has_xkb) {
      im.input.xkb_base_event = base_event;
      im.input.xkb_device_id = device_id;
      // StateNotify (modifier/group churn) *and* MapNotify/
      // NewKeyboardNotify (the keymap itself changed — a live
      // setxkbmap, or, far more often in practice, this process simply
      // starting before whatever sets the session's real layout has run
      // yet). Both matter: see keyboard_keymap_string's doc comment for
      // why fabricating a default keymap is never an acceptable
      // fallback, and on_xkb_keymap_changed's doc comment for why a
      // one-shot fetch at startup isn't enough on its own — it can
      // simply lose the race.
      xcb_xkb_select_events(
          im.conn, static_cast<xcb_xkb_device_spec_t>(device_id),
          XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
              XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
              XCB_XKB_EVENT_TYPE_CONTROLS_NOTIFY,
          0,
          XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
              XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
              XCB_XKB_EVENT_TYPE_CONTROLS_NOTIFY,
          0, 0, nullptr);

      // Ask this X11 connection for detectable auto-repeat. Without it, the
      // server represents a held/repeating key as synthetic KeyRelease /
      // KeyPress pairs, which this Wayland bridge would otherwise expose as
      // real transitions in addition to wl_keyboard.repeat_info. The setting
      // is per-client and does not change repeat behavior for other X clients.
      constexpr uint32_t kDetectableRepeat =
          XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT;
      const auto repeat_cookie = xcb_xkb_per_client_flags(
          im.conn, XCB_XKB_ID_USE_CORE_KBD, kDetectableRepeat,
          kDetectableRepeat, 0, 0, 0);
      xcb_generic_error_t *repeat_error = nullptr;
      auto *repeat_reply = xcb_xkb_per_client_flags_reply(
          im.conn, repeat_cookie, &repeat_error);
      if (repeat_reply && (repeat_reply->supported & kDetectableRepeat) &&
          (repeat_reply->value & kDetectableRepeat)) {
        WW_LOG_INFO("[x11] detectable auto-repeat enabled for WWindows");
      } else {
        WW_LOG_WARN("[x11] detectable auto-repeat unavailable; repeated-key "
                    "release/press pairs may be indistinguishable from real "
                    "transitions on this X server");
      }
      std::free(repeat_reply);
      std::free(repeat_error);

      WW_LOG_INFO("[x11] XKB extension ready (device={}) — using the "
                  "session's real keymap and live modifier state",
                  device_id);
    } else {
      WW_LOG_WARN("[x11] XKB extension unavailable — falling back to a "
                  "default keymap that may not match this session's "
                  "actual layout or keyboard options");
    }
  }

  int fd = xcb_get_file_descriptor(im.conn);

  auto result =
      loop.add(fd, IoIn | IoErr | IoHup,
               [backend = backend.get(), &loop](uint32_t events) {
                 if (events & (IoErr | IoHup)) {
                   WW_LOG_INFO("[x11] X server connection closed, shutting down");
                   loop.stop(0);
                   return;
                 }
                 if (events & IoIn) {
                   Impl::process_events(*backend->impl_);
                 }
               });

  if (!result)
    return std::unexpected(result.error());

  // Diagnostics: twice a second look for stalled frame stages and stranded
  // xcb events, and honour SIGUSR1 state-dump requests.
  ::ww::diag::install_signal_handler();
  Impl::arm_diag_tick(loop, backend->impl_.get());

  // DRI3 + Present are optional; failure just means dmabuf is unavailable.
  if (!backend->init_dri3_present()) {
    WW_LOG_WARN("[x11] DRI3/Present unavailable — dmabuf disabled, falling "
                "back to XShm only");
  }

  // Render is queried whether or not DRI3 came up: besides correcting
  // swapped-channel dmabufs it is what builds ARGB pointer cursors for
  // wl_pointer.set_cursor (X11Backend_cursor.cpp), and a server without DRI3
  // (Xvfb, a remote X session) still needs those.
  if (!backend->init_render()) {
    WW_LOG_WARN("[x11] Render extension unavailable or missing a usable "
                "PictFormat — client pointer cursors cannot be shown (the "
                "default arrow stays), and dmabuf buffers with a swapped "
                "channel order (e.g. ABGR8888, as used by Android/Waydroid "
                "surfaces) will be rejected rather than mis-rendered; "
                "native ARGB8888/XRGB8888 dmabuf import is unaffected");
  }

  WW_LOG_INFO("[x11] connected to X server, screen {}x{} ({}x{} mm)",
              im.screen->width_in_pixels, im.screen->height_in_pixels,
              im.screen->width_in_millimeters,
              im.screen->height_in_millimeters);

  // Pick up whatever already owns CLIPBOARD (if anything) so a Wayland
  // client that binds wl_data_device before any X11-side change happens
  // still sees the current clipboard contents.
  Impl::refresh_incoming_clipboard_targets(im);

  return backend;
}

xcb_connection_t *X11Backend::connection() const { return impl_->conn; }
xcb_screen_t *X11Backend::screen() const { return impl_->screen; }

X11Backend::ScreenGeometry X11Backend::screen_geometry() const {
  return impl_->screen_state.geometry;
}

void X11Backend::set_screen_changed_callback(
    std::function<void(const ScreenGeometry &)> cb) {
  impl_->screen_state.on_changed = std::move(cb);
}

uint32_t X11Backend::Impl::query_refresh_mhz(Impl &im) {
  const auto cookie = xcb_randr_get_screen_info(im.conn, im.screen->root);
  auto *reply = xcb_randr_get_screen_info_reply(im.conn, cookie, nullptr);
  if (!reply)
    return 0;
  const uint32_t hz = reply->rate; // 0 on a virtual screen with no real mode
  free(reply);
  return hz * 1000u;
}

std::optional<IWindowBackend::KeyRepeat> X11Backend::keyboard_repeat() {
  auto &im = *impl_;
  if (!im.input.has_xkb)
    return std::nullopt;

  // The server owns this setting (XKB's RepeatKeys control). Read at startup
  // and after each ControlsNotify -- human-timescale events, so one
  // synchronous reply is fine; nothing on a hot path calls this.
  const auto cookie = xcb_xkb_get_controls(
      im.conn, static_cast<xcb_xkb_device_spec_t>(im.input.xkb_device_id));
  xcb_generic_error_t *error = nullptr;
  auto *reply = xcb_xkb_get_controls_reply(im.conn, cookie, &error);
  if (!reply) {
    free(error);
    return std::nullopt;
  }
  KeyRepeat out;
  const bool enabled = (reply->enabledControls & XCB_XKB_BOOL_CTRL_REPEAT_KEYS) != 0;
  if (enabled && reply->repeatInterval > 0)
    out.rate_hz = (1000u + reply->repeatInterval / 2u) / reply->repeatInterval;
  out.delay_ms = reply->repeatDelay;
  free(reply);
  return out;
}

std::optional<std::string> X11Backend::keyboard_keymap_string() {
  auto &im = *impl_;
  if (!im.input.has_xkb)
    return std::nullopt;

  // A throwaway xkb_context/xkb_keymap that exist purely to ask the
  // server "what's your real keymap" and hand the answer back as a
  // string — InputTranslator compiles its own independent xkb_keymap/
  // xkb_state from that string (the same way it already hands a keymap
  // string to every Wayland client), rather than this port leaking an
  // xkb_context or xcb_connection_t of its own across the port boundary.
  xkb_context *tmp_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!tmp_ctx) {
    WW_LOG_WARN("[x11] keyboard_keymap_string: xkb_context_new failed");
    return std::nullopt;
  }

  xkb_keymap *km = xkb_x11_keymap_new_from_device(
      tmp_ctx, im.conn, im.input.xkb_device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!km) {
    WW_LOG_WARN(
        "[x11] keyboard_keymap_string: xkb_x11_keymap_new_from_device "
        "failed (device={})",
        im.input.xkb_device_id);
    xkb_context_unref(tmp_ctx);
    return std::nullopt;
  }

  char *raw = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
  std::optional<std::string> result;
  if (raw) {
    result = std::string(raw);
    ::free(raw);
  } else {
    WW_LOG_WARN("[x11] keyboard_keymap_string: xkb_keymap_get_as_string "
                "failed");
  }

  xkb_keymap_unref(km);
  xkb_context_unref(tmp_ctx);
  return result;
}

std::optional<std::string>
X11Backend::Impl::read_root_string_property(Impl &im, xcb_atom_t atom) {
  if (atom == XCB_ATOM_NONE)
    return std::nullopt;

  // 4096 32-bit units is far more than any socket path or similar string.
  xcb_get_property_reply_t *reply = xcb_get_property_reply(
      im.conn,
      xcb_get_property(im.conn, /*_delete=*/0, im.screen->root, atom,
                       XCB_GET_PROPERTY_TYPE_ANY, 0, 4096),
      nullptr);
  if (!reply)
    return std::nullopt;

  std::optional<std::string> value;
  if (reply->format == 8 && reply->type != XCB_ATOM_NONE) {
    const int len = xcb_get_property_value_length(reply);
    const char *data = static_cast<const char *>(xcb_get_property_value(reply));
    std::string text(data, static_cast<std::size_t>(len));
    while (!text.empty() && text.back() == '\0')
      text.pop_back(); // some writers include the terminator
    if (!text.empty())
      value = std::move(text);
  }
  ::free(reply);
  return value;
}

std::optional<std::string>
X11Backend::root_string_property(const char *name) const {
  auto &im = *impl_;

  xcb_intern_atom_reply_t *atom_reply = xcb_intern_atom_reply(
      im.conn,
      xcb_intern_atom(im.conn, /*only_if_exists=*/1,
                      static_cast<uint16_t>(std::strlen(name)), name),
      nullptr);
  const xcb_atom_t atom =
      atom_reply ? atom_reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  ::free(atom_reply);
  if (atom == XCB_ATOM_NONE)
    return std::nullopt; // nobody ever set it, so it was never interned

  return Impl::read_root_string_property(im, atom);
}

void X11Backend::watch_root_property(
    const char *name, std::function<void(std::optional<std::string>)> cb) {
  auto &im = *impl_;

  // only_if_exists=0, deliberately unlike root_string_property() above:
  // the whole point here is to watch for a property whose atom may not
  // exist yet because nobody has set it yet. Interning an atom is purely
  // a name→id mapping in the server's atom table, entirely independent
  // of whether any window currently has a property using it, so this
  // works fine even if I3_SOCKET_PATH (or whatever) has never been set
  // by anyone.
  xcb_intern_atom_reply_t *atom_reply = xcb_intern_atom_reply(
      im.conn,
      xcb_intern_atom(im.conn, /*only_if_exists=*/0,
                      static_cast<uint16_t>(std::strlen(name)), name),
      nullptr);
  const xcb_atom_t atom =
      atom_reply ? atom_reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  ::free(atom_reply);
  if (atom == XCB_ATOM_NONE) {
    WW_LOG_WARN("[x11] watch_root_property({}): xcb_intern_atom failed",
                name);
    return;
  }

  im.root_property_watches[atom] = std::move(cb);
}

} // namespace ww
