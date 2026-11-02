#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"
#include <wwindows/core/policy/surface_geometry.hpp>
#include <wwindows/core/detail/pixel_scale.hpp>

#include <wwindows/support/log.hpp>
#include <wwindows/ports/imported_buffer.hpp>

#include <memory>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace ww {

void SurfaceManager::on_expose(uint32_t x11_window) {
  // The backend owns a retained copy of the most recently presented image.
  // Never re-read the client's last wl_buffer here: it may have been
  // destroyed or legally reused after wl_buffer.release.
  if (!x11_window)
    return;

  auto window_it = impl_->surface_index.find_window(x11_window);
  if (window_it != impl_->surface_index.window_end()) {
    auto surface_it = impl_->surfaces.find(window_it->second);
    if (surface_it != impl_->surfaces.end()) {
      const Surface &state = surface_it->second;
      constexpr uint64_t kMaxStretchPixels = 8192ull * 8192ull;
      const policy::WindowSize target = state.resize_target();
      const uint16_t target_w = target.width;
      const uint16_t target_h = target.height;
      const bool size_differs =
          state.committed.content_size && *state.committed.content_size != target;
      const Toplevel *const top = state.live_toplevel();
      const bool stale_toplevel =
          top && top->configured && top->waiting_for_resize_frame &&
          size_differs && !target.empty();
      const bool can_cpu_stretch =
          static_cast<uint64_t>(target_w) * target_h <= kMaxStretchPixels;

      if (const ShmContent *shm = state.content.retained_shm();
          shm && state.content.has_size()) {
        const BufferRegion region = state.committed.buffer_region(
            state.content.width, state.content.height);
        if (stale_toplevel && can_cpu_stretch) {
          const BufferRegion source_region =
              region.declared()
                  ? region
                  : BufferRegion{0, 0, state.content.width,
                                 state.content.height};
          const auto scaled = detail::scale_shm_32_to_size(
              shm->pixels.data(), shm->stride,
              state.content.width, state.content.height, source_region,
              target_w, target_h);
          if (!scaled.bytes.empty()) {
            impl_->backend.present_pixels(x11_window, scaled.width,
                                          scaled.height, scaled.stride,
                                          scaled.bytes.data(), BufferRegion{},
                                          {});
            return;
          }
        }
        // Not stale: the backend's retained presentation is current (an
        // adopted resize already re-presented the committed frame), so Expose
        // is a plain repaint of it. A stale target too large for safe CPU
        // scaling also lands here rather than allocating an unbounded
        // temporary image.
        impl_->backend.repaint_window(x11_window);
        return;
      }

      if (stale_toplevel && can_cpu_stretch) {
        // Imported RGB dmabuf frames can be scaled by the backend's Render
        // path; converted NV12 frames use their retained canonical XRGB copy.
        // Expose repaints the retained frame and never sends another configure.
        impl_->backend.repaint_window_scaled(x11_window, target_w, target_h);
        return;
      }
    }
  }

  impl_->backend.repaint_window(x11_window);
}


} // namespace ww
