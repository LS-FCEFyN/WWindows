#pragma once

// Shared between the files in sources/wayland/data_device/ that together
// implement register_data_device_manager() (see ../DataDevice.hpp, one
// directory up, for the public entry point). wl_data_offer/wl_data_source/
// wl_data_device_manager are one file each (the natural per-interface
// split); wl_data_device itself splits again along its two independent
// features — clipboard and drag-and-drop — which is why there's no single
// "Device.cpp" containing all of wl_data_device's logic:
//
//   data_device/Offer.cpp     — wl_data_offer
//   data_device/Source.cpp    — wl_data_source
//   data_device/Device.cpp    — wl_data_device object itself (vtable +
//                                lifecycle only; the two request bodies
//                                live in Selection.cpp / Drag.cpp below)
//   data_device/Selection.cpp — wl_data_device.set_selection and the
//                                clipboard mirroring it drives
//   data_device/Drag.cpp      — wl_data_device.start_drag and the
//                                drag-and-drop state machine it drives
//   data_device/Manager.cpp   — wl_data_device_manager,
//                                register_data_device_manager()
//
// Nothing here is part of WWindows' public API.

#include <wayland-server-protocol.h>

#include <cstdint>
#include <string>
#include <vector>

struct wl_client;
struct wl_display;
struct wl_resource;

namespace ww {

class SurfaceManager;
class InputTranslator;
class IWindowBackend;

constexpr uint32_t kAllDndActions =
    static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) |
    static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) |
    static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

struct DataDeviceManagerState {
  wl_display *display{nullptr};
  SurfaceManager *surface_manager{nullptr};
  InputTranslator *input_translator{nullptr};
  IWindowBackend *backend{nullptr};

  std::vector<wl_resource *> devices; // every live wl_data_device

  // --- Selection (clipboard), driven by Selection.cpp ---
  wl_resource *selection_source{nullptr}; // wl_data_source*, live Wayland
  bool selection_is_x11{false};
  std::vector<std::string> x11_selection_mimes;

  // --- Drag-and-drop, driven by Drag.cpp ---
  bool drag_active{false};
  wl_resource *drag_source{nullptr}; // wl_data_source*, may be null
  wl_client *drag_origin_client{nullptr};
  wl_resource *drag_current_target_surface{nullptr};
  wl_resource *drag_current_target_device{nullptr};
  wl_resource *drag_current_offer{nullptr};
  uint32_t drag_source_actions{
      static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)};

  // Ties this object's lifetime to the wl_display it was registered on.
  //
  // It is owned by the wl_data_device_manager global, and libwayland has no
  // destructor hook for a global's user data -- so without this the object
  // is simply never freed. In the runtime that is invisible (one instance,
  // reclaimed at process exit), but it is the same shutdown-leak shape as
  // the WlDisplayDeleter fix, and it makes any test that registers the
  // manager leak under ASan.
  wl_listener display_destroy{};
};

struct SourceData {
  DataDeviceManagerState *mgr{nullptr};
  std::vector<std::string> mime_types;
  uint32_t dnd_actions{
      static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)};
  bool actions_set{false};
};

struct OfferData {
  DataDeviceManagerState *mgr{nullptr};
  wl_resource *live_source{nullptr}; // wl_data_source*, forward receive() to it
  // Fires when live_source is destroyed, so this offer never forwards to a
  // dead resource (see Offer.cpp). Linked only while live_source is set.
  wl_listener source_destroy_listener{};
  bool from_x11{false};              // fetch via backend on receive() instead
  bool is_drag{false};
  std::vector<std::string> mime_types;
};

// -------------------------------------------------------------------------
// data_device/Offer.cpp
// -------------------------------------------------------------------------
// Only creates and registers the wl_data_offer resource — does NOT emit any
// events on it yet. Per wl_data_device.data_offer's documented behavior,
// "Immediately following the data_device.data_offer event, the new
// data_offer object will send out data_offer.offer events" — i.e. the
// client only learns the new object's id/interface from the data_offer (or
// enter) event that announces it, and events sent on the new resource
// *before* that announcing event reaches the client are for an object the
// client doesn't know about yet and get silently discarded on the wire.
// So callers must: create the offer, send wl_data_device.data_offer/enter
// referencing it, and only *then* call emit_offer_events below.
wl_resource *create_offer_for_device(DataDeviceManagerState &mgr,
                                     wl_resource *device,
                                     std::vector<std::string> mime_types,
                                     wl_resource *live_source, bool from_x11,
                                     bool is_drag);
// Emits data_offer.offer() (and, for drags, source_actions()) on `offer`.
// Must only be called *after* the caller has already sent the
// wl_data_device event (data_offer/enter) that announces `offer` to the
// client — see create_offer_for_device's comment for why the order matters.
void emit_offer_events(DataDeviceManagerState &mgr, wl_resource *offer);

// -------------------------------------------------------------------------
// data_device/Source.cpp
// -------------------------------------------------------------------------
extern const struct wl_data_source_interface data_source_iface;
void source_resource_destroy(wl_resource *r);

// -------------------------------------------------------------------------
// data_device/Device.cpp
// -------------------------------------------------------------------------
extern const struct wl_data_device_interface data_device_iface;
void device_resource_destroy(wl_resource *r);

// -------------------------------------------------------------------------
// data_device/Selection.cpp
// -------------------------------------------------------------------------
void device_set_selection(wl_client *client, wl_resource *resource,
                          wl_resource *source, uint32_t serial);
void send_selection_offer_to_device(DataDeviceManagerState &mgr,
                                    wl_resource *device);
void broadcast_selection_to_all(DataDeviceManagerState &mgr);

// -------------------------------------------------------------------------
// data_device/Drag.cpp
// -------------------------------------------------------------------------
void device_start_drag(wl_client *client, wl_resource *resource,
                       wl_resource *source, wl_resource *origin,
                       wl_resource *icon, uint32_t serial);
void cancel_drag(DataDeviceManagerState &mgr);

} // namespace ww
