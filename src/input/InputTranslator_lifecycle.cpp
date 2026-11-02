#include "wwindows/input/detail/input_translator_impl.hpp"
#include "wwindows/ports/surface_interactions.hpp"
#include <wwindows/support/log.hpp>

namespace ww {

void InputTranslator::Impl::on_ptr_focus_destroy(wl_listener *listener, void *) {
  Impl *im = wl_container_of(listener, im, ptr_focus_listener);
  WW_LOG_TRACE("[input] pointer focus surface destroyed out from under us");
  im->ptr_focus = nullptr;
  // The window may be going away with the surface; never touch it again.
  im->ptr_focus_window = 0;
}

void InputTranslator::Impl::on_kbd_focus_destroy(wl_listener *listener, void *) {
  Impl *im = wl_container_of(listener, im, kbd_focus_listener);
  WW_LOG_TRACE("[input] keyboard focus surface destroyed out from under us");
  im->kbd_focus = nullptr;
  im->focus_keys_down.reset();
  if (im->kbd_focus_changed_cb)
    im->kbd_focus_changed_cb(nullptr);
}

InputTranslator::Impl::Impl(IWindowBackend &backend_ref, ISurfaceInteractions &surfaces_ref)
    : backend(&backend_ref), surfaces(&surfaces_ref) {
  ptr_focus_listener.notify = on_ptr_focus_destroy;
  kbd_focus_listener.notify = on_kbd_focus_destroy;
}

void InputTranslator::Impl::on_expose(uint32_t win) { surfaces->on_expose(win); }

InputTranslator::InputTranslator(IWindowBackend &backend, ISurfaceInteractions &surfaces)
    : impl_{std::make_unique<Impl>(backend, surfaces)} {
  auto *im = impl_.get();

  backend.set_input_callbacks({
      .on_motion = [im](uint32_t w, int16_t x, int16_t y, int16_t rx,
                        int16_t ry, uint32_t t) { im->on_motion(w, x, y, rx, ry, t); },
      .on_button = [im](uint32_t w, uint8_t b, bool p,
                        uint32_t t) { im->on_button(w, b, p, t); },
      .on_key = [im](uint32_t w, uint8_t k, bool p,
                     uint32_t t) { im->on_key(w, k, p, t); },
      .on_enter = [im](uint32_t w, int16_t x, int16_t y, int16_t rx,
                       int16_t ry) { im->on_enter(w, x, y, rx, ry); },
      .on_leave = [im](uint32_t w) { im->on_leave(w); },
      .on_expose = [im](uint32_t w) { im->on_expose(w); },
      .on_focus_in = [im](uint32_t w) { im->on_focus_in(w); },
      .on_focus_out = [im](uint32_t w) { im->on_focus_out(w); },
      .on_xkb_key_transition =
          [im](uint8_t k, bool p, uint32_t t) {
            im->on_xkb_key_transition(k, p, t);
          },
      .on_xkb_state_changed =
          [im](uint32_t d, uint32_t l, uint32_t lo, uint32_t g) {
            im->on_xkb_state_changed(d, l, lo, g);
          },
      .on_xkb_keymap_changed = [im] { im->on_xkb_keymap_changed(); },
      .on_xkb_repeat_changed = [im] { im->on_xkb_repeat_changed(); },
  });
}

InputTranslator::~InputTranslator() {
  WW_LOG_INFO("[input] translator shutting down ({} pointer(s), {} "
              "keyboard(s) attached)",
              impl_->pointers.size(), impl_->keyboards.size());

  // Clear input callbacks first — the backend outlives us only because of
  // member declaration order in WWindows::Impl, but clearing here is
  // belt-and-suspenders.
  impl_->backend->set_input_callbacks({});

  // The focus listeners live inside this object but are linked into the
  // surfaces' destroy signals. The Wayland display is torn down after us
  // (see application.cpp), so a surface still holding pointer or keyboard
  // focus would fire into freed memory at shutdown.
  if (impl_->ptr_focus)
    wl_list_remove(&impl_->ptr_focus_listener.link);
  if (impl_->kbd_focus)
    wl_list_remove(&impl_->kbd_focus_listener.link);

  // SurfaceManager (destroyed after us) still holds a callback into this
  // object for the cursor surface; take the role off it so a late commit
  // cannot call back into freed memory.
  if (impl_->cursor_surface) {
    impl_->surfaces->clear_cursor_surface(impl_->cursor_surface);
    wl_list_remove(&impl_->cursor_surface_listener.link);
    impl_->cursor_surface = nullptr;
  }

  if (impl_->xkb_st)
    xkb_state_unref(impl_->xkb_st);
  if (impl_->xkb_km)
    xkb_keymap_unref(impl_->xkb_km);
  if (impl_->xkb_ctx)
    xkb_context_unref(impl_->xkb_ctx);
}

void InputTranslator::set_display(wl_display *display) {
  impl_->display = display;

  impl_->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!impl_->xkb_ctx) {
    WW_LOG_ERROR("[input] xkb_context_new failed — keyboard input disabled");
    return;
  }

  // Prefer the session's actual keymap (see
  // IWindowBackend::keyboard_keymap_string's doc comment for why this
  // isn't optional polish) — a non-default layout or any personal
  // setxkbmap customization otherwise silently disagrees with this
  // compositor's own idea of every key, not just modifiers, for as long
  // as it runs. Falls back to bare defaults only if the backend can't
  // provide one at all (extension unavailable, or a backend — such as
  // the test fake — that never claims to have a real session to ask) —
  // or, if IWindowBackend::InputCallbacks::on_xkb_keymap_changed ever
  // fires, isn't the last word either: see recompile_keymap().
  impl_->recompile_keymap();
  impl_->refresh_key_repeat();
}

bool InputTranslator::Impl::recompile_keymap() {
  const std::optional<std::string> real_keymap =
      backend->keyboard_keymap_string();

  xkb_keymap *new_km = nullptr;
  if (real_keymap) {
    new_km = xkb_keymap_new_from_string(xkb_ctx, real_keymap->c_str(),
                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!new_km)
      WW_LOG_WARN("[input] the session's real keymap failed to compile — "
                  "falling back to a default one");
  }
  if (!new_km)
    new_km = xkb_keymap_new_from_names(xkb_ctx, nullptr,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!new_km) {
    WW_LOG_ERROR("[input] xkb_keymap_new_from_names failed — keyboard input "
                 "disabled");
    return false;
  }

  xkb_state *new_st = xkb_state_new(new_km);
  if (!new_st) {
    WW_LOG_ERROR("[input] xkb_state_new failed — keyboard input disabled");
    xkb_keymap_unref(new_km);
    return false;
  }

  // If we've already received XKB StateNotify, keep this replacement state
  // on the same authoritative mask-only path. In particular, do not follow
  // update_mask() with update_key() on this object: those are client-side
  // mask snapshots and server-side key transitions, respectively, and mixing
  // them recreates stale per-key modifier state.
  if (xkb_state_from_server) {
    // server_group is the effective group supplied by XKB StateNotify, not
    // the individual depressed/latched/locked layout components.
    xkb_state_update_mask(new_st, server_depressed_mods, server_latched_mods,
                          server_locked_mods, 0, 0, server_group);
  }

  // Only now, with a fully-formed replacement in hand, touch the
  // previous one — a recompile that fails partway through (either branch
  // above) must leave whatever keymap was already serving clients alone
  // rather than leaving them with none at all.
  if (xkb_st)
    xkb_state_unref(xkb_st);
  if (xkb_km)
    xkb_keymap_unref(xkb_km);
  xkb_km = new_km;
  xkb_st = new_st;

  char *raw = xkb_keymap_get_as_string(xkb_km, XKB_KEYMAP_FORMAT_TEXT_V1);
  keymap_str = raw;
  ::free(raw);

  WW_LOG_INFO("[input] xkb keymap ready ({} bytes, {})", keymap_str.size(),
              real_keymap ? "session keymap" : "default keymap");
  return true;
}

} // namespace ww
