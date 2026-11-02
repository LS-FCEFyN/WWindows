// wl_data_device.set_selection and the clipboard it drives. Mirrored to the
// X11 CLIPBOARD selection via IWindowBackend's clipboard methods (see
// X11Backend_clipboard.cpp) so copy/paste works between Wayland clients
// *and* with native X11 applications. Three possible states for "what's on
// the clipboard":
//   - nothing (selection_source == nullptr && !selection_is_x11)
//   - a live Wayland client's wl_data_source (selection_source set)
//   - X11-native content, tracked only as a canonical mime list
//     (selection_is_x11 == true) — the actual bytes are fetched on demand
//     via backend->request_clipboard_data() when a Wayland client receives.
//
// See impl.hpp for how this fits with the other files in this directory,
// and Drag.cpp for the other half of wl_data_device (start_drag /
// drag-and-drop).
#include "wwindows/adapters/wayland/globals/data_device/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"

#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ww {

void device_set_selection(wl_client *, wl_resource *resource,
                          wl_resource *source, uint32_t /*serial*/) {
  auto *mgr =
      static_cast<DataDeviceManagerState *>(wl_resource_get_user_data(resource));
  if (!mgr)
    return;

  if (mgr->selection_source && mgr->selection_source != source)
    wl_data_source_send_cancelled(mgr->selection_source);

  mgr->selection_source = source;
  mgr->selection_is_x11 = false;

  if (source) {
    auto *src = static_cast<SourceData *>(wl_resource_get_user_data(source));
    if (src && mgr->backend)
      mgr->backend->set_clipboard_owner(src->mime_types);
  } else if (mgr->backend) {
    mgr->backend->set_clipboard_owner({});
  }

  broadcast_selection_to_all(*mgr);
}

void send_selection_offer_to_device(DataDeviceManagerState &mgr,
                                    wl_resource *device) {
  if (!mgr.display)
    return;

  if (!mgr.selection_source && !mgr.selection_is_x11) {
    WW_LOG_DEBUG("[wayland-clipboard] send_selection_offer_to_device: no "
                "selection (neither wayland source nor x11) — sending null");
    wl_data_device_send_selection(device, nullptr);
    wl_display_flush_clients(mgr.display);
    return;
  }

  std::vector<std::string> mimes;
  wl_resource *live_source = nullptr;
  bool from_x11 = false;
  if (mgr.selection_is_x11) {
    mimes = mgr.x11_selection_mimes;
    from_x11 = true;
  } else {
    auto *src =
        static_cast<SourceData *>(wl_resource_get_user_data(mgr.selection_source));
    if (!src)
      return;
    mimes = src->mime_types;
    live_source = mgr.selection_source;
  }

  WW_LOG_DEBUG("[wayland-clipboard] send_selection_offer_to_device: "
              "from_x11={}, mime_count={}", from_x11, mimes.size());

  wl_resource *offer = create_offer_for_device(mgr, device, std::move(mimes),
                                               live_source, from_x11, false);
  if (!offer)
    return;
  // Order matters: data_offer (announces the new object to the client)
  // must go out before any events referencing it, so offer() events are
  // emitted only after this call.
  wl_data_device_send_data_offer(device, offer);
  emit_offer_events(mgr, offer);
  wl_data_device_send_selection(device, offer);
  wl_display_flush_clients(mgr.display);
}

void broadcast_selection_to_all(DataDeviceManagerState &mgr) {
  WW_LOG_DEBUG("[wayland-clipboard] broadcast_selection_to_all: {} device(s), "
              "selection_is_x11={}, x11_mimes={}",
              mgr.devices.size(), mgr.selection_is_x11,
              mgr.x11_selection_mimes.size());
  // A fresh wl_data_offer per device, same as send_selection_offer_to_device
  // — offers aren't reusable across clients/devices once sent.
  for (wl_resource *device : mgr.devices)
    send_selection_offer_to_device(mgr, device);
}

} // namespace ww
