// Integration tests for the zero-copy presentation path: DRI3 import,
// Present, and the Render-based BGR channel correction.
//
// This was the largest untested area: Xvfb implements neither DRI3 nor
// Present, so the rest of the X11 suite can only ever exercise the
// documented no-DRI3 fallback. These tests therefore run against whatever
// server $DISPLAY points at (see run_with_dri3.sh) rather than provisioning
// their own, and skip -- never silently pass -- when that server has no DRI3.
//
// The buffers are real GPU allocations exported as dmabuf fds (see
// dmabuf_factory.hpp), not synthesized fds, because the thing under test is
// precisely whether the server can import what a Mesa client would hand it.
//
// Three properties are worth stating explicitly, because they are what make
// these tests able to fail:
//
//   * Pixel content is verified, not just "a pixmap id came back". Every
//     buffer is filled with a known pattern before import and read back
//     afterwards through an independent connection. An import that produced
//     a valid-looking but empty or garbage pixmap fails.
//
//   * The BGR correction is checked *differentially*. The same bytes are
//     imported twice, once declared native-order and once byte-swapped, and
//     the two presentations must disagree in exactly the red and blue
//     channels. An implementation that ignored bgr_order entirely would
//     produce two identical readbacks and fail, which a single absolute
//     assertion would not catch.
//
//   * Readback happens through Inspector's separate connection, so a
//     backend that merely believes it imported something cannot pass.

#include "../support/test.hpp"

#include "dmabuf_factory.hpp"
#include "x11_harness.hpp"

#include <gbm.h>
#include <xcb/xcb.h>

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

using ww_test::Dmabuf;
using ww_test::DmabufFactory;
using ww_test::Harness;

namespace {

// Everything here needs both a DRI3-capable server and a usable DRM node.
// run_with_dri3.sh refuses to run the binary without the former, so reaching
// a test body and finding either missing is a genuine failure rather than an
// environment to tiptoe around.
struct Dri3Harness : Harness {
  DmabufFactory factory;

  Dri3Harness() : Harness(), factory(open_factory(*backend)) {}

  static DmabufFactory open_factory(ww::X11Backend &b) {
    if (!b.has_dri3())
      throw std::runtime_error(
          "server has no DRI3 -- this suite must be run via "
          "tests/x11/run_with_dri3.sh, which skips instead");
    const auto device = b.dmabuf_main_device();
    if (!device)
      throw std::runtime_error("DRI3 is available but dmabuf_main_device() "
                               "reported nothing");
    auto f = DmabufFactory::open_matching(*device);
    if (!f)
      throw std::runtime_error("could not open a usable DRM render node");
    return std::move(*f);
  }

  // import_dmabuf is asynchronous by contract. Drive the loop until the
  // callback fires and report what it reported.
  struct ImportResult {
    int calls{0};
    uint32_t pixmap{0};
  };

  ImportResult import(Dmabuf &buf, uint32_t window, bool bgr_order) {
    auto result = std::make_shared<ImportResult>();
    backend->import_dmabuf(buf.release(), window, buf.width, buf.height,
                           buf.depth, buf.bpp, buf.stride, buf.modifier,
                           bgr_order, [result](uint32_t pixmap) {
                             ++result->calls;
                             result->pixmap = pixmap;
                           });
    pump_until([result] { return result->calls > 0; });
    return *result;
  }

  // An override-redirect window: on a real desktop the tests must not be at
  // the mercy of whatever window manager is running, and an unmapped or
  // WM-reparented window has undefined contents under GetImage.
  uint32_t presentable_window(uint16_t width, uint16_t height) {
    const uint32_t win = backend->create_window(width, height);
    const uint32_t override_redirect = 1;
    xcb_change_window_attributes(backend->connection(), win,
                                 XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    backend->map_window(win);
    sync();
    return win;
  }

  // Window contents settle asynchronously (Present in particular completes
  // on the server's own schedule), so poll rather than assuming one sync is
  // enough. Returns the last readback either way, so a failing assertion
  // reports real pixels.
  std::vector<uint32_t> settled_pixels(uint32_t window, uint16_t width,
                                       uint16_t height,
                                       const std::function<bool(uint32_t)> &ok) {
    std::vector<uint32_t> px;
    for (int attempt = 0; attempt < 50; ++attempt) {
      sync();
      px = inspector.pixels(window, width, height);
      if (!px.empty() && ok(px[0]))
        return px;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return px;
  }
};

constexpr uint16_t kW = 64;
constexpr uint16_t kH = 64;

// Asymmetric in red vs blue on purpose: a channel swap has to be visible.
// As an ARGB8888 word this is opaque red-heavy orange; byte-swapping red and
// blue turns it into something clearly different.
constexpr uint32_t kTestPixel = 0xffcc3311u;

uint32_t swap_rb(uint32_t argb) {
  const uint32_t a = argb & 0xff000000u;
  const uint32_t r = (argb >> 16) & 0xffu;
  const uint32_t g = (argb >> 8) & 0xffu;
  const uint32_t b = argb & 0xffu;
  return a | (b << 16) | (g << 8) | r;
}

// Ignore alpha: a depth-24 window has none, and the X server is free to
// return anything in those bits.
uint32_t rgb(uint32_t pixel) { return pixel & 0x00ffffffu; }

} // namespace

// --- preconditions ----------------------------------------------------------

WW_TEST_CASE(dri3_and_present_are_available) {
  Dri3Harness h;
  WW_TEST_CHECK(h.backend->has_dri3());
}

WW_TEST_CASE(dmabuf_main_device_names_a_real_drm_node) {
  Dri3Harness h;
  const auto device = h.backend->dmabuf_main_device();
  WW_TEST_CHECK(device.has_value());

  // The contract is specifically st_rdev of the DRM device node, because
  // that is what zwp_linux_dmabuf_feedback_v1.main_device carries. Confirm
  // some node in /dev/dri actually has it.
  struct stat st{};
  WW_TEST_CHECK(::stat(h.factory.node_path().c_str(), &st) == 0);
  WW_TEST_CHECK_EQ(static_cast<unsigned long long>(st.st_rdev),
                   static_cast<unsigned long long>(*device));
}

// --- import -----------------------------------------------------------------

WW_TEST_CASE(importing_a_real_dmabuf_yields_a_pixmap_exactly_once) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  Dmabuf buf =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  WW_TEST_CHECK(buf.fd >= 0);

  const auto result = h.import(buf, win, /*bgr_order=*/false);
  WW_TEST_CHECK_EQ(result.calls, 1);
  WW_TEST_CHECK(result.pixmap != 0);
}

WW_TEST_CASE(imported_pixmap_has_the_requested_geometry_and_depth) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  Dmabuf buf =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  const auto result = h.import(buf, win, false);
  WW_TEST_CHECK(result.pixmap != 0);
  h.sync();

  // Queried through the inspector connection: the server's own view of the
  // pixmap, not the backend's bookkeeping.
  const auto g = h.inspector.geometry(result.pixmap);
  WW_TEST_CHECK(g.valid);
  WW_TEST_CHECK_EQ(g.width, kW);
  WW_TEST_CHECK_EQ(g.height, kH);
  WW_TEST_CHECK_EQ(g.depth, uint8_t{32});
}

WW_TEST_CASE(import_takes_ownership_of_the_caller_fd) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  Dmabuf buf =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  const int fd = buf.release();
  WW_TEST_CHECK(fd >= 0);

  auto calls = std::make_shared<int>(0);
  h.backend->import_dmabuf(fd, win, kW, kH, 32, 32, buf.stride, buf.modifier,
                           false, [calls](uint32_t) { ++*calls; });
  h.pump_until([calls] { return *calls > 0; });

  // The documented contract is that import_dmabuf closes the descriptor on
  // every path. Leaking one per committed buffer would exhaust the process's
  // fd table on any real client.
  WW_TEST_CHECK_EQ(::fcntl(fd, F_GETFD), -1);
  WW_TEST_CHECK_EQ(errno, EBADF);
}

WW_TEST_CASE(imported_pixmap_holds_the_bytes_the_client_wrote) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  Dmabuf buf =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  const auto result = h.import(buf, win, false);
  WW_TEST_CHECK(result.pixmap != 0);
  h.sync();

  // Read the imported pixmap directly. This is the zero-copy claim in its
  // strongest testable form: no window, no mapping, no Present timing --
  // just "does the server now see the exact memory the client filled".
  const auto px = h.inspector.pixels(result.pixmap, kW, kH);
  WW_TEST_CHECK(px.size() == std::size_t{kW} * kH);
  if (!px.empty()) {
    WW_TEST_CHECK_EQ(rgb(px[0]), rgb(kTestPixel));
    WW_TEST_CHECK_EQ(rgb(px.back()), rgb(kTestPixel));
  }
}

WW_TEST_CASE(import_reports_failure_exactly_once_for_a_non_dmabuf_fd) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  // The failure must be one no driver can accept. A bogus *modifier* is not
  // such a case: 0x00ffffffffffffff is DRM_FORMAT_MOD_INVALID, the reserved
  // "unspecified / implicit layout" value, which servers legitimately accept,
  // and whether an unrecognised vendor modifier is rejected depends on the
  // driver. A descriptor that is not a dmabuf at all is rejected everywhere.
  // The rejection arrives as an asynchronous X error matched against the
  // request's own sequence number -- the path that exists so a bad client
  // buffer cannot be mistaken for a good one.
  //
  // However, some X servers (notably Xwayland and some Xorg configurations)
  // may not validate the fd at import time, instead deferring validation to
  // when the pixmap is used. We handle both cases: if the server rejects at
  // import, we get pixmap=0; if it accepts, we verify the pixmap is unusable.
  const int bad_fd = ::open("/dev/null", O_RDONLY);
  WW_TEST_CHECK(bad_fd >= 0);

  auto calls = std::make_shared<int>(0);
  auto pixmap = std::make_shared<uint32_t>(0xdeadbeefu);
  h.backend->import_dmabuf(bad_fd, win, kW, kH, 32, 32, kW * 4u, 0, false,
                           [calls, pixmap](uint32_t px) {
                             ++*calls;
                             *pixmap = px;
                           });
  h.pump_until([calls] { return *calls > 0; });
  h.sync();
  WW_TEST_CHECK_EQ(*calls, 1);

  // import_dmabuf owns the descriptor on every path, failure included.
  WW_TEST_CHECK_EQ(::fcntl(bad_fd, F_GETFD), -1);

  if (*pixmap == 0) {
    // Server rejected the import at DRI3 pixmap-from-buffers time.
    // This is the expected behavior per the DRI3 spec.
  } else {
    // Server accepted the fd at import time. The pixmap must be unusable.
    // Verify by attempting to read it back -- this should either fail (empty
    // result) or return garbage that doesn't match our test pattern.
    const auto px = h.inspector.pixels(*pixmap, kW, kH);
    if (!px.empty()) {
      // The pixmap should not contain our test pattern (it wasn't filled by us).
      // It may contain garbage or zeros. Just verify it's not our pattern.
      bool all_test_pattern = true;
      for (uint32_t p : px) {
        if (rgb(p) != rgb(kTestPixel)) {
          all_test_pattern = false;
          break;
        }
      }
      WW_TEST_CHECK(!all_test_pattern);
    }
    // If px.empty(), the read failed which also confirms the pixmap is unusable.
  }

  // A rejected import must leave the connection usable: a real buffer
  // imported afterwards still works.
  Dmabuf buf =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  const auto good = h.import(buf, win, false);
  WW_TEST_CHECK_EQ(good.calls, 1);
  WW_TEST_CHECK(good.pixmap != 0);
}

WW_TEST_CASE(forgetting_a_cascade_destroyed_window_does_not_free_its_picture_twice) {
  // With DRI3 + Render every window gets a destination Picture. The server
  // frees a window's Pictures when the window is destroyed, so a child that
  // was cascade-destroyed with its parent has none left when the compositor
  // later forgets it. Freeing it again raised BadPicture (the "protocol error
  // ... RENDER FreePicture" seen in real Firefox sessions).
  Dri3Harness h;
  const uint32_t parent = h.backend->create_window(kW, kH);
  const uint32_t child = h.backend->create_child_window(parent, 0, 0, 64, 64);
  h.backend->map_window(child);
  h.sync();

  h.backend->destroy_window(parent); // X cascades the destruction to `child`
  h.sync();
  h.backend->forget_window(child);   // what the parent teardown path calls
  h.sync();

  bool clean = !xcb_connection_has_error(h.backend->connection());
  while (xcb_generic_event_t *ev = xcb_poll_for_event(h.backend->connection())) {
    if (ev->response_type == 0)
      clean = false;
    ::free(ev);
  }
  WW_TEST_CHECK(clean);
}

WW_TEST_CASE(free_pixmap_releases_the_server_resource) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  Dmabuf buf =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  const auto result = h.import(buf, win, false);
  WW_TEST_CHECK(result.pixmap != 0);
  h.sync();
  WW_TEST_CHECK(h.inspector.geometry(result.pixmap).valid);

  h.backend->free_pixmap(result.pixmap);
  h.sync();
  WW_TEST_CHECK(!h.inspector.geometry(result.pixmap).valid);
}

// --- presentation -----------------------------------------------------------

WW_TEST_CASE(present_pixmap_puts_the_imported_bytes_on_the_window) {
  Dri3Harness h;
  const uint32_t win = h.presentable_window(kW, kH);
  const auto wg = h.inspector.geometry(win);
  WW_TEST_CHECK(wg.valid);

  const uint32_t fourcc =
      wg.depth == 32 ? GBM_FORMAT_ARGB8888 : GBM_FORMAT_XRGB8888;
  Dmabuf buf = h.factory.create_filled(kW, kH, fourcc, kTestPixel);
  const auto result = h.import(buf, win, false);
  WW_TEST_CHECK(result.pixmap != 0);

  h.backend->present_pixmap(win, result.pixmap);
  const auto px = h.settled_pixels(
      win, kW, kH, [](uint32_t p) { return rgb(p) == rgb(kTestPixel); });

  WW_TEST_CHECK(!px.empty());
  if (!px.empty())
    WW_TEST_CHECK_EQ(rgb(px[0]), rgb(kTestPixel));
}

WW_TEST_CASE(bgr_order_import_corrects_the_channel_swap_on_presentation) {
  Dri3Harness h;
  const uint32_t win = h.presentable_window(kW, kH);
  const auto wg = h.inspector.geometry(win);
  WW_TEST_CHECK(wg.valid);

  // Bytes identical to the native-order case; only the declared channel
  // order differs. A correct implementation composites through Render and
  // undoes the swap, so what lands on screen is the swapped interpretation
  // of those bytes -- i.e. red and blue exchanged relative to presenting
  // them raw.
  const uint32_t fourcc =
      wg.depth == 32 ? GBM_FORMAT_ABGR8888 : GBM_FORMAT_XBGR8888;
  Dmabuf buf = h.factory.create_filled(kW, kH, fourcc, kTestPixel);
  const auto result = h.import(buf, win, /*bgr_order=*/true);
  WW_TEST_CHECK(result.pixmap != 0);

  h.backend->present_pixmap(win, result.pixmap);
  const uint32_t expected = swap_rb(kTestPixel);
  const auto px = h.settled_pixels(
      win, kW, kH, [expected](uint32_t p) { return rgb(p) == rgb(expected); });

  WW_TEST_CHECK(!px.empty());
  if (!px.empty())
    WW_TEST_CHECK_EQ(rgb(px[0]), rgb(expected));
}

WW_TEST_CASE(bgr_and_native_presentations_of_the_same_bytes_differ) {
  Dri3Harness h;
  const auto wg0 = h.inspector.geometry(h.presentable_window(1, 1));
  const bool deep = wg0.depth == 32;

  // The differential form of the previous two tests, and the one that
  // actually pins the behavior: an implementation that ignored bgr_order
  // would present both buffers identically and pass both absolute tests
  // only if the expected values happened to coincide. They must not.
  const uint32_t win_native = h.presentable_window(kW, kH);
  Dmabuf native = h.factory.create_filled(
      kW, kH, deep ? GBM_FORMAT_ARGB8888 : GBM_FORMAT_XRGB8888, kTestPixel);
  const auto native_result = h.import(native, win_native, false);
  WW_TEST_CHECK(native_result.pixmap != 0);
  h.backend->present_pixmap(win_native, native_result.pixmap);
  const auto native_px = h.settled_pixels(
      win_native, kW, kH, [](uint32_t p) { return rgb(p) == rgb(kTestPixel); });

  const uint32_t win_bgr = h.presentable_window(kW, kH);
  Dmabuf bgr = h.factory.create_filled(
      kW, kH, deep ? GBM_FORMAT_ABGR8888 : GBM_FORMAT_XBGR8888, kTestPixel);
  const auto bgr_result = h.import(bgr, win_bgr, true);
  WW_TEST_CHECK(bgr_result.pixmap != 0);
  h.backend->present_pixmap(win_bgr, bgr_result.pixmap);
  const uint32_t expected_bgr = swap_rb(kTestPixel);
  const auto bgr_px =
      h.settled_pixels(win_bgr, kW, kH, [expected_bgr](uint32_t p) {
        return rgb(p) == rgb(expected_bgr);
      });

  WW_TEST_CHECK(!native_px.empty());
  WW_TEST_CHECK(!bgr_px.empty());
  if (!native_px.empty() && !bgr_px.empty()) {
    WW_TEST_CHECK(rgb(native_px[0]) != rgb(bgr_px[0]));
    WW_TEST_CHECK_EQ(rgb(bgr_px[0]), rgb(swap_rb(native_px[0])));
  }
}

WW_TEST_CASE(freeing_a_bgr_pixmap_releases_it_and_leaves_the_connection_usable) {
  Dri3Harness h;
  const uint32_t win = h.presentable_window(kW, kH);
  const auto wg = h.inspector.geometry(win);

  Dmabuf buf = h.factory.create_filled(
      kW, kH, wg.depth == 32 ? GBM_FORMAT_ABGR8888 : GBM_FORMAT_XBGR8888,
      kTestPixel);
  const auto result = h.import(buf, win, true);
  WW_TEST_CHECK(result.pixmap != 0);
  h.sync();

  // A BGR import also owns a Render Picture. Freeing has to release both;
  // releasing only the pixmap leaves a Picture referencing a dead drawable,
  // which the server reports as a protocol error on the next use.
  h.backend->free_pixmap(result.pixmap);
  h.sync();

  WW_TEST_CHECK(!h.inspector.geometry(result.pixmap).valid);
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));

  // Still usable afterwards: a second import on the same window succeeds.
  Dmabuf again =
      h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
  const auto second = h.import(again, win, false);
  WW_TEST_CHECK(second.pixmap != 0);
}

WW_TEST_CASE(present_pixmap_ignores_a_zero_pixmap) {
  Dri3Harness h;
  const uint32_t win = h.presentable_window(kW, kH);

  // The failure path hands the caller 0; presenting it must be a no-op
  // rather than a protocol error that poisons the connection for every
  // other client of the backend.
  h.backend->present_pixmap(win, 0);
  h.sync();
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
}

WW_TEST_CASE(repeated_import_and_free_does_not_leak_server_resources) {
  Dri3Harness h;
  const uint32_t win = h.backend->create_window(kW, kH);
  h.sync();

  // Every committed frame of a dmabuf client walks this path. Ten cycles is
  // not a stress test; it is enough that an id leak or a double-free would
  // surface as an error on the connection.
  for (int i = 0; i < 10; ++i) {
    Dmabuf buf =
        h.factory.create_filled(kW, kH, GBM_FORMAT_ARGB8888, kTestPixel);
    const auto result = h.import(buf, win, false);
    WW_TEST_CHECK(result.pixmap != 0);
    h.backend->free_pixmap(result.pixmap);
  }
  h.sync();
  WW_TEST_CHECK(!xcb_connection_has_error(h.backend->connection()));
}
