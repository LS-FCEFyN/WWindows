// wl_output — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/support/log.hpp>

#include <cstdint>

namespace ww {

namespace {
void output_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

const struct wl_output_interface output_iface = {
    .release = output_release,
};

// A client must be given *some* refresh rate; 0 from the window system means
// it could not say (see OutputState::refresh_mhz).
constexpr int32_t kFallbackRefreshMhz = 60000;

// Everything a client is told about an output, in the order the protocol
// wants it, ending with `done` where the version has it. Used both when a
// client binds and when the window system later changes the screen.
void send_output_state(wl_resource *r, const OutputState &state) {
  wl_output_send_geometry(r, 0, 0, state.width_mm, state.height_mm,
                          WL_OUTPUT_SUBPIXEL_UNKNOWN, "wwindows", "monitor",
                          WL_OUTPUT_TRANSFORM_NORMAL);
  // The X screen is presented 1:1; HiDPI is expressed by wp_fractional_scale.
  wl_output_send_scale(r, 1);
  wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                      state.width_px, state.height_px,
                      state.refresh_mhz ? static_cast<int32_t>(state.refresh_mhz)
                                        : kFallbackRefreshMhz);
  if (wl_resource_get_version(r) >= WL_OUTPUT_DONE_SINCE_VERSION)
    wl_output_send_done(r);
}

void output_resource_destroyed(wl_resource *r) {
  auto *global = static_cast<OutputGlobal *>(wl_resource_get_user_data(r));
  if (!global)
    return; // the global was freed with the display first
  std::erase(global->bound, r);
}
} // namespace

void bind_output(wl_client *client, void *data, uint32_t version,
                 uint32_t id) {
  auto *global = static_cast<OutputGlobal *>(data);
  wl_resource *r = wl_resource_create(client, &wl_output_interface,
                                      static_cast<int>(version), id);
  wl_resource_set_implementation(r, &output_iface, global,
                                 output_resource_destroyed);
  global->bound.push_back(r);

  send_output_state(r, global->state);
  WW_LOG_DEBUG("[wayland] client bound wl_output v{} ({}x{}px)", version,
               global->state.width_px, global->state.height_px);
}

void update_output(OutputGlobal &global, const OutputState &state) {
  if (global.state == state)
    return;
  global.state = state;
  WW_LOG_INFO("[wayland] wl_output is now {}x{}px, {}x{}mm, {} mHz — telling "
              "{} client resource(s)",
              state.width_px, state.height_px, state.width_mm, state.height_mm,
              state.refresh_mhz, global.bound.size());
  for (wl_resource *r : global.bound)
    send_output_state(r, state);
}

} // namespace ww
