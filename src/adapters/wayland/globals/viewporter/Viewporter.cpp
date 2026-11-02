#include "wwindows/adapters/wayland/globals/viewporter.hpp"
#include <wwindows/support/log.hpp>

#include <wwindows/adapters/wayland/protocols/viewporter-server-protocol.h>
#include <wayland-server-core.h>

#include "wwindows/core/surface_manager.hpp"

#include <vector>
#include <algorithm>

namespace ww {

// User data for the wp_viewporter global (manager).
class ViewporterGlobal {
public:
  // Every wp_viewport a client holds; each removes itself when destroyed.
  std::vector<wl_resource *> viewports;
  wl_listener display_destroy{};
};

// User data for each wp_viewport (per-surface object).
struct ViewportData {
  ViewporterGlobal *global{nullptr};
  wl_resource *surface{nullptr};
};

// Resource destroy callback for wp_viewport - called when the resource is destroyed.
static void viewport_resource_destroyed(wl_resource *resource) {
  auto *data = static_cast<ViewportData *>(wl_resource_get_user_data(resource));
  if (!data)
    return;

  // When the viewport is destroyed, the crop and scale state is removed
  // from the wl_surface. The change will be applied on the next commit.
  if (data->surface) {
    auto *mgr = static_cast<SurfaceManager *>(wl_resource_get_user_data(data->surface));
    if (mgr) {
      const wl_fixed_t minus_one = wl_fixed_from_int(-1);
      mgr->set_pending_viewport_source(data->surface, minus_one, minus_one, minus_one, minus_one);
      mgr->set_pending_viewport_destination(data->surface, -1, -1);
      WW_LOG_TRACE("[viewporter] viewport destroyed, state unset (surface={})",
                   static_cast<void *>(data->surface));
    }
  }

  if (data->global) {
    std::erase(data->global->viewports, resource);
  }
  delete data;
}

static void viewport_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

static void viewport_set_source(wl_client *, wl_resource *resource,
                                wl_fixed_t x, wl_fixed_t y,
                                wl_fixed_t width, wl_fixed_t height) {
  auto *data = static_cast<ViewportData *>(wl_resource_get_user_data(resource));
  if (!data || !data->surface) {
    wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                           "viewport_set_source: surface already destroyed");
    return;
  }

  // Check for unset: all four are -1.0
  const wl_fixed_t minus_one = wl_fixed_from_int(-1);
  if (x == minus_one && y == minus_one && width == minus_one && height == minus_one) {
    auto *mgr = static_cast<SurfaceManager *>(wl_resource_get_user_data(data->surface));
    if (mgr)
      mgr->set_pending_viewport_source(data->surface, minus_one, minus_one, minus_one, minus_one);
    WW_LOG_TRACE("[viewporter] set_source unset (surface={})",
                 static_cast<void *>(data->surface));
    return;
  }

  // Validate: negative x or y, or zero/negative width or height
  if (x < 0 || y < 0 || width <= 0 || height <= 0) {
    wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                           "viewport_set_source: negative or zero values in width or height");
    return;
  }

  auto *mgr = static_cast<SurfaceManager *>(wl_resource_get_user_data(data->surface));
  if (mgr)
    mgr->set_pending_viewport_source(data->surface, x, y, width, height);

  WW_LOG_TRACE("[viewporter] set_source x={} y={} w={} h={} (surface={})",
               wl_fixed_to_double(x), wl_fixed_to_double(y),
               wl_fixed_to_double(width), wl_fixed_to_double(height),
               static_cast<void *>(data->surface));
}

static void viewport_set_destination(wl_client *, wl_resource *resource,
                                     int32_t width, int32_t height) {
  auto *data = static_cast<ViewportData *>(wl_resource_get_user_data(resource));
  if (!data || !data->surface) {
    wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                           "viewport_set_destination: surface already destroyed");
    return;
  }

  // Check for unset: both are -1
  if (width == -1 && height == -1) {
    // Destination size is unset
    WW_LOG_TRACE("[viewporter] set_destination unset (surface={})",
                 static_cast<void *>(data->surface));
    return;
  }

  // Validate: zero or negative values
  if (width <= 0 || height <= 0) {
    wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                           "viewport_set_destination: zero or negative values");
    return;
  }

  WW_LOG_TRACE("[viewporter] set_destination {}x{} (surface={})",
               width, height, static_cast<void *>(data->surface));
}

static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

static void viewporter_get_viewport(wl_client *client, wl_resource *resource,
                                    uint32_t id, wl_resource *surface) {
  auto *global = static_cast<ViewporterGlobal *>(wl_resource_get_user_data(resource));

  // Check if surface already has a viewport
  for (wl_resource *vp : global->viewports) {
    auto *vp_data = static_cast<ViewportData *>(wl_resource_get_user_data(vp));
    if (vp_data && vp_data->surface == surface) {
      wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                             "the surface already has a viewport object associated");
      return;
    }
  }

  auto *data = new ViewportData{};
  data->global = global;
  data->surface = surface;

  wl_resource *vp = wl_resource_create(client, &wp_viewport_interface,
                                       wl_resource_get_version(resource), id);
  wl_resource_set_implementation(vp, &viewport_impl, data, viewport_resource_destroyed);
  global->viewports.push_back(vp);

  WW_LOG_TRACE("[viewporter] get_viewport created (surface={}, viewport={})",
               static_cast<void *>(surface), static_cast<void *>(vp));
}

static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void bind_viewporter(wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
  auto *global = static_cast<ViewporterGlobal *>(data);
  wl_resource *r = wl_resource_create(
      client, &wp_viewporter_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(r, &viewporter_impl, global, nullptr);
}

Result<void *> register_viewporter(wl_display *display) {
  auto *global = new ViewporterGlobal{};

  if (!wl_global_create(display, &wp_viewporter_interface, 1,
                        global, bind_viewporter)) {
    delete global;
    return std::unexpected(Error::wayland_init(
        "Failed to create wp_viewporter global"));
  }

  global->display_destroy.notify = [](wl_listener *listener, void *) {
    ViewporterGlobal *g = wl_container_of(listener, g, display_destroy);
    for (wl_resource *r : g->viewports) {
      auto *vp_data = static_cast<ViewportData *>(wl_resource_get_user_data(r));
      if (vp_data) {
        vp_data->global = nullptr;
        vp_data->surface = nullptr;
      }
      wl_resource_set_user_data(r, nullptr);
    }
    delete g;
  };
  wl_display_add_destroy_listener(display, &global->display_destroy);

  WW_LOG_INFO("[wayland] viewporter v1 registered");
  return global;
}

} // namespace ww