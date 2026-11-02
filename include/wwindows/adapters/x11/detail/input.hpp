#pragma once
// XKB extension and input-callback state owned by X11Backend::Impl.
//
// Driven by X11Backend_events.cpp (dispatch_xkb_event and the key/button/
// motion dispatchers) and X11Backend_lifecycle.cpp (extension setup).
//
// Pure data: no methods.

#include "wwindows/ports/window_backend.hpp"

#include <cstdint>
#include <deque>

namespace ww {

// XKB StateNotify can accompany a core key event. When it is observed before
// that core event on our X connection, delay delivering the state snapshot
// until after the matching press/release so the Wayland side can emit
// wl_keyboard.key before the resulting wl_keyboard.modifiers. If a key event
// is grabbed and never reaches this connection, process_events() flushes the
// snapshot after draining the batch.
struct PendingXkbStateNotify {
  uint32_t time{0};
  uint8_t keycode{0};
  uint8_t event_type{0};
  uint32_t depressed_mods{0};
  uint32_t latched_mods{0};
  uint32_t locked_mods{0};
  uint32_t effective_group{0};
};

struct InputBackendState {
  IWindowBackend::InputCallbacks input_cbs;
  std::deque<PendingXkbStateNotify> pending_xkb_state_notifies;

  // XKB extension: real keymap + live modifier/group state from the
  // server, rather than a fabricated default. has_xkb false means either
  // the extension setup failed (xkb_x11_setup_xkb_extension) or the core
  // keyboard device couldn't be identified -- both treated the same way,
  // as "fall back to a default keymap", not a hard failure. See
  // keyboard_keymap_string's doc comment (window_backend.hpp) for why
  // this matters.
  bool has_xkb{false};
  uint8_t xkb_base_event{0};
  int32_t xkb_device_id{-1};
};

} // namespace ww
