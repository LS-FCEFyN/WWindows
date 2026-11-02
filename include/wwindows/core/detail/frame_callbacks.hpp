#pragma once

#include "wwindows/core/detail/surface_state.hpp"
#include <wwindows/support/frame_diag.hpp>

#include <ctime>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace ww::detail {

inline std::function<void()> make_frame_completion(
    std::vector<std::shared_ptr<FrameCallbackState>> callbacks,
    std::shared_ptr<bool> surface_alive, uint32_t diag_window = 0) {
  if (callbacks.empty())
    return {};
  return [callbacks = std::move(callbacks),
          surface_alive = std::move(surface_alive), diag_window]() mutable {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint32_t milliseconds = static_cast<uint32_t>(
        static_cast<uint64_t>(ts.tv_sec) * 1'000u +
        static_cast<uint64_t>(ts.tv_nsec) / 1'000'000u);
    for (const auto &callback : callbacks) {
      if (!callback || !callback->resource)
        continue;
      if (!surface_alive || !*surface_alive) {
        wl_resource_destroy(callback->resource);
        continue;
      }
      wl_callback_send_done(callback->resource, milliseconds);
      ::ww::diag::note(diag_window, ::ww::diag::Ev::FcbFired);
      wl_resource_destroy(callback->resource);
    }
    callbacks.clear();
  };
}

} // namespace ww::detail
