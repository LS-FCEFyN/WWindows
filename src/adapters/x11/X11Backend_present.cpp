#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include <wwindows/support/frame_diag.hpp>
#include <wwindows/core/detail/pixel_scale.hpp>
#include <wwindows/adapters/x11/policy/nv12.hpp>

#include <wwindows/support/log.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/shm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

namespace ww {

namespace {

struct MappedDmaBuf {
  int fd{-1};
  void *address{MAP_FAILED};
  size_t length{0};
  bool sync_started{false};
};

bool map_dma_buf_for_read(int fd, uint64_t required_size, MappedDmaBuf &out) {
  if (fd < 0 || required_size == 0 || required_size > SIZE_MAX)
    return false;
  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size < 0)
    return false;
  const uint64_t reported_size = static_cast<uint64_t>(st.st_size);
  if (reported_size == 0 || reported_size < required_size) {
    WW_LOG_WARN("[x11] NV12 dma-buf plane is shorter than its declared layout "
                "(fd={}, size={}, required={})", fd, reported_size,
                required_size);
    return false;
  }
  const uint64_t map_size = reported_size;
  if (map_size > SIZE_MAX)
    return false;

  struct dma_buf_sync sync{};
  sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
  if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
    WW_LOG_WARN("[x11] NV12 dma-buf CPU sync start failed (fd={}, errno={}: {})",
                fd, errno, ::strerror(errno));
    return false;
  }
  out.fd = fd;
  out.sync_started = true;
  out.length = static_cast<size_t>(map_size);
  out.address = ::mmap(nullptr, out.length, PROT_READ, MAP_SHARED, fd, 0);
  if (out.address == MAP_FAILED) {
    WW_LOG_WARN("[x11] NV12 dma-buf mmap failed (fd={}, size={}, errno={}: {})",
                fd, out.length, errno, ::strerror(errno));
    struct dma_buf_sync end_sync{};
    end_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &end_sync);
    out.sync_started = false;
    return false;
  }
  return true;
}

void unmap_dma_buf(MappedDmaBuf &mapping) {
  if (mapping.address != MAP_FAILED)
    ::munmap(mapping.address, mapping.length);
  if (mapping.sync_started) {
    struct dma_buf_sync sync{};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)::ioctl(mapping.fd, DMA_BUF_IOCTL_SYNC, &sync);
  }
  mapping = MappedDmaBuf{};
}

} // namespace

void free_shm_state(xcb_connection_t *conn, ShmState &shm) {
  if (shm.shmid == -1)
    return;
  xcb_shm_detach(conn, shm.seg);
  shmdt(shm.data);
  shmctl(shm.shmid, IPC_RMID, nullptr);
  shm.seg = 0;
  shm.shmid = -1;
  shm.data = nullptr;
  shm.size = 0;
  shm.in_flight = false;
  shm.on_presented = {};
}

void X11Backend::present_pixels(uint32_t window, uint16_t width,
                                uint16_t height, uint32_t stride,
                                const void *pixels, BufferRegion src,
                                FramePresentedCallback on_presented) {
  const size_t required = static_cast<size_t>(stride) * height;

  // Copy the committed image into backend-owned memory before handing it to
  // asynchronous SHM presentation. This also covers NV12, whose conversion
  // ends by entering this function with an owned XRGB image.
  RetainedFrame retained;
  retained.width = width;
  retained.height = height;
  retained.stride = stride;
  retained.region = src;
  retained.pixels.resize(required);
  if (pixels)
    std::memcpy(retained.pixels.data(), pixels, required);
  else
    std::fill(retained.pixels.begin(), retained.pixels.end(), 0);
  uint32_t old_snapshot = 0;
  if (const auto old = impl_->present.retained_frames.find(window);
      old != impl_->present.retained_frames.end() && old->second.is_pixmap)
    old_snapshot = old->second.pixmap;
  impl_->present.retained_frames[window] = std::move(retained);
  if (old_snapshot)
    Impl::retire_retained_pixmap(*impl_, window, old_snapshot);

  if (!impl_->shm.has_shm) {
    if (on_presented)
      on_presented();
    WW_LOG_WARN("[x11] MIT-SHM unavailable; cannot present wl_shm frame");
    return;
  }

  auto &wshm = impl_->shm.window_shm[window];

  // Select only a segment the X server has finished reading. If both are
  // busy, queue the newest frame in process memory and retry it when a
  // completion event frees a segment. This is bounded to one pending frame
  // per window and prevents same-window SHM data races under a stalled X
  // server without blocking the compositor's event loop.
  int slot = -1;
  for (int offset = 0; offset < 2; ++offset) {
    const int candidate = (wshm.next + offset) & 1;
    if (!wshm.buffers[candidate].in_flight) {
      slot = candidate;
      break;
    }
  }
  if (slot < 0) {
    ::ww::diag::note(window, wshm.pending.present
                              ? ::ww::diag::Ev::ShmPendingReplaced
                              : ::ww::diag::Ev::ShmPendingStored);
    wshm.pending.present = true;
    wshm.pending.width = width;
    wshm.pending.height = height;
    wshm.pending.stride = stride;
    wshm.pending.src = src;
    wshm.pending.pixels.resize(required);
    if (wshm.pending.on_presented && on_presented) {
      auto previous = std::move(wshm.pending.on_presented);
      wshm.pending.on_presented =
          [previous = std::move(previous), next = std::move(on_presented)]() mutable {
            previous();
            next();
          };
    } else if (on_presented) {
      wshm.pending.on_presented = std::move(on_presented);
    }
    if (pixels)
      std::memcpy(wshm.pending.pixels.data(), pixels, required);
    else
      std::fill(wshm.pending.pixels.begin(), wshm.pending.pixels.end(), 0);
    return;
  }
  wshm.next = slot ^ 1;
  auto &shm = wshm.buffers[slot];

  // Grow this segment only when it is no longer in flight.
  if (shm.size < required) {
    free_shm_state(impl_->conn, shm); // no-op if this segment is still empty

    const size_t new_size = required * 2;
    const int new_shmid = shmget(IPC_PRIVATE, new_size, IPC_CREAT | 0600);
    if (new_shmid == -1) {
      WW_LOG_ERROR("[x11] shmget failed (size={}, errno={}: {}) — dropping frame",
                   new_size, errno, ::strerror(errno));
      if (on_presented)
        on_presented();
      return;
    }

    void *new_data = shmat(new_shmid, nullptr, 0);
    if (new_data == reinterpret_cast<void *>(-1)) {
      WW_LOG_ERROR("[x11] shmat failed (shmid={}, errno={}: {}) — dropping frame",
                   new_shmid, errno, ::strerror(errno));
      // The segment was created but never attached to any process; it must
      // still be explicitly removed or it leaks until the system reboots.
      shmctl(new_shmid, IPC_RMID, nullptr);
      if (on_presented)
        on_presented();
      return;
    }

    shm.shmid = new_shmid;
    shm.data = new_data;
    shm.size = new_size;
    shm.seg = xcb_generate_id(impl_->conn);
    xcb_shm_attach(impl_->conn, shm.seg, static_cast<uint32_t>(shm.shmid), 0);
    // Mark for deferred cleanup — kernel keeps it alive while attached
    shmctl(shm.shmid, IPC_RMID, nullptr);
    WW_LOG_DEBUG("[x11] grew SHM segment for window {} to {} bytes", window,
                 new_size);
  }

  if (pixels)
    std::memcpy(shm.data, pixels, required);
  else
    std::memset(shm.data, 0, required);

  // Retrieve or create a cached GC for this window.
  // Creating a GC on every frame is a round-trip per commit — very expensive.
  auto [gc_it, inserted] = impl_->window.gcs.emplace(window, xcb_gcontext_t{});
  if (inserted) {
    gc_it->second = xcb_generate_id(impl_->conn);
    xcb_create_gc(impl_->conn, gc_it->second, window, 0, nullptr);
  }

  const uint8_t depth = [&]() -> uint8_t {
    auto dit = impl_->window.window_depths.find(window);
    return dit != impl_->window.window_depths.end()
               ? dit->second
               : static_cast<uint8_t>(impl_->screen->root_depth);
  }();

  // xcb_shm_put_image's wire request has no explicit stride field (see
  // xcb_shm_put_image_request_t in xcb/shm.h) — the row pitch is
  // *implicitly* `total_width * bytes-per-pixel`. Passing the visible
  // `width` here is only correct when the caller's buffer happens to be
  // tightly packed (stride == width * bpp). Many real clients pad rows
  // beyond that (row alignment for SIMD/cache-line reasons is common), in
  // which case telling X the pitch is `width * bpp` silently misreads
  // every row after the first — X keeps consuming bytes at the wrong
  // pitch and the image shears/corrupts more with every row. Both depths
  // this backend creates windows at (24 and 32) are 4 bytes/pixel in
  // Z_PIXMAP format on essentially every modern X server, so derive the
  // *actual* total_width X needs from the real stride instead of trusting
  // `width` to imply it. (If a depth needing a different bpp is ever
  // added, this needs to become a real depth->bpp lookup.)
  constexpr uint32_t kBytesPerPixel = 4;
  const auto total_width =
      static_cast<uint16_t>(stride / kBytesPerPixel);

  // Window-geometry crop (xdg_surface.set_window_geometry): everything
  // outside `src` is the client's own drop shadow and invisible resize
  // border, and `src`'s origin — not the buffer's — is what belongs at the
  // window's top-left. Clamped hard, because these numbers arrive straight
  // from the client and a source rectangle that leaves the image is a
  // protocol error on our connection, not theirs.
  uint16_t src_x = 0;
  uint16_t src_y = 0;
  uint16_t src_w = width;
  uint16_t src_h = height;
  if (src.declared()) {
    src_x = static_cast<uint16_t>(std::clamp<int32_t>(src.x, 0, width));
    src_y = static_cast<uint16_t>(std::clamp<int32_t>(src.y, 0, height));
    src_w = static_cast<uint16_t>(
        std::min<int32_t>(src.width, width - src_x));
    src_h = static_cast<uint16_t>(
        std::min<int32_t>(src.height, height - src_y));
    if (src_w == 0 || src_h == 0) {
      WW_LOG_WARN("[x11] window geometry ({},{} {}x{}) selects nothing "
                  "inside a {}x{} buffer — dropping frame",
                  src.x, src.y, src.width, src.height, width, height);
      if (on_presented)
        on_presented();
      return;
    }
  }

  const xcb_void_cookie_t put_cookie =
      xcb_shm_put_image(impl_->conn, window, gc_it->second, total_width,
                    height, // total (virtual) image size
                    src_x, src_y, src_w, src_h, // src rect: the window proper
                    0, 0,                       // dst origin
                    depth, XCB_IMAGE_FORMAT_Z_PIXMAP,
                    1, // send_event: segment reuse waits for completion
                    shm.seg, 0);
  shm.in_flight = true;
  shm.put_sequence = put_cookie.sequence;
  ::ww::diag::note(window, ::ww::diag::Ev::ShmSubmit);
  shm.on_presented = std::move(on_presented);

  xcb_flush(impl_->conn);
}

void X11Backend::repaint_window(uint32_t window, BufferRegion src,
                                int32_t buffer_scale,
                                FramePresentedCallback on_presented) {
  const auto it = impl_->present.retained_frames.find(window);
  if (it == impl_->present.retained_frames.end()) {
    if (on_presented)
      on_presented();
    return;
  }

  // Copy the record locally because present_pixels/present_pixmap replace
  // the cache with a fresh retained copy. In particular, the pixel pointer
  // must remain valid until present_pixels has copied it into SHM/cache.
  RetainedFrame retained = it->second;
  const bool override_view = buffer_scale > 0;
  if (!override_view) {
    src = retained.region;
    buffer_scale = retained.buffer_scale;
  }
  if (retained.is_pixmap) {
    if (retained.pixmap)
      present_pixmap(window, retained.pixmap, src, buffer_scale,
                     std::move(on_presented));
    else if (on_presented)
      on_presented();
    return;
  }
  if (!retained.pixels.empty()) {
    const bool has_canonical = !retained.canonical_pixels.empty() &&
                               retained.canonical_width > 0 &&
                               retained.canonical_height > 0;
    const BufferRegion canonical_region =
        override_view ? src : retained.canonical_region;
    const int32_t canonical_scale =
        override_view ? buffer_scale : retained.canonical_scale;
    if (has_canonical) {
      // NV12's cached display pixels may have been stretched to an interim WM
      // target. Always rebuild from the canonical conversion on repaint so
      // the image returns to its committed dimensions after adoption instead
      // of cropping that old stretched cache to the now-smaller X11 window.
      auto scaled = detail::scale_shm_32(
          retained.canonical_pixels.data(), retained.canonical_width,
          retained.canonical_height, retained.canonical_stride, canonical_scale);
      present_pixels(window, scaled.width, scaled.height, scaled.stride,
                     scaled.bytes.data(),
                     detail::scale_region(canonical_region, canonical_scale),
                     std::move(on_presented));
    } else {
      // Other pixel-backed retained images are already in logical output
      // pixels. Re-present them without consulting the client resource that
      // supplied the original frame.
      present_pixels(window, retained.width, retained.height, retained.stride,
                     retained.pixels.data(), src, std::move(on_presented));
    }
    // present_pixels replaced the cache with its display-resolution copy;
    // carry the canonical NV12 conversion forward so later repaints and scale
    // changes never need to re-read the client's dmabuf planes.
    if (has_canonical) {
      if (auto updated = impl_->present.retained_frames.find(window);
          updated != impl_->present.retained_frames.end()) {
        updated->second.canonical_pixels = std::move(retained.canonical_pixels);
        updated->second.canonical_width = retained.canonical_width;
        updated->second.canonical_height = retained.canonical_height;
        updated->second.canonical_stride = retained.canonical_stride;
        updated->second.canonical_region = canonical_region;
        updated->second.canonical_scale = canonical_scale;
      }
    }
  } else if (on_presented) {
    on_presented();
  }
}

void X11Backend::repaint_window_scaled(uint32_t window, uint16_t width,
                                       uint16_t height,
                                       FramePresentedCallback on_presented) {
  const auto it = impl_->present.retained_frames.find(window);
  if (it == impl_->present.retained_frames.end() || width == 0 || height == 0) {
    if (on_presented)
      on_presented();
    return;
  }

  RetainedFrame retained = it->second;
  if (retained.is_pixmap) {
    if (retained.pixmap) {
      present_pixmap(window, retained.pixmap, retained.region,
                     retained.buffer_scale, std::move(on_presented), {},
                     width, height);
    } else if (on_presented) {
      on_presented();
    }
    return;
  }

  const bool has_canonical = !retained.canonical_pixels.empty() &&
                             retained.canonical_width > 0 &&
                             retained.canonical_height > 0;
  const uint8_t *source = has_canonical ? retained.canonical_pixels.data()
                                        : retained.pixels.data();
  const uint32_t source_stride = has_canonical ? retained.canonical_stride
                                                : retained.stride;
  const uint32_t source_width = has_canonical ? retained.canonical_width
                                               : retained.width;
  const uint32_t source_height = has_canonical ? retained.canonical_height
                                                : retained.height;
  BufferRegion source_region = has_canonical ? retained.canonical_region
                                               : retained.region;
  if (!source_region.declared())
    source_region = BufferRegion{0, 0,
                                 static_cast<uint16_t>(source_width),
                                 static_cast<uint16_t>(source_height)};
  const auto scaled = detail::scale_shm_32_to_size(
      source, source_stride, source_width, source_height, source_region,
      width, height);
  if (scaled.bytes.empty()) {
    if (on_presented)
      on_presented();
    return;
  }

  present_pixels(window, scaled.width, scaled.height, scaled.stride,
                 scaled.bytes.data(), BufferRegion{}, std::move(on_presented));
  // present_pixels replaces the displayed retained frame, but a converted
  // NV12 frame must keep its unscaled canonical source for later target sizes.
  if (has_canonical) {
    if (auto updated = impl_->present.retained_frames.find(window);
        updated != impl_->present.retained_frames.end()) {
      updated->second.canonical_pixels = std::move(retained.canonical_pixels);
      updated->second.canonical_width = retained.canonical_width;
      updated->second.canonical_height = retained.canonical_height;
      updated->second.canonical_stride = retained.canonical_stride;
      updated->second.canonical_region = retained.canonical_region;
      updated->second.canonical_scale = retained.canonical_scale;
    }
  }
}

void X11Backend::clear_retained_frame(uint32_t window) {
  const auto it = impl_->present.retained_frames.find(window);
  if (it == impl_->present.retained_frames.end())
    return;
  const uint32_t pixmap = it->second.is_pixmap ? it->second.pixmap : 0;
  impl_->present.retained_frames.erase(it);
  if (pixmap)
    Impl::retire_retained_pixmap(*impl_, window, pixmap);
}

void X11Backend::present_nv12(uint32_t window, uint16_t width,
                              uint16_t height, int fd_y, uint32_t offset_y,
                              uint32_t stride_y, int fd_uv, uint32_t offset_uv,
                              uint32_t stride_uv, BufferRegion src,
                              int32_t buffer_scale,
                              FramePresentedCallback on_presented,
                              uint16_t stretch_width,
                              uint16_t stretch_height) {
  const uint32_t uv_rows = (static_cast<uint32_t>(height) + 1u) / 2u;
  const uint32_t uv_row_bytes = ((static_cast<uint32_t>(width) + 1u) / 2u) * 2u;
  if (!width || !height || stride_y < width || stride_uv < uv_row_bytes) {
    WW_LOG_WARN("[x11] rejecting malformed NV12 plane dimensions/strides");
    if (on_presented)
      on_presented();
    return;
  }
  const uint64_t y_end = static_cast<uint64_t>(offset_y) +
                         static_cast<uint64_t>(stride_y) * height;
  const uint64_t uv_end = static_cast<uint64_t>(offset_uv) +
                          static_cast<uint64_t>(stride_uv) * uv_rows;
  MappedDmaBuf y_mapping;
  MappedDmaBuf uv_mapping;
  if (!map_dma_buf_for_read(fd_y, y_end, y_mapping) ||
      !map_dma_buf_for_read(fd_uv, uv_end, uv_mapping)) {
    unmap_dma_buf(y_mapping);
    unmap_dma_buf(uv_mapping);
    WW_LOG_WARN("[x11] NV12 CPU import failed; dropping frame");
    if (on_presented)
      on_presented();
    return;
  }

  const auto *y_plane = static_cast<const uint8_t *>(y_mapping.address);
  const auto *uv_plane = static_cast<const uint8_t *>(uv_mapping.address);
  const int32_t scale = std::max<int32_t>(buffer_scale, 1);
  const uint32_t canonical_stride = static_cast<uint32_t>(width) * 4u;
  std::vector<uint8_t> canonical(
      static_cast<size_t>(canonical_stride) * static_cast<size_t>(height));
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const int luma = y_plane[offset_y + static_cast<size_t>(y) * stride_y + x];
      const size_t chroma_offset = offset_uv +
          static_cast<size_t>(y / 2u) * stride_uv +
          static_cast<size_t>(x / 2u) * 2u;
      const uint8_t u = uv_plane[chroma_offset];
      const uint8_t v = uv_plane[chroma_offset + 1u];
      const uint32_t pixel = ww::x11::policy::nv12_to_xrgb(
          static_cast<uint8_t>(luma), u, v);
      std::memcpy(canonical.data() + static_cast<size_t>(y) * canonical_stride +
                      static_cast<size_t>(x) * 4u,
                  &pixel, sizeof(pixel));
    }
  }
  detail::ScaledPixels scaled;
  uint16_t out_width = width;
  uint16_t out_height = height;
  uint32_t out_stride = canonical_stride;
  const uint8_t *display_pixels = canonical.data();
  BufferRegion display_region = ww::detail::scale_region(src, scale);
  if (stretch_width != 0 && stretch_height != 0) {
    const BufferRegion source_region = src.declared()
        ? src : BufferRegion{0, 0, width, height};
    scaled = detail::scale_shm_32_to_size(
        canonical.data(), canonical_stride, width, height, source_region,
        stretch_width, stretch_height);
    out_width = scaled.width;
    out_height = scaled.height;
    out_stride = scaled.stride;
    display_pixels = scaled.bytes.data();
    display_region = BufferRegion{};
  } else if (scale > 1) {
    scaled = detail::scale_shm_32(canonical.data(), width, height,
                                  canonical_stride, scale);
    out_width = scaled.width;
    out_height = scaled.height;
    out_stride = scaled.stride;
    display_pixels = scaled.bytes.data();
  }
  unmap_dma_buf(y_mapping);
  unmap_dma_buf(uv_mapping);
  if (out_width == 0 || out_height == 0 || !display_pixels) {
    if (on_presented)
      on_presented();
    return;
  }
  present_pixels(window, out_width, out_height, out_stride, display_pixels,
                 display_region, std::move(on_presented));
  if (auto retained = impl_->present.retained_frames.find(window);
      retained != impl_->present.retained_frames.end()) {
    retained->second.canonical_pixels = std::move(canonical);
    retained->second.canonical_width = width;
    retained->second.canonical_height = height;
    retained->second.canonical_stride = canonical_stride;
    retained->second.canonical_region = src;
    retained->second.canonical_scale = scale;
  }
}

} // namespace ww
