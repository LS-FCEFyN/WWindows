#pragma once

#include "wwindows/ports/surface_interactions.hpp"
#include "wwindows/input/input_translator.hpp"
#include "wwindows/ports/window_backend.hpp"

#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include <bitset>
#include <optional>
#include <string>
#include <vector>

namespace ww {

struct InputTranslator::Impl {
  IWindowBackend *backend;
  ISurfaceInteractions *surfaces;
  wl_display *display{nullptr};

  // xkbcommon
  xkb_context *xkb_ctx{nullptr};
  xkb_keymap *xkb_km{nullptr};
  xkb_state *xkb_st{nullptr};
  std::string keymap_str; // null-terminated; sent verbatim to keyboard clients

  // StateNotify is the X server's authoritative modifier/group state. Once
  // one has arrived, xkb_st must only be updated from masks, never from key
  // events as well: mixing xkb_state_update_mask() and xkb_state_update_key()
  // leaves the state's internal pressed-key set inconsistent with those
  // authoritative masks (a missed release can reassert a stale modifier on
  // the next ordinary key press).
  bool xkb_state_from_server{false};
  uint32_t server_depressed_mods{0};
  uint32_t server_latched_mods{0};
  uint32_t server_locked_mods{0};
  uint32_t server_group{0};

  // Physical X11 key state and the key state currently established by
  // wl_keyboard.enter/key for the focused client are deliberately separate.
  // A leave resets the latter per Wayland protocol, while physical keys may
  // remain held across focus changes. Bit positions use X11 keycodes (the
  // evdev/Wayland keycode is X11 keycode - 8).
  std::bitset<256> physical_keys_down{};
  std::bitset<256> focus_keys_down{};

  // Active Wayland resources (one entry per client-side bind)
  std::vector<wl_resource *> pointers;
  std::vector<wl_resource *> keyboards;

  // Current focus surfaces (wl_surface resources, may be null)
  wl_resource *ptr_focus{nullptr};
  wl_resource *kbd_focus{nullptr};

  // Surface-local coordinates (window-geometry origin already added) from
  // the most recent enter/motion — cached
  // purely so begin_drag() can synthesize its initial on_enter without
  // waiting for the next real motion event.
  int16_t last_local_x{0};
  int16_t last_local_y{0};

  InputTranslator::KeyboardFocusChangedCallback kbd_focus_changed_cb;

  wl_listener ptr_focus_listener{};
  wl_listener kbd_focus_listener{};

  static void on_ptr_focus_destroy(wl_listener *listener, void *);
  static void on_kbd_focus_destroy(wl_listener *listener, void *);

  // --- wl_data_device drag-and-drop redirect --------------------------------
  bool drag_active{false};
  InputTranslator::DragCallbacks drag_cbs;
  wl_resource *drag_target{nullptr}; // surface currently "entered" by the drag

  // --- wl_pointer.set_cursor -----------------------------------------------
  // A client's cursor is a wl_surface whose commits SurfaceManager turns into
  // window-system cursor objects (see ISurfaceInteractions::set_cursor_surface).
  // What lives here is only *which* cursor applies and *where* it is shown:
  //   cursor_surface   the surface currently holding the cursor role
  //   cursor_client    the client whose set_cursor is in effect -- a cursor is
  //                    only ever shown over that client's own windows, and
  //                    every other window gets the window system's default
  //                    until its client asks for something (protocol: the
  //                    cursor is undefined after enter until set_cursor)
  //   cursor_hidden    that client called set_cursor(null)
  //   ptr_focus_window the backend window the pointer is currently over, i.e.
  //                    the window to put the cursor on
  wl_resource *cursor_surface{nullptr};
  wl_client *cursor_client{nullptr}; // compared, never dereferenced
  bool cursor_hidden{false};
  uint32_t ptr_focus_window{0};
  wl_listener cursor_surface_listener{};
  static void on_cursor_surface_destroy(wl_listener *listener, void *);
  void set_cursor(wl_resource *pointer_res, wl_resource *surface_res,
                  int32_t hotspot_x, int32_t hotspot_y);
  // The cursor that should be showing over the current pointer focus: the
  // client's committed image, the blank cursor if it hid the pointer, or 0
  // for "window system default".
  [[nodiscard]] uint32_t cursor_for_focus() const;
  void apply_cursor_to_focus();
  void on_cursor_image_changed();

  Impl(IWindowBackend &backend_ref, ISurfaceInteractions &surfaces_ref);

  // --- low-level send helpers ---------------------------------------------
  // Defined in InputTranslator_pointer.cpp / InputTranslator_keyboard.cpp.
  void ptr_frame(wl_resource *p);
  void send_modifiers(wl_resource *kbd, uint32_t serial);
  void send_keymap_to(wl_resource *kbd);

  // Converts raw X11 window-local coordinates into surface-local ones by
  // adding `surf`'s window-geometry origin (see
  // ISurfaceInteractions::surface_local_origin). The single correction point
  // shared by wl_pointer and wl_data_device positions.
  void to_surface_local(wl_resource *surf, int16_t &x, int16_t &y) const;

  // --- event handlers (invoked from IWindowBackend callbacks) --------------
  // Defined in InputTranslator_lifecycle.cpp / _pointer.cpp / _keyboard.cpp.
  void on_expose(uint32_t win);
  void on_enter(uint32_t win, int16_t x, int16_t y, int16_t root_x,
               int16_t root_y);
  void on_leave(uint32_t win);
  void on_motion(uint32_t win, int16_t x, int16_t y, int16_t root_x,
                int16_t root_y, uint32_t time);
  void on_button(uint32_t win, uint8_t btn, bool pressed, uint32_t time);
  void on_key(uint32_t win, uint8_t x11_keycode, bool pressed, uint32_t time);
  // Real X11 keyboard focus changing, per IWindowBackend::InputCallbacks'
  // on_focus_in/on_focus_out. Defined in InputTranslator_keyboard.cpp,
  // alongside resync_modifiers() and clear_keyboard_focus() which they
  // share.
  void on_focus_in(uint32_t win);
  void on_focus_out(uint32_t win);
  // The key transition associated with an XKB StateNotify. This can be the
  // only indication of a modifier release when a window-manager grab swallows
  // the corresponding core KeyRelease.
  void on_xkb_key_transition(uint8_t x11_keycode, bool pressed, uint32_t time);
  // A genuine XKB StateNotify from the server — see
  // IWindowBackend::InputCallbacks::on_xkb_state_changed's doc comment.
  // Defined in InputTranslator_keyboard.cpp.
  void on_xkb_state_changed(uint32_t depressed, uint32_t latched,
                            uint32_t locked, uint32_t group);
  // The server's keymap itself changed — see
  // IWindowBackend::InputCallbacks::on_xkb_keymap_changed's doc comment.
  // Defined in InputTranslator_keyboard.cpp.
  void on_xkb_keymap_changed();

  // The keyboard auto-repeat announced to clients (wl_keyboard.repeat_info).
  // The window system owns the setting; this is the last value it reported,
  // refreshed by refresh_key_repeat(). The initial value is only what is
  // announced when the backend cannot say at all.
  IWindowBackend::KeyRepeat key_repeat{25, 200};
  // Re-reads the setting from the backend. True if it changed.
  bool refresh_key_repeat();
  // IWindowBackend::InputCallbacks::on_xkb_repeat_changed: re-read, and if the
  // user's setting really changed, tell every keyboard.
  void on_xkb_repeat_changed();
  void send_repeat_info_to(wl_resource *kbd);
  // (Re)compiles xkb_km/xkb_st/keymap_str from
  // backend->keyboard_keymap_string(), falling back to
  // xkb_keymap_new_from_names(ctx, nullptr, ...) if the backend can't
  // provide a real one. On failure, leaves whatever keymap was already
  // in place untouched and returns false — a failed *re*compile should
  // never take away a keyboard that was already working. Public (unlike
  // most of Impl's other internal helpers) because set_display() — a
  // method of the outer InputTranslator wrapper, not of Impl — needs to
  // call it directly for the first compile, exactly as it already calls
  // on_xkb_state_changed() above; on_xkb_keymap_changed() calls it again
  // for every one after.
  bool recompile_keymap();

private:
  // Sends wl_keyboard.leave for whatever currently holds kbd_focus and
  // nulls it, if anything does. Idempotent (a no-op when kbd_focus is
  // already null) so on_focus_in can call it defensively before assigning
  // a new focus without needing to know whether on_focus_out already ran
  // for the surface it's replacing.
  void clear_keyboard_focus();
  // Refreshes the physical key bitmap from the window system. When the
  // backend has no authoritative XKB StateNotify stream, also re-derives
  // xkb_st from that bitmap; when StateNotify is active, xkb_st is mask-only
  // and must not be mixed with key-event updates.
  void resync_modifiers();
  // The actual focus-transfer: resync, leave whatever kbd_focus currently
  // is, assign kbd_focus = surf, send enter + modifiers, fire the
  // callback. Shared by on_focus_in (the prompt, common-case path — a
  // window manager that always sends FocusIn/FocusOut promptly) and
  // on_key (the backstop: a key event proves beyond doubt that `win`
  // holds real input focus *right now*, whatever did or didn't happen
  // before it, so on_key corrects kbd_focus itself rather than assuming
  // some earlier FocusIn already got it right — a window manager that
  // skips re-announcing focus it believes hasn't changed, e.g. after a
  // workspace switch back to an already-"focused" window, would otherwise
  // leave kbd_focus stuck on nothing forever with no event left to fix
  // it). A no-op if surf already is kbd_focus.
  void assign_keyboard_focus(
      wl_resource *surf, uint32_t win,
      std::optional<uint8_t> key_press_being_forwarded = std::nullopt);
};

} // namespace ww
