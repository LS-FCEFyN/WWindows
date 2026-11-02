// wl_data_device.start_drag and the drag-and-drop state machine it drives.
// Scoped to drags between WWindows-managed Wayland clients. Hit-testing
// reuses InputTranslator's ordinary per-window Enter/Leave/Motion dispatch
// via a redirect (see InputTranslator::begin_drag) rather than an explicit
// X11 pointer grab, and the drag icon surface is accepted (so well-behaved
// clients don't get a protocol error) but not rendered — there's no XDND
// bridge, so this never interoperates with a drag to/from a native X11
// window anyway, and skipping icon rendering avoids having to extend
// SurfaceManager's role system for a purely cosmetic feature.
//
// See impl.hpp for how this fits with the other files in this directory,
// and Selection.cpp for the other half of wl_data_device (set_selection /
// clipboard).
#include "wwindows/adapters/wayland/globals/data_device/detail/impl.hpp"
#include "wwindows/input/input_translator.hpp"

#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace ww {

namespace {
wl_resource *find_device_for_client(DataDeviceManagerState &mgr,
                                    wl_client *client) {
  for (wl_resource *d : mgr.devices) {
    if (wl_resource_get_client(d) == client)
      return d;
  }
  return nullptr;
}

void drag_enter_surface(DataDeviceManagerState &mgr, wl_resource *surf,
                        int16_t x, int16_t y) {
  mgr.drag_current_target_surface = surf;
  mgr.drag_current_target_device = nullptr;
  mgr.drag_current_offer = nullptr;
  if (!surf || !mgr.display)
    return;

  wl_client *target_client = wl_resource_get_client(surf);
  // A source-less drag ("the client is expected to handle the data
  // passing internally", per wl_data_device.start_drag) only ever gets
  // enter/motion/leave sent to the client that started it.
  if (!mgr.drag_source && target_client != mgr.drag_origin_client)
    return;

  wl_resource *device = find_device_for_client(mgr, target_client);
  if (!device)
    return;

  std::vector<std::string> mimes;
  if (mgr.drag_source) {
    if (auto *src =
            static_cast<SourceData *>(wl_resource_get_user_data(mgr.drag_source)))
      mimes = src->mime_types;
  }
  wl_resource *offer = create_offer_for_device(mgr, device, std::move(mimes),
                                               mgr.drag_source, false, true);
  if (!offer)
    return;
  mgr.drag_current_target_device = device;
  mgr.drag_current_offer = offer;

  const uint32_t serial = wl_display_next_serial(mgr.display);
  // Same ordering requirement as the selection path: data_offer first, then
  // offer()/source_actions(), then the event (here, enter) referencing it.
  wl_data_device_send_data_offer(device, offer);
  emit_offer_events(mgr, offer);
  wl_data_device_send_enter(device, serial, surf, wl_fixed_from_int(x),
                            wl_fixed_from_int(y), offer);
  wl_display_flush_clients(mgr.display);
}

void drag_motion(DataDeviceManagerState &mgr, int16_t x, int16_t y,
                 uint32_t time) {
  if (!mgr.drag_current_target_device || !mgr.display)
    return;
  wl_data_device_send_motion(mgr.drag_current_target_device, time,
                             wl_fixed_from_int(x), wl_fixed_from_int(y));
  wl_display_flush_clients(mgr.display);
}

void drag_leave(DataDeviceManagerState &mgr) {
  if (mgr.drag_current_target_device && mgr.display) {
    wl_data_device_send_leave(mgr.drag_current_target_device);
    wl_display_flush_clients(mgr.display);
  }
  mgr.drag_current_target_surface = nullptr;
  mgr.drag_current_target_device = nullptr;
  mgr.drag_current_offer = nullptr;
}

void drag_release(DataDeviceManagerState &mgr) {
  wl_resource *device = mgr.drag_current_target_device;
  wl_resource *source = mgr.drag_source;
  if (device) {
    wl_data_device_send_drop(device);
    if (source)
      wl_data_source_send_dnd_drop_performed(source);
  } else if (source) {
    wl_data_source_send_cancelled(source);
  }
  if (mgr.display)
    wl_display_flush_clients(mgr.display);

  if (mgr.input_translator)
    mgr.input_translator->end_drag();
  mgr.drag_active = false;
  mgr.drag_source = nullptr;
  mgr.drag_origin_client = nullptr;
  mgr.drag_current_target_surface = nullptr;
  mgr.drag_current_target_device = nullptr;
  mgr.drag_current_offer = nullptr;
}
} // namespace

void cancel_drag(DataDeviceManagerState &mgr) {
  if (mgr.drag_current_target_device) {
    wl_data_device_send_leave(mgr.drag_current_target_device);
    if (mgr.display)
      wl_display_flush_clients(mgr.display);
  }
  if (mgr.input_translator)
    mgr.input_translator->end_drag();
  mgr.drag_active = false;
  mgr.drag_source = nullptr;
  mgr.drag_origin_client = nullptr;
  mgr.drag_current_target_surface = nullptr;
  mgr.drag_current_target_device = nullptr;
  mgr.drag_current_offer = nullptr;
}

void device_start_drag(wl_client *client, wl_resource *resource,
                       wl_resource *source, wl_resource *origin,
                       wl_resource * /*icon*/, uint32_t /*serial*/) {
  auto *mgr =
      static_cast<DataDeviceManagerState *>(wl_resource_get_user_data(resource));
  if (!mgr || !origin)
    return;
  if (mgr->drag_active) {
    WW_LOG_WARN("[wayland] start_drag while a drag is already active — "
                "ignored");
    return;
  }

  auto *src_data =
      source ? static_cast<SourceData *>(wl_resource_get_user_data(source))
             : nullptr;

  mgr->drag_active = true;
  mgr->drag_source = source;
  mgr->drag_origin_client = client;
  mgr->drag_current_target_surface = nullptr;
  mgr->drag_current_target_device = nullptr;
  mgr->drag_current_offer = nullptr;
  mgr->drag_source_actions =
      (src_data && src_data->actions_set)
          ? src_data->dnd_actions
          : static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

  InputTranslator::DragCallbacks cbs;
  cbs.on_enter = [mgr](wl_resource *surf, int16_t x, int16_t y) {
    drag_enter_surface(*mgr, surf, x, y);
  };
  cbs.on_motion = [mgr](int16_t x, int16_t y, uint32_t time) {
    drag_motion(*mgr, x, y, time);
  };
  cbs.on_leave = [mgr]() { drag_leave(*mgr); };
  cbs.on_release = [mgr]() { drag_release(*mgr); };
  if (mgr->input_translator)
    mgr->input_translator->begin_drag(std::move(cbs));

  WW_LOG_DEBUG("[wayland] drag-and-drop started (source={})",
              static_cast<void *>(source));
}

} // namespace ww
