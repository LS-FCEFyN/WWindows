// wl_seat and the wl_pointer / wl_keyboard device objects it vends. These
// three interfaces are kept together (rather than three more files) because
// none of wl_pointer/wl_keyboard is reachable except through wl_seat, and
// all three exist purely to route input events to InputTranslator. See
// impl.hpp for how this fits with the other files in this directory.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"
#include "wwindows/input/input_translator.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/support/log.hpp>

#include <cstdint>

namespace ww {

namespace {
// -- wl_pointer -----------------------------------------------------------
void pointer_set_cursor(wl_client *, wl_resource *resource,
                        uint32_t /*serial*/, wl_resource *surface,
                        int32_t hotspot_x, int32_t hotspot_y) {
  auto *t = static_cast<InputTranslator *>(wl_resource_get_user_data(resource));
  if (t)
    t->set_cursor(resource, surface, hotspot_x, hotspot_y);
}
void pointer_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

const struct wl_pointer_interface pointer_iface = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};

void pointer_resource_destroy(wl_resource *r) {
  auto *t = static_cast<InputTranslator *>(wl_resource_get_user_data(r));
  if (t)
    t->remove_pointer(r);
}

// -- wl_keyboard ------------------------------------------------------------
void keyboard_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

const struct wl_keyboard_interface keyboard_iface = {
    .release = keyboard_release,
};

void keyboard_resource_destroy(wl_resource *r) {
  auto *t = static_cast<InputTranslator *>(wl_resource_get_user_data(r));
  if (t)
    t->remove_keyboard(r);
}

// -- wl_seat ----------------------------------------------------------------
void seat_get_pointer(wl_client *client, wl_resource *resource,
                      uint32_t id) {
  auto *translator =
      static_cast<InputTranslator *>(wl_resource_get_user_data(resource));
  wl_resource *ptr = wl_resource_create(client, &wl_pointer_interface,
                                        wl_resource_get_version(resource), id);
  wl_resource_set_implementation(ptr, &pointer_iface, translator,
                                 pointer_resource_destroy);
  if (translator)
    translator->add_pointer(ptr);
}

void seat_get_keyboard(wl_client *client, wl_resource *resource,
                       uint32_t id) {
  auto *translator =
      static_cast<InputTranslator *>(wl_resource_get_user_data(resource));
  wl_resource *kbd = wl_resource_create(client, &wl_keyboard_interface,
                                        wl_resource_get_version(resource), id);
  wl_resource_set_implementation(kbd, &keyboard_iface, translator,
                                 keyboard_resource_destroy);
  if (translator)
    translator->add_keyboard(kbd);
}

void seat_get_touch(wl_client *, wl_resource *, uint32_t) {} // not advertised
void seat_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

const struct wl_seat_interface seat_iface = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};
} // namespace

void bind_seat(wl_client *client, void *data, uint32_t version,
              uint32_t id) {
  wl_resource *r = wl_resource_create(client, &wl_seat_interface,
                                      static_cast<int>(version), id);
  // data == InputTranslator* — forwarded to seat resource so
  // get_pointer/keyboard can use it
  wl_resource_set_implementation(r, &seat_iface, data, nullptr);
  if (version >= WL_SEAT_NAME_SINCE_VERSION)
    wl_seat_send_name(r, "seat0");
  wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
  WW_LOG_DEBUG("[wayland] client bound wl_seat v{}", version);
}

} // namespace ww
