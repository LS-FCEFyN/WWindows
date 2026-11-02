// zwp_linux_dmabuf_feedback_v1 — the format-table/device feedback object
// vended by zwp_linux_dmabuf_v1.get_default_feedback and
// get_surface_feedback (Dmabuf.cpp owns that vtable; see impl.hpp for how
// this fits with the other files in this directory).
//
// This backend has exactly one device and one static format set, so both
// requests send the same thing: one main_device event, a single tranche
// covering every format in SUPPORTED_DMABUF_FORMATS targeting that same
// device, then done. There is no per-surface preference to express and
// nothing changes at runtime, so feedback is sent once and never resent —
// both of which the protocol explicitly allows.
#include <wwindows/ports/imported_buffer.hpp>
#include "wwindows/adapters/wayland/globals/linux_dmabuf/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"
#include <wwindows/support/log.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <wwindows/adapters/wayland/protocols/linux-dmabuf-unstable-v1-server-protocol.h>
#include <sys/mman.h> // memfd_create, MFD_CLOEXEC
#include <sys/types.h> // dev_t
#include <unistd.h>
#include <wayland-server-core.h>

namespace ww {

namespace {

void feedback_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

const struct zwp_linux_dmabuf_feedback_v1_interface feedback_iface = {
    .destroy = feedback_destroy,
};

// The wire format for format_table's blob: a tightly packed array of
// {format, padding, modifier} triples, 16 bytes each, native endianness.
// tranche_formats then refers back into this table by index. See the
// protocol's format_table/tranche_formats event docs for the exact layout.
struct FormatTableEntry {
  uint32_t format;
  uint32_t padding;
  uint64_t modifier;
};
static_assert(sizeof(FormatTableEntry) == 16);

// Builds the format table in an anonymous, read-only-mappable file and
// returns an fd the caller owns (must ::close() it once sent — the receive
// side gets its own dup via SCM_RIGHTS at send time, same convention as
// InputTranslator's keymap fd). Returns -1 on failure.
int build_format_table_fd() {
  std::array<FormatTableEntry, SUPPORTED_DMABUF_FORMATS.size()> entries{};
  for (size_t i = 0; i < SUPPORTED_DMABUF_FORMATS.size(); ++i)
    entries[i] = FormatTableEntry{SUPPORTED_DMABUF_FORMATS[i], 0,
                                  DRM_FORMAT_MOD_LINEAR};

  int fd = ::memfd_create("wwindows-dmabuf-format-table", MFD_CLOEXEC);
  if (fd < 0) {
    WW_LOG_WARN("[wayland] memfd_create for dmabuf format table failed: {}",
               ::strerror(errno));
    return -1;
  }

  constexpr auto bytes =
      static_cast<ssize_t>(sizeof(FormatTableEntry) *
                           SUPPORTED_DMABUF_FORMATS.size());
  if (::write(fd, entries.data(), static_cast<size_t>(bytes)) != bytes) {
    WW_LOG_WARN("[wayland] short write building dmabuf format table: {}",
               ::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}

// Sends the full done-terminated feedback sequence for `feedback` and
// leaves it otherwise alone (the client destroys it explicitly, or it dies
// with its client).
void send_feedback(wl_resource *feedback, IWindowBackend &backend) {
  // register_linux_dmabuf() only advertises zwp_linux_dmabuf_v1 once
  // dmabuf_main_device() has a value, so a client can't normally reach
  // this handler without one. Stay defensive rather than assume that
  // invariant holds forever: finish the sequence so the client isn't left
  // waiting on `done` forever, just with nothing behind it.
  const std::optional<dev_t> main_device = backend.dmabuf_main_device();
  if (!main_device) {
    WW_LOG_WARN("[wayland] dmabuf feedback requested with no main device "
               "available — sending an empty sequence");
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);
    return;
  }

  const int table_fd = build_format_table_fd();
  if (table_fd < 0) {
    // Nothing useful to offer if we can't even describe the formats.
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);
    return;
  }
  constexpr auto table_size =
      static_cast<uint32_t>(sizeof(FormatTableEntry) *
                            SUPPORTED_DMABUF_FORMATS.size());
  zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, table_fd,
                                                 table_size);
  ::close(table_fd); // client received its own dup via SCM_RIGHTS

  wl_array device_arr;
  wl_array_init(&device_arr);
  if (void *p = wl_array_add(&device_arr, sizeof(dev_t)))
    std::memcpy(p, &*main_device, sizeof(dev_t));
  zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device_arr);

  // A single tranche covering every format we support, targeting the same
  // (only) device main_device just advertised — satisfies the protocol's
  // "must send at least one tranche with tranche_target_device equal to
  // main_device" requirement trivially, since it's the only tranche.
  zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback,
                                                          &device_arr);
  wl_array_release(&device_arr);

  zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);

  wl_array indices_arr;
  wl_array_init(&indices_arr);
  for (uint16_t i = 0; i < SUPPORTED_DMABUF_FORMATS.size(); ++i) {
    if (void *p = wl_array_add(&indices_arr, sizeof(uint16_t)))
      std::memcpy(p, &i, sizeof(uint16_t));
  }
  zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices_arr);
  wl_array_release(&indices_arr);

  zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
  zwp_linux_dmabuf_feedback_v1_send_done(feedback);
}

} // namespace

void dmabuf_get_default_feedback(wl_client *client, wl_resource *resource,
                                 uint32_t id) {
  auto *backend =
      static_cast<IWindowBackend *>(wl_resource_get_user_data(resource));
  wl_resource *feedback =
      wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                         wl_resource_get_version(resource), id);
  if (!feedback) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(feedback, &feedback_iface, nullptr, nullptr);

  if (backend)
    send_feedback(feedback, *backend);
}

void dmabuf_get_surface_feedback(wl_client *client, wl_resource *resource,
                                 uint32_t id, wl_resource * /*surface*/) {
  // No per-surface preference to express (see the file comment) — identical
  // to get_default_feedback bar the extra, unused surface argument the
  // protocol requires this request to accept.
  dmabuf_get_default_feedback(client, resource, id);
}

} // namespace ww
