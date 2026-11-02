#include "wwindows/input/detail/input_translator_impl.hpp"
#include <wwindows/support/log.hpp>

#include <wayland-server-protocol.h>

#include <bitset>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h> // mmap, munmap, shm_open
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>

namespace ww {

namespace {

// Create an anonymous sealed shared-memory file containing `data`.
// The file is unlinked immediately so it disappears when the fd is closed.
// Returns an open fd the caller must ::close(), or -1 on error.
int make_shm_fd(const char *data, size_t size) {
  static std::atomic<int> seq{0};
  const std::string name = "/ww-keymap-" + std::to_string(::getpid()) + "-" +
                           std::to_string(seq.fetch_add(1));

  int fd =
      ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    WW_LOG_WARN("[input] shm_open({}) failed: {}", name, ::strerror(errno));
    return -1;
  }
  ::shm_unlink(name.c_str()); // name gone; data survives until fd is closed

  if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
    WW_LOG_WARN("[input] ftruncate(size={}) failed: {}", size,
                ::strerror(errno));
    ::close(fd);
    return -1;
  }

  void *ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    WW_LOG_WARN("[input] mmap(size={}) failed: {}", size, ::strerror(errno));
    ::close(fd);
    return -1;
  }
  std::memcpy(ptr, data, size);
  ::munmap(ptr, size);
  return fd;
}

} // namespace

void InputTranslator::Impl::send_modifiers(wl_resource *kbd, uint32_t serial) {
  const uint32_t dep =
      xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_DEPRESSED);
  const uint32_t lat =
      xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LATCHED);
  const uint32_t lock =
      xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LOCKED);
  const uint32_t grp =
      xkb_state_serialize_layout(xkb_st, XKB_STATE_LAYOUT_EFFECTIVE);
  wl_keyboard_send_modifiers(kbd, serial, dep, lat, lock, grp);
}

void InputTranslator::Impl::send_keymap_to(wl_resource *kbd) {
  const size_t sz = keymap_str.size() + 1; // include null terminator
  int fd = make_shm_fd(keymap_str.c_str(), sz);
  if (fd < 0) {
    WW_LOG_ERROR("[input] failed to create keymap shm fd ({} bytes) — "
                 "client will have no keymap",
                 sz);
    return;
  }
  wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                          static_cast<uint32_t>(sz));
  ::close(fd); // client received its own dup via SCM_RIGHTS

  send_repeat_info_to(kbd);

  WW_LOG_DEBUG("[input] keymap sent ({} bytes)", sz);
}

void InputTranslator::Impl::send_repeat_info_to(wl_resource *kbd) {
  if (wl_resource_get_version(kbd) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
    wl_keyboard_send_repeat_info(kbd, static_cast<int32_t>(key_repeat.rate_hz),
                                 static_cast<int32_t>(key_repeat.delay_ms));
  WW_LOG_DEBUG("[input] repeat_info sent: {} Hz after {} ms", key_repeat.rate_hz,
               key_repeat.delay_ms);
}

bool InputTranslator::Impl::refresh_key_repeat() {
  const auto reported = backend->keyboard_repeat();
  if (!reported || *reported == key_repeat)
    return false;
  key_repeat = *reported;
  return true;
}

void InputTranslator::Impl::on_xkb_repeat_changed() {
  if (!display || !refresh_key_repeat())
    return; // other XKB controls changed, or nothing we announce did

  WW_LOG_INFO("[input] key repeat is now {} Hz after {} ms — telling {} "
              "keyboard(s)", key_repeat.rate_hz, key_repeat.delay_ms,
              keyboards.size());
  for (auto *k : keyboards)
    send_repeat_info_to(k);
  wl_display_flush_clients(display);
}

void InputTranslator::Impl::on_key(uint32_t win, uint8_t x11_keycode,
                                   bool pressed, uint32_t time) {
  if (!display)
    return;

  // X11 keycode and XKB keycode are the same numbering.
  // Linux evdev keycode = XKB keycode - 8. Codes below 8 are reserved and
  // cannot be represented by the evdev numbering wl_keyboard clients expect.
  if (x11_keycode < 8)
    return;
  const uint32_t evdev = static_cast<uint32_t>(x11_keycode) - 8u;

  const std::size_t key_index = static_cast<std::size_t>(x11_keycode);
  const bool was_physically_down = physical_keys_down.test(key_index);
  const bool physical_transition = pressed != was_physically_down;
  if (physical_transition) {
    physical_keys_down.set(key_index, pressed);

    // Before the first server StateNotify, retain the key-event fallback for
    // backends without XKB state notifications. After that, the mask stream
    // is authoritative; applying key events to the same xkb_state would mix
    // two mutually inconsistent sources and can resurrect a released Shift.
    if (xkb_st && !xkb_state_from_server)
      xkb_state_update_key(xkb_st, static_cast<xkb_keycode_t>(x11_keycode),
                           pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
  }

  WW_LOG_TRACE("[input] key {} (x11_keycode={}, evdev={})",
               pressed ? "down" : "up", x11_keycode, evdev);

  // Dispatched to whichever surface `win` — the X11 window this specific
  // event actually arrived on — maps to. X only ever delivers a key event
  // to whatever currently holds real input focus, so if that disagrees
  // with kbd_focus, `win` is right and kbd_focus is stale — correct it
  // right here rather than just noting the mismatch, via the same path
  // on_focus_in uses (send the client a proper enter + fresh modifiers
  // first). Sending .key to a client that never received a matching
  // .enter for this surface is a protocol violation real clients don't
  // tolerate gracefully; this is what actually closes that gap, and it
  // closes it unconditionally, with no dependency on the window manager
  // ever sending a FocusIn to prompt it.
  wl_resource *target = surfaces->surface_for_window(win);
  if (!target)
    return;
  if (target != kbd_focus) {
    // The physical bitmap already includes this press. Keep it out of the
    // enter event's keys array so the following wl_keyboard.key is the one
    // transition that introduces it to the newly focused client.
    const std::optional<uint8_t> pending_press =
        pressed && physical_transition
            ? std::optional<uint8_t>{x11_keycode}
            : std::nullopt;
    assign_keyboard_focus(target, win, pending_press);
  }

  // Wayland's logical key state is a set: sending a duplicate press or an
  // unmatched release violates wl_keyboard.key's protocol contract. In
  // particular, X11 detectable autorepeat is a stream of repeated KeyPress
  // events; the client repeats using wl_keyboard.repeat_info, so those
  // duplicate presses must not be forwarded as new transitions.
  const bool logically_down = focus_keys_down.test(key_index);
  if (pressed == logically_down)
    return;
  focus_keys_down.set(key_index, pressed);

  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() {
      if (display)
        wl_display_flush_clients(display);
    }
  } guard{display};

  const uint32_t ser = wl_display_next_serial(display);
  const uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                 : WL_KEYBOARD_KEY_STATE_RELEASED;
  wl_client *focus_client = wl_resource_get_client(target);
  for (auto *k : keyboards) {
    if (wl_resource_get_client(k) != focus_client)
      continue;
    wl_keyboard_send_key(k, ser, time, evdev, state);
    // With XKB StateNotify available, the server's authoritative mask update
    // is sent by on_xkb_state_changed() after the causative key event. Sending
    // a snapshot here could briefly publish the *old* modifier state when the
    // XKB notification follows this core event. The fallback path has no such
    // notifications, so it must update modifiers from this key transition.
    if (!xkb_state_from_server)
      send_modifiers(k, ser);
  }
}

void InputTranslator::Impl::on_xkb_key_transition(uint8_t x11_keycode,
                                                  bool pressed,
                                                  uint32_t time) {
  if (x11_keycode < 8)
    return;

  const std::size_t key_index = static_cast<std::size_t>(x11_keycode);
  physical_keys_down.set(key_index, pressed);
  if (pressed || !display || !kbd_focus ||
      !focus_keys_down.test(key_index))
    return;

  // A modifier release can be consumed by a window-manager passive grab, so
  // WWindows never receives its core KeyRelease even though the matching
  // press was already sent to a Wayland client. XKB StateNotify still names
  // that release. Clear the client's logical key state here, before the
  // authoritative wl_keyboard.modifiers event is sent for the same transition.
  focus_keys_down.reset(key_index);
  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() { wl_display_flush_clients(display); }
  } guard{display};

  const uint32_t serial = wl_display_next_serial(display);
  const uint32_t evdev = static_cast<uint32_t>(x11_keycode) - 8u;
  wl_client *focus_client = wl_resource_get_client(kbd_focus);
  for (auto *kbd : keyboards) {
    if (wl_resource_get_client(kbd) == focus_client)
      wl_keyboard_send_key(kbd, serial, time, evdev,
                           WL_KEYBOARD_KEY_STATE_RELEASED);
  }
}

void InputTranslator::Impl::resync_modifiers() {
  if (!backend)
    return;

  physical_keys_down.reset();
  for (uint8_t kc : backend->query_pressed_keycodes()) {
    if (kc >= 8)
      physical_keys_down.set(static_cast<std::size_t>(kc));
  }

  // The physical bitmap is still needed to construct a correct wl_keyboard
  // enter event even when StateNotify has become authoritative. Only the
  // fallback xkb_state needs key-event replay.
  if (!xkb_st || xkb_state_from_server)
    return;

  // Replay every valid X11 keycode's *current* physical state against
  // xkb_st, discarding whatever it currently believes. Safe to do
  // unconditionally, including for keys whose direction hasn't actually
  // changed: xkb_state_update_key tracks its own per-key down-set
  // internally, so a same-direction call — even one that's flat-out wrong
  // about the key never having been down at all — is a harmless no-op
  // (verified against libxkbcommon before relying on it here, since
  // getting this wrong would mean occasionally corrupting good state
  // while trying to fix bad state).
  //
  // This is the actual fix for a modifier appearing stuck after a window
  // manager's own key binding fires: see
  // IWindowBackend::query_pressed_keycodes' doc comment for why WWindows'
  // own incrementally-tracked xkb_st can miss a release entirely in that
  // situation. Grounding it in what the server says is physically down
  // right now, on every real keyboard-focus transition, self-heals
  // regardless of how many events were missed or why.
  for (int kc = 8; kc <= 255; ++kc) {
    xkb_state_update_key(
        xkb_st, static_cast<xkb_keycode_t>(kc),
        physical_keys_down.test(static_cast<size_t>(kc)) ? XKB_KEY_DOWN
                                                         : XKB_KEY_UP);
  }
}

void InputTranslator::Impl::clear_keyboard_focus() {
  if (!kbd_focus)
    return;

  const uint32_t ser = wl_display_next_serial(display);
  wl_client *old_client = wl_resource_get_client(kbd_focus);
  for (auto *k : keyboards) {
    if (wl_resource_get_client(k) != old_client)
      continue;
    wl_keyboard_send_leave(k, ser, kbd_focus);
  }
  wl_list_remove(&kbd_focus_listener.link);
  kbd_focus = nullptr;
  // wl_keyboard.leave resets the receiving client's logical pressed-key set,
  // regardless of what remains physically held on the X11 keyboard.
  focus_keys_down.reset();
  if (kbd_focus_changed_cb)
    kbd_focus_changed_cb(nullptr);
}

void InputTranslator::Impl::assign_keyboard_focus(
    wl_resource *surf, uint32_t win,
    std::optional<uint8_t> key_press_being_forwarded) {
  if (!surf || surf == kbd_focus)
    return;

  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() { wl_display_flush_clients(display); }
  } guard{display};

  // Ground xkb_st in reality before computing what to send: a client
  // that's about to receive wl_keyboard.enter should see modifiers that
  // reflect what's actually held right now, not whatever this compositor
  // last happened to observe. Redundant with on_xkb_state_changed's
  // real-time server push when the XKB extension is available, but cheap,
  // and it's the only correction available at all when it isn't.
  resync_modifiers();

  // Defensive rather than assumed: clean up whatever kbd_focus currently
  // is first, the same way on_focus_out would, in case a FocusOut for it
  // never arrived (X does not guarantee every FocusIn is preceded by a
  // paired FocusOut in every reparenting/grab scenario, and this path is
  // also reached from on_key, which has no FocusOut-equivalent at all).
  clear_keyboard_focus();

  kbd_focus = surf;
  wl_resource_add_destroy_listener(surf, &kbd_focus_listener);
  {
    const uint32_t ser = wl_display_next_serial(display);
    wl_client *new_client = wl_resource_get_client(surf);
    wl_array keys;
    wl_array_init(&keys);

    focus_keys_down = physical_keys_down;
    if (key_press_being_forwarded)
      focus_keys_down.reset(
          static_cast<std::size_t>(*key_press_being_forwarded));

    // wl_keyboard.enter carries the currently-held evdev keycodes. They are
    // state, not synthetic key-press events; clients must not replay actions
    // from this list. A press which triggered our focus-recovery path is
    // excluded here and is sent as the immediately following key event.
    for (int kc = 8; kc <= 255; ++kc) {
      if (!focus_keys_down.test(static_cast<size_t>(kc)))
        continue;
      auto *slot = static_cast<uint32_t *>(wl_array_add(&keys, sizeof(uint32_t)));
      if (!slot)
        break;
      *slot = static_cast<uint32_t>(kc - 8);
    }

    for (auto *k : keyboards) {
      if (wl_resource_get_client(k) != new_client)
        continue;
      wl_keyboard_send_enter(k, ser, surf, &keys);
      send_modifiers(k, ser);
    }
    wl_array_release(&keys);
  }
  WW_LOG_DEBUG("[input] keyboard focus -> surface {} (x11={})",
              static_cast<void *>(surf), win);
  if (kbd_focus_changed_cb)
    kbd_focus_changed_cb(surf);
}

void InputTranslator::Impl::on_focus_in(uint32_t win) {
  if (!display)
    return;
  wl_resource *surf = surfaces->surface_for_window(win);
  if (!surf)
    return;
  assign_keyboard_focus(surf, win);
}

void InputTranslator::Impl::on_focus_out(uint32_t win) {
  if (!display)
    return;
  wl_resource *surf = surfaces->surface_for_window(win);
  // Only act if this is the window we currently believe holds focus —
  // FocusOut for anything else (a window losing focus to another window
  // of ours, which on_focus_in/assign_keyboard_focus above already
  // handles end-to-end) is not this compositor's problem.
  if (!surf || surf != kbd_focus)
    return;

  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() { wl_display_flush_clients(display); }
  } guard{display};

  WW_LOG_TRACE("[input] keyboard focus lost (x11={})", win);
  clear_keyboard_focus();
}

void InputTranslator::Impl::on_xkb_state_changed(uint32_t depressed,
                                                 uint32_t latched,
                                                 uint32_t locked,
                                                 uint32_t group) {
  if (!xkb_st)
    return;

  xkb_state_from_server = true;
  server_depressed_mods = depressed;
  server_latched_mods = latched;
  server_locked_mods = locked;
  server_group = group;

  // The server's own authoritative modifier/group tracking for the
  // keyboard device, broadcast to every client that asks for it
  // regardless of which window has focus or which client owns whatever
  // grab is active right now. This is the actual, complete fix for a
  // modifier appearing stuck after a window manager's own key binding
  // fires (see query_pressed_keycodes' doc comment for the mechanism):
  // unlike resync_modifiers(), which only corrects state at the moment
  // this compositor happens to reassign focus, this fires the instant
  // the server's own state changes — including the modifier's release
  // during a grab this compositor was never going to see directly at
  // all — with no dependency on a focus transition ever happening
  // afterward.
  // XKB StateNotify exposes only the effective group here, not the three
  // layout components separately. libxkbcommon's documented way to mirror a
  // single reported group is to place it in locked_layout and pass zero for
  // depressed_layout/latched_layout. Repeating the effective group in all
  // three components can multiply the group index and select the wrong layout.
  xkb_state_update_mask(xkb_st, depressed, latched, locked, 0, 0, group);

  // StateNotify also covers modifier transitions whose core KeyPress/Release
  // was consumed by a window-manager grab. Updating only our local xkb_state
  // leaves every Wayland client stale until some later key happens to cause
  // send_modifiers(). Push the authoritative state to the focused client now.
  if (!display || !kbd_focus)
    return;

  struct FlushGuard {
    wl_display *display;
    ~FlushGuard() { wl_display_flush_clients(display); }
  } guard{display};

  const uint32_t ser = wl_display_next_serial(display);
  wl_client *focus_client = wl_resource_get_client(kbd_focus);
  for (auto *kbd : keyboards) {
    if (wl_resource_get_client(kbd) == focus_client)
      send_modifiers(kbd, ser);
  }
}

void InputTranslator::Impl::on_xkb_keymap_changed() {
  if (!display || !xkb_ctx)
    return;

  const std::string previous_keymap = keymap_str;
  if (!recompile_keymap())
    return; // couldn't compile a replacement; keep serving the old one

  if (keymap_str == previous_keymap)
    return; // e.g. a NewKeyboardNotify for a device swap that landed on
            // the same effective layout — nothing to redistribute

  WW_LOG_INFO("[input] keymap changed — redistributing to {} keyboard(s)",
              keyboards.size());

  const uint32_t ser = wl_display_next_serial(display);
  for (auto *k : keyboards) {
    send_keymap_to(k);
    // Modifiers are per-focus state, unlike the keymap itself — only the
    // client that currently holds keyboard focus needs a fresh
    // wl_keyboard.modifiers to match, exactly as add_keyboard() already
    // does for a keyboard resource that's new rather than re-keymapped.
    if (kbd_focus &&
        wl_resource_get_client(kbd_focus) == wl_resource_get_client(k))
      send_modifiers(k, ser);
  }
  wl_display_flush_clients(display);
}

void InputTranslator::add_keyboard(wl_resource *resource) {
  impl_->keyboards.push_back(resource);
  WW_LOG_DEBUG("[input] keyboard resource added (total: {})",
               impl_->keyboards.size());

  if (!impl_->display || impl_->keymap_str.empty())
    return;

  impl_->send_keymap_to(resource);

  // If a surface already has keyboard focus AND it belongs to this same
  // client, send enter immediately so the client isn't left without focus
  // state.  We must not pass a surface owned by a different client —
  // libwayland validates object ownership and kills the connection if we do.
  if (impl_->kbd_focus && wl_resource_get_client(impl_->kbd_focus) ==
                              wl_resource_get_client(resource)) {
    const uint32_t ser = wl_display_next_serial(impl_->display);
    wl_array keys;
    wl_array_init(&keys);
    for (int kc = 8; kc <= 255; ++kc) {
      if (!impl_->focus_keys_down.test(static_cast<size_t>(kc)))
        continue;
      auto *slot = static_cast<uint32_t *>(wl_array_add(&keys, sizeof(uint32_t)));
      if (!slot)
        break;
      *slot = static_cast<uint32_t>(kc - 8);
    }
    wl_keyboard_send_enter(resource, ser, impl_->kbd_focus, &keys);
    wl_array_release(&keys);
    impl_->send_modifiers(resource, ser);
    WW_LOG_DEBUG("[input] new keyboard resource caught up to existing focus");
  }
}

void InputTranslator::remove_keyboard(wl_resource *resource) {
  std::erase(impl_->keyboards, resource);
  if (resource == impl_->kbd_focus)
    impl_->kbd_focus = nullptr;
  WW_LOG_DEBUG("[input] keyboard resource removed (total: {})",
               impl_->keyboards.size());
}

} // namespace ww
