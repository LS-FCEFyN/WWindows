// wl_data_device_manager — the global that creates wl_data_source
// (Source.cpp) and wl_data_device (Device.cpp) objects, and owns
// register_data_device_manager(), the public entry point declared in
// DataDevice.hpp. See impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/data_device.hpp"
#include "wwindows/adapters/wayland/globals/data_device/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"
#include "wwindows/core/surface_manager.hpp"
#include "wwindows/input/input_translator.hpp"

#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ww {

namespace {
void manager_create_data_source(wl_client *client, wl_resource *resource,
                                uint32_t id) {
  auto *mgr =
      static_cast<DataDeviceManagerState *>(wl_resource_get_user_data(resource));
  wl_resource *src_res = wl_resource_create(
      client, &wl_data_source_interface, wl_resource_get_version(resource), id);
  auto *data = new SourceData();
  data->mgr = mgr;
  wl_resource_set_implementation(src_res, &data_source_iface, data,
                                 source_resource_destroy);
}

void manager_get_data_device(wl_client *client, wl_resource *resource,
                             uint32_t id, wl_resource * /*seat*/) {
  auto *mgr =
      static_cast<DataDeviceManagerState *>(wl_resource_get_user_data(resource));
  wl_resource *dev = wl_resource_create(
      client, &wl_data_device_interface, wl_resource_get_version(resource), id);
  wl_resource_set_implementation(dev, &data_device_iface, mgr,
                                 device_resource_destroy);
  if (!mgr)
    return;
  mgr->devices.push_back(dev);
  // Every new data_device immediately learns the current selection state
  // (including "none") rather than waiting for a focus change that may
  // never come — clipboard metadata isn't sensitive, only the actual
  // bytes are (gated by an explicit receive()).
  send_selection_offer_to_device(*mgr, dev);
}

#ifdef WWINDOWS_HAVE_WL_DATA_DEVICE_MANAGER_RELEASE
void manager_release(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}
#endif

const struct wl_data_device_manager_interface manager_iface = {
    .create_data_source = manager_create_data_source,
    .get_data_device = manager_get_data_device,
#ifdef WWINDOWS_HAVE_WL_DATA_DEVICE_MANAGER_RELEASE
    // .release was added to wl_data_device_manager_interface by newer
    // libwayland (detected at configure time; not present on every
    // distro's older libwayland, hence the #ifdef). The
    // wl_data_device_manager global below is advertised at version 3,
    // which does carry this request in the upstream protocol — but a
    // client can only ever reach this member if the *linked* libwayland's
    // struct actually has it, so guard it the same way
    // wl_compositor_interface.release and wl_surface_interface.get_release
    // are guarded above in core_globals/Compositor.cpp / Surface.cpp.
    .release = manager_release,
#endif
};

void bind_data_device_manager(wl_client *client, void *data, uint32_t version,
                              uint32_t id) {
  wl_resource *r = wl_resource_create(client, &wl_data_device_manager_interface,
                                      static_cast<int>(version), id);
  wl_resource_set_implementation(r, &manager_iface, data, nullptr);
}
} // namespace

VoidResult register_data_device_manager(wl_display *display,
                                        SurfaceManager &surface_manager,
                                        InputTranslator &input_translator,
                                        IWindowBackend &backend) {
  auto *mgr = new DataDeviceManagerState();
  mgr->display = display;
  mgr->surface_manager = &surface_manager;
  mgr->input_translator = &input_translator;
  mgr->backend = &backend;

  if (!wl_global_create(display, &wl_data_device_manager_interface, 3, mgr,
                        bind_data_device_manager)) {
    delete mgr;
    return std::unexpected(
        Error::wayland_init("Failed to create wl_data_device_manager global"));
  }

  // The global owns `mgr` from here on, and libwayland offers no destructor
  // hook for a global's user data -- so hang the delete off the display
  // instead. Without it nothing ever frees this object.
  mgr->display_destroy.notify = [](wl_listener *listener, void *) {
    DataDeviceManagerState *state = nullptr;
    delete wl_container_of(listener, state, display_destroy);
  };
  wl_display_add_destroy_listener(display, &mgr->display_destroy);

  backend.set_clipboard_send_callback(
      [mgr](const std::string &mime_type, int fd) {
        if (!mgr->selection_source) {
          ::close(fd);
          return;
        }
        wl_data_source_send_send(mgr->selection_source, mime_type.c_str(), fd);
        ::close(fd);
        if (mgr->display)
          wl_display_flush_clients(mgr->display);
      });

  backend.set_clipboard_owner_changed_callback(
      [mgr](std::vector<std::string> mime_types) {
        // Only ever invoked for an owner that isn't us — any live Wayland
        // selection is now stale.
        if (mgr->selection_source) {
          wl_data_source_send_cancelled(mgr->selection_source);
          mgr->selection_source = nullptr;
        }
        mgr->selection_is_x11 = !mime_types.empty();
        mgr->x11_selection_mimes = std::move(mime_types);
        broadcast_selection_to_all(*mgr);
      });

  WW_LOG_INFO("[wayland] wl_data_device_manager registered (v3) — clipboard "
             "mirrored to X11 CLIPBOARD, drag-and-drop scoped to "
             "WWindows-managed clients");
  return {};
}

} // namespace ww
