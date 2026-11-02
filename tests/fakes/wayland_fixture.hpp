#pragma once

// In-process Wayland server fixture.
//
// SurfaceManager is written against wl_resource, so testing it honestly
// requires real ones -- a hand-rolled stub pointer would not exercise
// wl_resource_add_destroy_listener, wl_resource_destroy, or the destroy-
// listener callbacks that a large share of the surface lifecycle logic
// exists to handle.
//
// libwayland-server supports this directly: create a wl_display, attach a
// wl_client over one end of a socketpair, and create resources on it. No
// compositor socket, no real client process, no X server. The other end of
// the socketpair is never read; protocol events just accumulate in the
// kernel buffer and are discarded on teardown.

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wwindows/adapters/wayland/protocols/viewporter-server-protocol.h>
#include <wwindows/adapters/wayland/protocols/foreign-toplevel-list-server-protocol.h>
#include <wwindows/adapters/wayland/protocols/image-capture-source-server-protocol.h>
#include <wwindows/adapters/wayland/protocols/image-copy-capture-server-protocol.h>
#include <wwindows/adapters/wayland/globals/viewporter.hpp>
#include <wwindows/adapters/wayland/globals/foreign_toplevel_list.hpp>
#include <wwindows/adapters/wayland/globals/image_capture_source.hpp>
#include <wwindows/adapters/wayland/globals/image_copy_capture.hpp>

#include <wwindows/ports/imported_buffer.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace ww_test {

class WaylandFixture {
public:
  WaylandFixture() {
    display_ = wl_display_create();
    if (!display_)
      throw std::runtime_error("wl_display_create failed");

    int sv[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
      throw std::runtime_error("socketpair failed");
    client_fd_ = sv[1];

    // wl_client takes ownership of sv[0].
    client_ = wl_client_create(display_, sv[0]);
    if (!client_)
      throw std::runtime_error("wl_client_create failed");
  }

  ~WaylandFixture() {
    // Mirrors the production teardown in WaylandServer.cpp: reap clients
    // first (wl_display_destroy alone leaks every resource they still own,
    // and never fires their destroy listeners), then destroy the display.
    wl_display_destroy_clients(display_);
    wl_display_destroy(display_);
    if (client_fd_ >= 0)
      ::close(client_fd_);
    for (int fd : received_fds_)
      ::close(fd);
  }

  WaylandFixture(const WaylandFixture &) = delete;
  WaylandFixture &operator=(const WaylandFixture &) = delete;

  [[nodiscard]] wl_display *display() const { return display_; }
  [[nodiscard]] wl_client *client() const { return client_; }

  // id 0 asks libwayland to allocate a server-side id.
  wl_resource *make(const wl_interface *iface, int version = 1) {
    wl_resource *r = wl_resource_create(client_, iface, version, 0);
    if (!r)
      throw std::runtime_error("wl_resource_create failed");
    return r;
  }

  wl_resource *surface() { return make(&wl_surface_interface, 4); }
  wl_resource *xdg_surface() { return make(&xdg_surface_interface, 1); }
  // Version 2 is where the TILED_* states first exist; the default of 1 is
  // the oldest client, which must never be sent them.
  wl_resource *xdg_toplevel(int version = 1) {
    return make(&xdg_toplevel_interface, version);
  }
  wl_resource *xdg_popup() { return make(&xdg_popup_interface, 1); }
  wl_resource *subsurface() { return make(&wl_subsurface_interface, 1); }
  wl_resource *callback() { return make(&wl_callback_interface, 1); }
  wl_resource *buffer() { return make(&wl_buffer_interface, 1); }
  // Version 5 so wl_pointer.frame is in range (WL_POINTER_FRAME_SINCE_VERSION).
  wl_resource *pointer(int version = 5) {
    return make(&wl_pointer_interface, version);
  }
  wl_resource *keyboard(int version = 5) {
    return make(&wl_keyboard_interface, version);
  }

  // Register the wp_viewporter global on this fixture's display.
  // Returns the opaque global pointer (for cleanup if needed).
  void *register_viewporter() {
    auto r = ww::register_viewporter(display_);
    if (!r)
      throw std::runtime_error("register_viewporter failed: " + r.error().message);
    return *r;
  }

  // Register the ext_foreign_toplevel_list_v1 global on this fixture's display.
  void *register_foreign_toplevel_list(ww::SurfaceManager &surface_manager) {
    auto r = ww::foreign_toplevel_list::register_foreign_toplevel_list(display_, surface_manager);
    if (!r)
      throw std::runtime_error("register_foreign_toplevel_list failed: " + r.error().message);
    return *r;
  }

  // Register the ext_image_capture_source_v1 global on this fixture's display.
  void *register_image_capture_source(ww::SurfaceManager &surface_manager) {
    auto r = ww::image_capture_source::register_image_capture_source(display_, surface_manager);
    if (!r)
      throw std::runtime_error("register_image_capture_source failed: " + r.error().message);
    return *r;
  }

  // Register the ext_image_copy_capture_v1 global on this fixture's display.
  void *register_image_copy_capture(ww::SurfaceManager &surface_manager) {
    auto r = ww::image_copy_capture::register_image_copy_capture(display_, surface_manager);
    if (!r)
      throw std::runtime_error("register_image_copy_capture failed: " + r.error().message);
    return *r;
  }

  // A wl_buffer carrying ImportedBuffer user data -- i.e. what the commit
  // path sees for a dmabuf-backed buffer. wl_shm_buffer_get() returns null
  // for it (it was never created through a wl_shm pool), which is exactly
  // how SurfaceManager distinguishes the two presentation paths.
  //
  // fd is -1 so commit() exercises sizing without attempting an import;
  // tests that want the import path set it themselves. Passing a non-zero
  // `pixmap` stands in for a buffer already imported by an earlier commit,
  // which is what puts commit() on its present-immediately path — the one
  // that actually reaches the backend with a presentation rectangle.
  wl_resource *dmabuf_buffer(uint16_t width, uint16_t height,
                             uint32_t pixmap = 0) {
    wl_resource *r = buffer();
    auto owned = std::make_unique<ww::ImportedBuffer>();
    owned->width = width;
    owned->height = height;
    owned->fd = -1;
    owned->pixmap = pixmap;
    wl_resource_set_user_data(r, owned.get());
    imported_.push_back(std::move(owned));
    return r;
  }

  // The same lightweight resource shape for the multi-plane NV12 fallback.
  // Core tests use fake plane fds: they assert which backend presentation path
  // is selected, not the X11 backend's mmap/conversion implementation.
  wl_resource *nv12_buffer(uint16_t width, uint16_t height) {
    wl_resource *r = buffer();
    auto owned = std::make_unique<ww::ImportedBuffer>();
    owned->format = ww::ImportedBufferFormat::Nv12;
    owned->plane_count = 2;
    owned->width = width;
    owned->height = height;
    owned->planes[0] = ww::ImportedDmabufPlane{
        -1, 0, static_cast<uint32_t>(width), 0};
    owned->planes[1] = ww::ImportedDmabufPlane{
        -1, 0, ((static_cast<uint32_t>(width) + 1u) / 2u) * 2u, 0};
    wl_resource_set_user_data(r, owned.get());
    imported_.push_back(std::move(owned));
    return r;
  }

  // --- event readback -------------------------------------------------------
  // Asserting on the bytes the compositor actually put on the wire is the
  // only way to verify protocol behavior from outside: wl_pointer.enter,
  // wl_keyboard.modifiers and xdg_surface.configure have no observable
  // effect on any API this process can see.
  struct Event {
    uint32_t object_id{0};
    uint16_t opcode{0};
    std::vector<uint8_t> args; // everything after the 8-byte header
  };

  // Every event sent to `resource` so far, in order.
  std::vector<Event> events_for(wl_resource *resource) {
    const uint32_t target = wl_resource_get_id(resource);
    std::vector<Event> out;
    for (const Event &e : all_events())
      if (e.object_id == target)
        out.push_back(e);
    return out;
  }

  // Opcodes of every event sent to `resource`, which is usually all a test
  // needs ("did it get an enter followed by a frame?").
  std::vector<uint16_t> opcodes_for(wl_resource *resource) {
    std::vector<uint16_t> out;
    for (const Event &e : events_for(resource))
      out.push_back(e.opcode);
    return out;
  }

  [[nodiscard]] std::size_t count_events(wl_resource *resource,
                                         uint16_t opcode) {
    std::size_t n = 0;
    for (uint16_t op : opcodes_for(resource))
      if (op == opcode)
        ++n;
    return n;
  }

  // Read a 32-bit argument out of an event payload by index.
  static uint32_t arg_u32(const Event &e, std::size_t index) {
    uint32_t v{};
    if ((index + 1) * 4 <= e.args.size())
      std::memcpy(&v, e.args.data() + index * 4, 4);
    return v;
  }
  static int32_t arg_i32(const Event &e, std::size_t index) {
    return static_cast<int32_t>(arg_u32(e, index));
  }

  // Descriptors the compositor passed as SCM_RIGHTS ancillary data, in
  // arrival order. In practice this is the wl_keyboard.keymap fd -- the only
  // event whose real payload travels out of band, and therefore the only way
  // to check what keymap the compositor actually handed the client.
  //
  // Call after something that forces a read of the stream (events_for,
  // opcodes_for, ...); the fds are collected as a side effect of draining.
  // Ownership stays with the fixture, which closes them on teardown.
  const std::vector<int> &received_fds() {
    all_events(); // force a drain so anything pending is collected
    return received_fds_;
  }

  // The states carried by the last xdg_toplevel.configure sent to
  // `toplevel` (opcode 0: width int, height int, then a wl_array of
  // uint32 whose wire form is a byte count followed by the payload).
  std::vector<uint32_t> last_toplevel_states(wl_resource *toplevel) {
    std::vector<uint32_t> out;
    for (const Event &e : events_for(toplevel)) {
      if (e.opcode != 0 || e.args.size() < 12)
        continue;
      out.clear();
      const uint32_t bytes = arg_u32(e, 2);
      for (uint32_t i = 0; i + 4 <= bytes && 12u + i + 4u <= e.args.size();
           i += 4) {
        uint32_t v{};
        std::memcpy(&v, e.args.data() + 12 + i, 4);
        out.push_back(v);
      }
    }
    return out;
  }

  // The width and height of the last xdg_toplevel.configure sent to
  // `toplevel`; {0, 0} if there was none.
  std::pair<int32_t, int32_t> last_toplevel_size(wl_resource *toplevel) {
    std::pair<int32_t, int32_t> out{0, 0};
    for (const Event &e : events_for(toplevel)) {
      if (e.opcode != 0 || e.args.size() < 12)
        continue;
      out = {static_cast<int32_t>(arg_u32(e, 0)),
             static_cast<int32_t>(arg_u32(e, 1))};
    }
    return out;
  }

  // Serials from every xdg_surface.configure (opcode 0, single u32 arg).
  std::vector<uint32_t> configure_serials(wl_resource *xdg_surface) {
    std::vector<uint32_t> serials;
    for (const Event &e : events_for(xdg_surface))
      if (e.opcode == 0 && e.args.size() >= 4)
        serials.push_back(arg_u32(e, 0));
    return serials;
  }

private:
  // Parses the accumulated wire stream. Reading the socket consumes it, so
  // bytes are appended to a persistent buffer -- otherwise querying a second
  // resource would parse an already-drained stream and see nothing at all.
  const std::vector<Event> &all_events() {
    wl_client_flush(client_);
    const std::vector<uint8_t> chunk = drain_client_socket();
    seen_.insert(seen_.end(), chunk.begin(), chunk.end());

    // Wire format: object id (u32), then (opcode | size << 16) (u32), then
    // size - 8 bytes of arguments.
    while (parsed_ + 8 <= seen_.size()) {
      uint32_t id{};
      uint32_t second{};
      std::memcpy(&id, seen_.data() + parsed_, 4);
      std::memcpy(&second, seen_.data() + parsed_ + 4, 4);
      const auto opcode = static_cast<uint16_t>(second & 0xffffu);
      const std::size_t size = second >> 16;
      if (size < 8 || parsed_ + size > seen_.size())
        break;
      Event e;
      e.object_id = id;
      e.opcode = opcode;
      e.args.assign(seen_.begin() + static_cast<long>(parsed_ + 8),
                    seen_.begin() + static_cast<long>(parsed_ + size));
      events_.push_back(std::move(e));
      parsed_ += size;
    }
    return events_;
  }

  std::vector<uint8_t> drain_client_socket() {
    std::vector<uint8_t> out;
    uint8_t buf[4096];
    // Keep reading until the socket is actually empty (EAGAIN). A short read
    // does NOT mean the stream is drained: libwayland attaches file
    // descriptors (wl_keyboard.keymap sends one) as SCM_RIGHTS ancillary
    // data, and a Unix-domain SOCK_STREAM recv() stops at that message
    // boundary. Treating a short read as end-of-stream silently truncated
    // every event queued after the keymap.
    //
    // recvmsg rather than recv so those descriptors are *received* rather
    // than dropped. A plain recv() discards the ancillary data and the
    // kernel closes the descriptor, which made the keymap -- the one event
    // whose payload is the fd -- impossible to inspect from a test.
    for (;;) {
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 8)]{};
      iovec iov{buf, sizeof buf};
      msghdr msg{};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = control;
      msg.msg_controllen = sizeof control;

      const ssize_t n = ::recvmsg(client_fd_, &msg, MSG_DONTWAIT);
      if (n > 0) {
        out.insert(out.end(), buf, buf + n);
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
          if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
          const std::size_t count =
              (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
          for (std::size_t i = 0; i < count; ++i) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
            if (fd >= 0)
              received_fds_.push_back(fd);
          }
        }
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      break; // 0 == peer closed, -1 with EAGAIN == drained
    }
    return out;
  }

  std::vector<int> received_fds_; // SCM_RIGHTS descriptors, owned here
  std::vector<uint8_t> seen_;   // every event byte read back so far
  std::vector<Event> events_;   // decoded from seen_
  std::size_t parsed_{0};       // how far into seen_ decoding has reached
  std::vector<std::unique_ptr<ww::ImportedBuffer>> imported_;
  wl_display *display_{nullptr};
  wl_client *client_{nullptr};
  int client_fd_{-1};
};

} // namespace ww_test
