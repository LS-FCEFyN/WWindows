#pragma once

// Shared scaffolding for the X11 integration suites.
//
// Both x11_backend_tests.cpp (general adapter behavior, runs against Xvfb)
// and dri3_tests.cpp (the zero-copy path, needs a GPU-backed server) drive a
// real X11Backend against a real server and verify the result through an
// independent second connection. That scaffolding is identical for both, so
// it lives here rather than being copy-pasted -- a second copy is exactly
// the arrangement in which the two silently drift.

#include <wwindows/adapters/x11/x11_backend.hpp>
#include <wwindows/core/event_loop.hpp>

#include <xcb/shape.h>
#include <xcb/xcb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ww_test {

// An X server performs a full reset when its last client disconnects: every
// window and selection is destroyed and, briefly, new connections are
// refused. Each test here opens and closes its own connections, so without
// something holding the server up the suite would reset between every test
// and roughly half of them would fail to connect at all.
//
// One connection held for the lifetime of the binary keeps the client count
// above zero throughout.
struct ServerKeepAlive {
  xcb_connection_t *conn{nullptr};
  ServerKeepAlive() {
    conn = xcb_connect(nullptr, nullptr);
    if (!conn || xcb_connection_has_error(conn))
      throw std::runtime_error("could not connect to X server (is DISPLAY set? "
                               "run via tests/x11/run_with_xvfb.sh)");
  }
  ~ServerKeepAlive() {
    if (conn)
      xcb_disconnect(conn);
  }
  ServerKeepAlive(const ServerKeepAlive &) = delete;
  ServerKeepAlive &operator=(const ServerKeepAlive &) = delete;
};

inline const ServerKeepAlive &keep_server_alive() {
  static ServerKeepAlive k;
  return k;
}

// An independent connection used only to verify what the server believes.
// Keeping it separate from the backend's own connection is the point: it is
// the difference between testing the server's state and testing our cache.
class Inspector {
public:
  Inspector() {
    keep_server_alive();
    conn_ = xcb_connect(nullptr, nullptr);
    if (!conn_ || xcb_connection_has_error(conn_))
      throw std::runtime_error("inspector could not connect to X server");
  }
  ~Inspector() {
    if (conn_)
      xcb_disconnect(conn_);
  }
  Inspector(const Inspector &) = delete;
  Inspector &operator=(const Inspector &) = delete;

  xcb_connection_t *conn() const { return conn_; }

  // xcb_get_geometry on a destroyed/never-created window returns an error.
  [[nodiscard]] bool window_exists(xcb_window_t w) {
    auto *reply = xcb_get_geometry_reply(conn_, xcb_get_geometry(conn_, w),
                                         nullptr);
    const bool ok = reply != nullptr;
    ::free(reply);
    return ok;
  }

  struct Geometry {
    int16_t x{0};
    int16_t y{0};
    uint16_t width{0};
    uint16_t height{0};
    uint8_t depth{0};
    bool valid{false};
  };

  // Works for any drawable, windows and pixmaps alike -- which is how the
  // DRI3 suite checks an imported pixmap's dimensions without needing it to
  // be on screen.
  [[nodiscard]] Geometry geometry(xcb_drawable_t d) {
    Geometry g;
    auto *reply = xcb_get_geometry_reply(conn_, xcb_get_geometry(conn_, d),
                                         nullptr);
    if (reply) {
      g.x = reply->x;
      g.y = reply->y;
      g.width = reply->width;
      g.height = reply->height;
      g.depth = reply->depth;
      g.valid = true;
      ::free(reply);
    }
    return g;
  }

  // The window's current *input* shape, as the Shape extension reports it
  // right now — not whatever WWindows believes it last set. Confirmed
  // empirically against a real server: a window whose input shape was
  // never touched, and one explicitly reset via ShapeMask(None), both
  // report a single rectangle covering the whole window; a window given an
  // explicitly empty rectangle list via ShapeRectangles(SO_SET, 0 rects)
  // reports zero rectangles — a real, distinct, queryable state rather
  // than a no-op.
  [[nodiscard]] std::vector<xcb_rectangle_t> input_shape_rects(xcb_window_t w) {
    std::vector<xcb_rectangle_t> out;
    auto *reply = xcb_shape_get_rectangles_reply(
        conn_, xcb_shape_get_rectangles(conn_, w, XCB_SHAPE_SK_INPUT),
        nullptr);
    if (!reply)
      return out;
    const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply);
    const int n = xcb_shape_get_rectangles_rectangles_length(reply);
    out.assign(rects, rects + n);
    ::free(reply);
    return out;
  }

  [[nodiscard]] bool is_mapped(xcb_window_t w) {
    auto *reply = xcb_get_window_attributes_reply(
        conn_, xcb_get_window_attributes(conn_, w), nullptr);
    if (!reply)
      return false;
    const bool mapped = reply->map_state != XCB_MAP_STATE_UNMAPPED;
    ::free(reply);
    return mapped;
  }

  [[nodiscard]] bool is_override_redirect(xcb_window_t w) {
    auto *reply = xcb_get_window_attributes_reply(
        conn_, xcb_get_window_attributes(conn_, w), nullptr);
    if (!reply)
      return false;
    const bool ov = reply->override_redirect != 0;
    ::free(reply);
    return ov;
  }

  [[nodiscard]] xcb_window_t parent_of(xcb_window_t w) {
    auto *reply =
        xcb_query_tree_reply(conn_, xcb_query_tree(conn_, w), nullptr);
    if (!reply)
      return 0;
    const xcb_window_t parent = reply->parent;
    ::free(reply);
    return parent;
  }

  // Children of `w` in bottom-to-top stacking order, as the server sees it.
  [[nodiscard]] std::vector<xcb_window_t> children_of(xcb_window_t w) {
    std::vector<xcb_window_t> out;
    auto *reply =
        xcb_query_tree_reply(conn_, xcb_query_tree(conn_, w), nullptr);
    if (!reply)
      return out;
    const xcb_window_t *kids = xcb_query_tree_children(reply);
    const int n = xcb_query_tree_children_length(reply);
    out.assign(kids, kids + n);
    ::free(reply);
    return out;
  }

  [[nodiscard]] std::string string_property(xcb_window_t w,
                                            xcb_atom_t property) {
    auto *reply = xcb_get_property_reply(
        conn_, xcb_get_property(conn_, 0, w, property, XCB_ATOM_ANY, 0, 1024),
        nullptr);
    if (!reply)
      return {};
    const auto *data = static_cast<const char *>(xcb_get_property_value(reply));
    const int len = xcb_get_property_value_length(reply);
    std::string out(data, data + len);
    ::free(reply);
    return out;
  }

  // A format-32 property as raw CARD32 values; empty if absent.
  [[nodiscard]] std::vector<uint32_t> u32_property(xcb_window_t w,
                                                   xcb_atom_t property) {
    auto *reply = xcb_get_property_reply(
        conn_, xcb_get_property(conn_, 0, w, property, XCB_ATOM_ANY, 0, 256),
        nullptr);
    std::vector<uint32_t> out;
    if (reply && reply->format == 32) {
      const auto *data =
          static_cast<const uint32_t *>(xcb_get_property_value(reply));
      out.assign(data, data + xcb_get_property_value_length(reply) / 4);
    }
    ::free(reply);
    return out;
  }

  [[nodiscard]] xcb_atom_t atom(const char *name) {
    auto *reply = xcb_intern_atom_reply(
        conn_,
        xcb_intern_atom(conn_, 0, static_cast<uint16_t>(std::strlen(name)),
                        name),
        nullptr);
    if (!reply)
      return XCB_ATOM_NONE;
    const xcb_atom_t a = reply->atom;
    ::free(reply);
    return a;
  }

  // Raw 32-bit pixels of a drawable region, as the server holds them.
  // Empty on failure (e.g. the drawable is gone, or a window's contents are
  // undefined because it is unmapped or obscured).
  [[nodiscard]] std::vector<uint32_t> pixels(xcb_drawable_t d, uint16_t width,
                                             uint16_t height) {
    std::vector<uint32_t> out;
    auto *reply = xcb_get_image_reply(
        conn_,
        xcb_get_image(conn_, XCB_IMAGE_FORMAT_Z_PIXMAP, d, 0, 0, width, height,
                      ~0u),
        nullptr);
    if (!reply)
      return out;
    const auto *data = static_cast<const uint8_t *>(xcb_get_image_data(reply));
    const int len = xcb_get_image_data_length(reply);
    out.resize(static_cast<std::size_t>(len) / 4);
    std::memcpy(out.data(), data, out.size() * 4);
    ::free(reply);
    return out;
  }

private:
  xcb_connection_t *conn_{nullptr};
};

// Owns an EventLoop and a live X11Backend, plus an inspector connection.
struct Harness {
  std::unique_ptr<ww::EventLoop> loop;
  std::unique_ptr<ww::X11Backend> backend;
  Inspector inspector;

  Harness() {
    keep_server_alive();
    auto l = ww::EventLoop::create();
    if (!l)
      throw std::runtime_error("EventLoop::create failed");
    loop = std::move(*l);

    auto b = ww::X11Backend::create(*loop);
    if (!b)
      throw std::runtime_error("X11Backend::create failed: " +
                               b.error().message);
    backend = std::move(*b);
  }

  // Force every queued request out and wait for the server to catch up, so
  // the inspector observes a settled state rather than racing the backend.
  void sync() {
    xcb_flush(backend->connection());
    auto *reply = xcb_get_input_focus_reply(
        backend->connection(), xcb_get_input_focus(backend->connection()),
        nullptr);
    ::free(reply);
    auto *reply2 = xcb_get_input_focus_reply(
        inspector.conn(), xcb_get_input_focus(inspector.conn()), nullptr);
    ::free(reply2);
  }

  // Run the event loop until `done` reports true, or `timeout_ms` elapses.
  // Returns what `done` last reported.
  //
  // Needed because the backend's asynchronous completions -- the DRI3 import
  // fence, and any X error that resolves an import as failed -- are only
  // processed from the fd callback the backend registered with the
  // EventLoop. Nothing else can reach them: Impl::process_events is private.
  //
  // Deliberately does NOT sync() first. sync() performs a blocking reply
  // wait on the backend's own connection, which makes libxcb read the
  // socket dry into its internal queue; the fence reply would then already
  // be buffered in userspace and the fd would never become readable again,
  // so run() would block forever waiting for a wakeup that cannot come.
  // Letting the loop see the server's reply arrive naturally is the whole
  // mechanism being exercised here.
  //
  // The watchdog turns "the completion never arrives" into a failed
  // assertion instead of a hung suite.
  bool pump_until(const std::function<bool()> &done, int timeout_ms = 3000) {
    if (done())
      return true;

    int wake[2]{-1, -1};
    if (::pipe(wake) != 0)
      throw std::runtime_error("pipe failed");

    bool timed_out = false;
    (void)loop->add(wake[0], ww::IoIn, [&](uint32_t) {
      timed_out = true;
      loop->stop();
    });
    backend->set_events_processed_callback([&] {
      if (done())
        loop->stop();
    });

    std::thread watchdog([&] {
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeout_ms);
      while (std::chrono::steady_clock::now() < deadline) {
        if (finished.load())
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      const char b = 'x';
      ssize_t ignored = ::write(wake[1], &b, 1);
      (void)ignored;
    });

    finished.store(false);
    (void)loop->run();
    finished.store(true);
    watchdog.join();

    backend->set_events_processed_callback(nullptr);
    (void)loop->remove(wake[0]);
    ::close(wake[0]);
    ::close(wake[1]);
    (void)timed_out;
    return done();
  }

  std::atomic<bool> finished{false};

  [[nodiscard]] xcb_window_t root() { return backend->screen()->root; }
};

} // namespace ww_test
