#pragma once

// In-process Wayland *client* fixture.
//
// tests/fakes/wayland_fixture.hpp fabricates wl_resource objects directly on
// the server side and reads back the event stream. That is the right tool for
// SurfaceManager and InputTranslator, which are handed resources and asked to
// do something with them -- but it cannot reach the protocol globals at all,
// because the thing those globals do is *unpack incoming requests*. A test
// that constructs the resource itself has already skipped the code under
// test.
//
// So this fixture runs both halves for real: an in-process wl_display with a
// wl_client on one end of a socketpair, and a genuine libwayland-client
// connection on the other. Requests go out through the generated client
// stubs, are marshalled over a real socket, and arrive at the real
// request handlers. Nothing about the protocol path is simulated.
//
// A HAZARD worth stating plainly: libwayland-server and libwayland-client
// both declare `struct wl_display`, as distinct opaque types that happen to
// share a tag name. In a translation unit that includes both -- which this
// one must -- the compiler sees a single type and will silently accept a
// client display passed to a server function, or the reverse. Neither side
// type-checks the mistake for you. Hence the naming discipline below:
// `server_display_` and `client_display_` are never abbreviated, and the
// accessors keep the distinction in their names.
//
// Both ends live in one thread, so nothing here may block. wl_display_roundtrip
// would deadlock instantly: it waits for a sync reply that only this same
// thread could produce. pump() is the non-blocking equivalent -- flush one
// side, let the other dispatch, repeat -- and every test drives the exchange
// with it explicitly.

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include <cstdint>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace ww_test {

class WaylandClientFixture {
public:
  WaylandClientFixture() {
    server_display_ = wl_display_create();
    if (!server_display_)
      throw std::runtime_error("wl_display_create failed");
    server_loop_ = wl_display_get_event_loop(server_display_);

    int sv[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
      throw std::runtime_error("socketpair failed");

    // wl_client_create takes ownership of sv[0]; wl_display_connect_to_fd
    // takes ownership of sv[1].
    server_client_ = wl_client_create(server_display_, sv[0]);
    if (!server_client_) {
      ::close(sv[0]);
      ::close(sv[1]);
      throw std::runtime_error("wl_client_create failed");
    }

    client_display_ = wl_display_connect_to_fd(sv[1]);
    if (!client_display_)
      throw std::runtime_error("wl_display_connect_to_fd failed");
  }

  ~WaylandClientFixture() {
    if (registry_)
      wl_registry_destroy(registry_);
    if (client_display_)
      wl_display_disconnect(client_display_);
    if (server_display_) {
      // Same ordering as production teardown: reap clients before the
      // display, or every resource they still own leaks and their destroy
      // listeners never fire.
      wl_display_destroy_clients(server_display_);
      wl_display_destroy(server_display_);
    }
  }

  WaylandClientFixture(const WaylandClientFixture &) = delete;
  WaylandClientFixture &operator=(const WaylandClientFixture &) = delete;

  // Server-side display: the one to hand register_core_globals() and friends.
  [[nodiscard]] wl_display *server_display() const { return server_display_; }
  [[nodiscard]] wl_client *server_client() const { return server_client_; }
  // Client-side display: the one to create proxies from.
  [[nodiscard]] wl_display *client_display() const { return client_display_; }

  // Move messages in both directions until things settle.
  //
  // Several rounds because a single exchange is rarely enough: binding a
  // global takes one (request out, events back), and anything the compositor
  // sends in response to a bound object takes another. Six is empirically
  // more than any exchange in this suite needs and costs microseconds.
  void pump(int rounds = 6) {
    for (int i = 0; i < rounds; ++i) {
      // client -> server
      wl_display_flush(client_display_);
      wl_event_loop_dispatch(server_loop_, 0);
      // server -> client
      wl_display_flush_clients(server_display_);
      read_client_events();
    }
  }

  // --- registry -------------------------------------------------------------
  struct GlobalInfo {
    uint32_t name{0};
    std::string interface;
    uint32_t version{0};
  };

  // Everything the compositor advertised, as the client saw it.
  const std::vector<GlobalInfo> &globals() {
    if (!registry_) {
      registry_ = wl_display_get_registry(client_display_);
      static const wl_registry_listener listener = {
          &WaylandClientFixture::on_global,
          &WaylandClientFixture::on_global_remove};
      wl_registry_add_listener(registry_, &listener, this);
      pump();
    }
    return globals_;
  }

  [[nodiscard]] const GlobalInfo *find_global(const char *interface) {
    for (const GlobalInfo &g : globals())
      if (g.interface == interface)
        return &g;
    return nullptr;
  }

  [[nodiscard]] bool has_global(const char *interface) {
    return find_global(interface) != nullptr;
  }

  // Bind a global at `version`, or at whatever the compositor advertised if
  // that is lower. Returns nullptr when the global was never advertised, so
  // a test can assert on absence without crashing.
  void *bind(const char *interface, const wl_interface *iface,
             uint32_t version) {
    void *proxy = bind_deferred(interface, iface, version);
    if (proxy)
      pump();
    return proxy;
  }

  // Bind without pumping, so a listener can be attached before any exchange
  // happens.
  //
  // This matters more than it looks. Several globals send events from inside
  // their bind handler -- wl_output emits geometry/mode/done, wl_seat emits
  // capabilities -- so those events are already on the wire by the end of the
  // bind round trip. A test that binds, pumps, and only then adds a listener
  // has no listener attached at the moment they are dispatched, and sees
  // nothing: the events were delivered to an object that was not listening
  // yet, which is indistinguishable from never having been sent.
  //
  // Real clients hit exactly the same ordering requirement, which is why
  // wl_registry_bind returns a proxy synchronously: attach first, round trip
  // second.
  void *bind_deferred(const char *interface, const wl_interface *iface,
                      uint32_t version) {
    const GlobalInfo *g = find_global(interface);
    if (!g)
      return nullptr;
    const uint32_t use = version < g->version ? version : g->version;
    return wl_registry_bind(registry_, g->name, iface, use);
  }

  // Raw registry, for a test that needs to bind outside bind()/
  // bind_deferred()'s version clamp -- e.g. deliberately requesting a
  // version above what the compositor advertised, to exercise libwayland's
  // own version enforcement rather than anything this project implements.
  [[nodiscard]] wl_registry *registry() {
    globals(); // ensures registry_ exists and is populated
    return registry_;
  }

private:
  // Non-blocking read of whatever the compositor sent. The prepare/read/
  // cancel dance is libwayland's documented way to poll a connection without
  // committing to a blocking wl_display_dispatch().
  void read_client_events() {
    while (wl_display_prepare_read(client_display_) != 0)
      wl_display_dispatch_pending(client_display_);
    wl_display_flush(client_display_);

    pollfd p{};
    p.fd = wl_display_get_fd(client_display_);
    p.events = POLLIN;
    if (::poll(&p, 1, 0) > 0)
      wl_display_read_events(client_display_);
    else
      wl_display_cancel_read(client_display_);

    wl_display_dispatch_pending(client_display_);
  }

  static void on_global(void *data, wl_registry *, uint32_t name,
                        const char *interface, uint32_t version) {
    auto *self = static_cast<WaylandClientFixture *>(data);
    self->globals_.push_back({name, interface ? interface : "", version});
  }
  static void on_global_remove(void *data, wl_registry *, uint32_t name) {
    auto *self = static_cast<WaylandClientFixture *>(data);
    for (auto it = self->globals_.begin(); it != self->globals_.end(); ++it) {
      if (it->name == name) {
        self->globals_.erase(it);
        return;
      }
    }
  }

  wl_display *server_display_{nullptr};
  wl_event_loop *server_loop_{nullptr};
  wl_client *server_client_{nullptr};
  wl_display *client_display_{nullptr};
  wl_registry *registry_{nullptr};
  std::vector<GlobalInfo> globals_;
};

} // namespace ww_test
