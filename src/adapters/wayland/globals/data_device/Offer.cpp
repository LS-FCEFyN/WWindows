// wl_data_offer — see impl.hpp for how this fits with the other files in
// this directory.
#include <cerrno>
#include "wwindows/adapters/wayland/globals/data_device/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"

#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <cstdint>
#include <string>
#include <unistd.h>
#include <vector>

namespace ww {

namespace {
void offer_accept(wl_client *, wl_resource *resource, uint32_t /*serial*/,
                  const char *mime_type) {
  auto *offer = static_cast<OfferData *>(wl_resource_get_user_data(resource));
  if (!offer || !offer->is_drag || !offer->mgr || !offer->mgr->drag_source)
    return;
  wl_data_source_send_target(offer->mgr->drag_source, mime_type);
}

void offer_receive(wl_client *, wl_resource *resource, const char *mime_type,
                   int32_t fd) {
  auto *offer = static_cast<OfferData *>(wl_resource_get_user_data(resource));
  if (!offer) {
    ::close(fd);
    return;
  }
  const std::string mime = mime_type ? mime_type : "";

  if (offer->live_source) {
    // The two Wayland clients hand the bytes to each other directly — we
    // just forward the fd, exactly like wl_keyboard's keymap fd.
    wl_data_source_send_send(offer->live_source, mime.c_str(), fd);
    ::close(fd);
    return;
  }

  if (offer->from_x11 && offer->mgr && offer->mgr->backend) {
    offer->mgr->backend->request_clipboard_data(
        mime, [fd](std::vector<uint8_t> data) {
          size_t off = 0;
          while (off < data.size()) {
            const ssize_t n =
                ::write(fd, data.data() + off, data.size() - off);
            if (n < 0 && errno == EINTR)
              continue; // interrupted by a signal: retry, don't truncate
            if (n <= 0)
              break;
            off += static_cast<size_t>(n);
          }
          ::close(fd);
        });
    return;
  }

  ::close(fd);
}

void offer_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

void offer_finish(wl_client *, wl_resource *resource) {
  auto *offer = static_cast<OfferData *>(wl_resource_get_user_data(resource));
  if (!offer || !offer->is_drag)
    return;
  if (offer->live_source)
    wl_data_source_send_dnd_finished(offer->live_source);
}

void offer_set_actions(wl_client *, wl_resource *resource,
                       uint32_t dnd_actions, uint32_t preferred_action) {
  auto *offer = static_cast<OfferData *>(wl_resource_get_user_data(resource));
  if (!offer || !offer->is_drag || !offer->mgr)
    return;
  auto &mgr = *offer->mgr;

  const uint32_t intersect =
      mgr.drag_source_actions & (dnd_actions & kAllDndActions);
  uint32_t selected = 0;
  if (preferred_action & intersect)
    selected = preferred_action;
  else if (intersect &
          static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY))
    selected = static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  else if (intersect &
          static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE))
    selected = static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
  else if (intersect &
          static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK))
    selected = static_cast<uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

  if (wl_resource_get_version(resource) >= WL_DATA_OFFER_ACTION_SINCE_VERSION)
    wl_data_offer_send_action(resource, selected);
  if (mgr.drag_source &&
      wl_resource_get_version(mgr.drag_source) >=
          WL_DATA_SOURCE_ACTION_SINCE_VERSION)
    wl_data_source_send_action(mgr.drag_source, selected);
}

const struct wl_data_offer_interface data_offer_iface = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

void offer_source_destroyed(wl_listener *listener, void *) {
  OfferData *data = wl_container_of(listener, data, source_destroy_listener);
  // The source is going away: later receive/finish requests on this offer
  // must find no source rather than a dangling one. (libwayland unlinks the
  // listener itself once it has fired.)
  data->live_source = nullptr;
}

void offer_resource_destroy(wl_resource *r) {
  auto *data = static_cast<OfferData *>(wl_resource_get_user_data(r));
  if (data && data->live_source)
    wl_list_remove(&data->source_destroy_listener.link);
  // Same reasoning as device_resource_destroy: the manager tracks the
  // current drag offer, so a destroyed offer must not stay referenced.
  if (data && data->mgr && data->mgr->drag_current_offer == r)
    data->mgr->drag_current_offer = nullptr;
  delete data;
}
} // namespace

wl_resource *create_offer_for_device(DataDeviceManagerState &mgr,
                                     wl_resource *device,
                                     std::vector<std::string> mime_types,
                                     wl_resource *live_source, bool from_x11,
                                     bool is_drag) {
  wl_client *client = wl_resource_get_client(device);
  const int version = wl_resource_get_version(device);
  // id == 0: server-allocated id, the standard idiom for a compositor-
  // created object announced via an event (wl_data_device.data_offer)
  // rather than a client request.
  wl_resource *offer =
      wl_resource_create(client, &wl_data_offer_interface, version, 0);
  if (!offer)
    return nullptr;

  auto *data = new OfferData();
  data->mgr = &mgr;
  data->live_source = live_source;
  if (live_source) {
    data->source_destroy_listener.notify = offer_source_destroyed;
    wl_resource_add_destroy_listener(live_source, &data->source_destroy_listener);
  }
  data->from_x11 = from_x11;
  data->is_drag = is_drag;
  data->mime_types = std::move(mime_types);
  wl_resource_set_implementation(offer, &data_offer_iface, data,
                                 offer_resource_destroy);

  return offer;
}

void emit_offer_events(DataDeviceManagerState &mgr, wl_resource *offer) {
  auto *data = static_cast<OfferData *>(wl_resource_get_user_data(offer));
  if (!data)
    return;
  const int version = wl_resource_get_version(offer);

  WW_LOG_DEBUG("[wayland-clipboard] emit_offer_events: from_x11={}, "
              "is_drag={}, mime_count={}", data->from_x11, data->is_drag,
              data->mime_types.size());
  for (const auto &mime : data->mime_types) {
    WW_LOG_DEBUG("[wayland-clipboard]   offer mime: {}", mime);
    wl_data_offer_send_offer(offer, mime.c_str());
  }
  if (data->is_drag &&
      version >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
    wl_data_offer_send_source_actions(offer, mgr.drag_source_actions);
}

} // namespace ww
