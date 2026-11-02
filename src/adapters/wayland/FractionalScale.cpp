#include "wwindows/adapters/wayland/globals/fractional_scale.hpp"
#include <wwindows/support/log.hpp>
#include <wwindows/adapters/wayland/policy/scale.hpp>

#include <wwindows/adapters/wayland/protocols/fractional-scale-v1-server-protocol.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <vector>
#include <cmath>

namespace ww {

uint32_t compute_scale_120ths(int32_t width_px, int32_t width_mm) { return wayland::policy::compute_scale_120ths(width_px, width_mm); }

class FractionalScaleGlobal {
public:
  uint32_t scale_120ths{120};
  // Every wp_fractional_scale_v1 a client holds; each removes itself when
  // destroyed, so an update can reach them.
  std::vector<wl_resource *> objects;
  wl_listener display_destroy{};
};

static void fractional_scale_resource_destroyed(wl_resource *r) {
  auto *global =
      static_cast<FractionalScaleGlobal *>(wl_resource_get_user_data(r));
  if (!global)
    return; // the global was freed with the display first
  std::erase(global->objects, r);
}

static void fractional_scale_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}

static const struct wp_fractional_scale_v1_interface fractional_scale_impl = {
    .destroy = fractional_scale_destroy,
};

static void fractional_scale_manager_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}

static void fractional_scale_manager_get_fractional_scale(
    wl_client *client, wl_resource *resource, uint32_t id,
    wl_resource * /*surface*/) {
  auto *mgr_data = static_cast<FractionalScaleGlobal *>(
      wl_resource_get_user_data(resource));

  wl_resource *fs =
      wl_resource_create(client, &wp_fractional_scale_v1_interface,
                         wl_resource_get_version(resource), id);
  wl_resource_set_implementation(fs, &fractional_scale_impl, mgr_data,
                                 fractional_scale_resource_destroyed);
  mgr_data->objects.push_back(fs);

  wp_fractional_scale_v1_send_preferred_scale(fs, mgr_data->scale_120ths);
  WW_LOG_TRACE("[wayland] fractional_scale.get_fractional_scale -> {}/120",
               mgr_data->scale_120ths);
}

static const struct wp_fractional_scale_manager_v1_interface
    fractional_scale_manager_impl = {
        .destroy = fractional_scale_manager_destroy,
        .get_fractional_scale = fractional_scale_manager_get_fractional_scale,
};

static void bind_fractional_scale_manager(wl_client *client, void *data,
                                          uint32_t version, uint32_t id) {
  auto *mgr_data = static_cast<FractionalScaleGlobal *>(data);
  wl_resource *r = wl_resource_create(
      client, &wp_fractional_scale_manager_v1_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(r, &fractional_scale_manager_impl, mgr_data,
                                 nullptr);
}

Result<FractionalScaleGlobal *> register_fractional_scale(wl_display *display,
                                                          uint32_t scale_120ths) {
  auto *data = new FractionalScaleGlobal{};
  data->scale_120ths = scale_120ths;

  if (!wl_global_create(display, &wp_fractional_scale_manager_v1_interface, 1,
                        data, bind_fractional_scale_manager)) {
    delete data;
    return std::unexpected(Error::wayland_init(
        "Failed to create wp_fractional_scale_manager_v1 global"));
  }
  // libwayland has no destructor hook for a global's user data; free it with
  // the display, as the wl_output global does.
  data->display_destroy.notify = [](wl_listener *listener, void *) {
    FractionalScaleGlobal *g = nullptr;
    g = wl_container_of(listener, g, display_destroy);
    for (wl_resource *r : g->objects)
      wl_resource_set_user_data(r, nullptr);
    delete g;
  };
  wl_display_add_destroy_listener(display, &data->display_destroy);

  WW_LOG_INFO("[wayland] fractional-scale v1 registered (scale={}/120 = {:.3f}x)",
              scale_120ths, static_cast<double>(scale_120ths) / 120.0);
  return data;
}

void update_fractional_scale(FractionalScaleGlobal &global,
                             uint32_t scale_120ths) {
  if (global.scale_120ths == scale_120ths)
    return;
  global.scale_120ths = scale_120ths;
  WW_LOG_INFO("[wayland] preferred fractional scale is now {}/120 = {:.3f}x — "
              "telling {} object(s)",
              scale_120ths, static_cast<double>(scale_120ths) / 120.0,
              global.objects.size());
  for (wl_resource *r : global.objects)
    wp_fractional_scale_v1_send_preferred_scale(r, scale_120ths);
}

} // namespace ww
