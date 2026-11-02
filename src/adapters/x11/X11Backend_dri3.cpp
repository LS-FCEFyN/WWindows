#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include <wwindows/support/log.hpp>
#include <wwindows/support/frame_diag.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/render.h>

namespace ww {

uint32_t X11Backend::Impl::snapshot_pixmap(Impl &im, uint32_t source) {
  const auto meta_it = im.present.pixmap_metadata.find(source);
  if (meta_it == im.present.pixmap_metadata.end())
    return 0;
  const PixmapMetadata meta = meta_it->second;
  if (!meta.width || !meta.height || !meta.depth)
    return 0;

  const xcb_pixmap_t copy = xcb_generate_id(im.conn);
  xcb_create_pixmap(im.conn, meta.depth, copy, im.screen->root, meta.width,
                    meta.height);
  const xcb_gcontext_t gc = xcb_generate_id(im.conn);
  xcb_create_gc(im.conn, gc, copy, 0, nullptr);
  xcb_copy_area(im.conn, static_cast<xcb_drawable_t>(source), copy, gc, 0, 0,
                0, 0, meta.width, meta.height);
  xcb_free_gc(im.conn, gc);

  im.present.pixmap_metadata.emplace(copy, meta);
  if (const auto source_it = im.present.render_source_pictures.find(source);
      source_it != im.present.render_source_pictures.end() &&
      source_it->second.pictformat) {
    const auto picture = xcb_generate_id(im.conn);
    xcb_render_create_picture(im.conn, picture, copy,
                              source_it->second.pictformat, 0, nullptr);
    im.present.render_source_pictures.emplace(
        copy, RenderSource{picture, meta.width, meta.height,
                           source_it->second.bgr_order,
                           source_it->second.pictformat});
  }
  xcb_flush(im.conn);
  return copy;
}

bool X11Backend::init_dri3_present() const {
  auto &im = *impl_;

  // Both extensions must be present
  const auto *dri3 = xcb_get_extension_data(im.conn, &xcb_dri3_id);
  const auto *present = xcb_get_extension_data(im.conn, &xcb_present_id);
  if (!dri3 || !dri3->present || !present || !present->present) {
    WW_LOG_DEBUG("[x11] DRI3 and/or Present X extension not available "
                 "(dri3={}, present={})",
                 dri3 && dri3->present, present && present->present);
    return false;
  }

  // Ask the X server which DRM device it's rendering on and hand us an fd.
  // This is exactly what XWayland does — avoids hard-coding
  // /dev/dri/renderD128.
  xcb_dri3_open_cookie_t cookie = xcb_dri3_open(im.conn, im.screen->root, 0);
  xcb_dri3_open_reply_t *reply = xcb_dri3_open_reply(im.conn, cookie, nullptr);
  if (!reply || reply->nfd != 1) {
    WW_LOG_DEBUG("[x11] DRI3Open did not return a usable device fd (nfd={})",
                 reply ? reply->nfd : -1);
    ::free(reply);
    return false;
  }

  int *fds = xcb_dri3_open_reply_fds(im.conn, reply);
  im.present.drm_fd = fds[0];
  ::free(reply);

  im.present.has_dri3_present = true;
  im.present.present_opcode = present->major_opcode;
  WW_LOG_INFO("[x11] DRI3 + Present available (drm_fd={})", im.present.drm_fd);
  return true;
}

bool X11Backend::has_dri3() const { return impl_->present.has_dri3_present; }

std::optional<dev_t> X11Backend::dmabuf_main_device() const {
  if (!impl_->present.has_dri3_present)
    return std::nullopt;

  struct stat st{};
  if (::fstat(impl_->present.drm_fd, &st) != 0) {
    WW_LOG_WARN("[x11] fstat on DRM device fd {} failed: {}", impl_->present.drm_fd,
               ::strerror(errno));
    return std::nullopt;
  }
  return st.st_rdev;
}

void X11Backend::import_dmabuf(int fd, uint32_t window, uint16_t width,
                               uint16_t height, uint8_t depth, uint8_t bpp,
                               uint16_t stride, uint64_t modifier,
                               bool bgr_order,
                               DmabufImportCallback on_imported) {
  if (!impl_->present.has_dri3_present) {
    ::close(fd);
    if (on_imported)
      on_imported(0);
    return;
  }

  // Pure local lookup, no round-trip: reject up front rather than spend a
  // pixmap id and an import attempt on a buffer we already know we can't
  // display correctly. init_render() (X11Backend_lifecycle.cpp) queries
  // and caches these once at startup.
  xcb_render_pictformat_t bgr_pictformat = 0;
  if (bgr_order) {
    bgr_pictformat = depth == 32 ? impl_->present.render_bgr32_pictformat
                                 : impl_->present.render_bgr24_pictformat;
    if (!bgr_pictformat) {
      WW_LOG_WARN("[x11] rejecting BGR-order dmabuf import (depth={}) — no "
                 "matching Render PictFormat available",
                 depth);
      ::close(fd);
      if (on_imported)
        on_imported(0);
      return;
    }
  }

  const xcb_render_pictformat_t source_pictformat =
      bgr_order ? bgr_pictformat
                : (depth == 32 ? impl_->present.render_visual32_pictformat
                               : impl_->present.render_root_pictformat);
  xcb_pixmap_t pixmap = xcb_generate_id(impl_->conn);

  // Checked (not the plain unchecked variant) so a failure — a real
  // possibility here, unlike Present's select_input above: this validates
  // client-supplied data (an incompatible modifier, mismatched stride, an
  // fd that isn't actually a dmabuf) — is reported deterministically
  // rather than silently producing an unusable pixmap. What we don't do
  // is xcb_request_check() on it: that blocks until the reply/error
  // arrives, and this runs from inside live Wayland-request dispatch (a
  // client's wl_surface.commit on a dmabuf-backed buffer) — exactly the
  // situation (see set_window_title's comment in X11Backend_window.cpp)
  // where a blocking XCB call can freeze the whole compositor, for every
  // client, until whatever else the X server is doing (most commonly a
  // window manager's server grab while reparenting) clears.
  //
  // A *checked void* request reports failure as an async X error tied to
  // its own sequence number, but reports success as silence — nothing is
  // ever sent back. That silence can't be told apart from "still being
  // processed" without some later, known-ordered request to serve as a
  // fence: xcb_get_input_focus below, chosen only because it's about the
  // cheapest round-trip there is. X11 guarantees strict per-connection
  // ordering, so by the time *that* reply comes back (polled for, never
  // blocked on — see poll_pending_dmabuf_imports), the server has
  // necessarily already finished processing our import request one way
  // or the other. dispatch_one() matches incoming errors against
  // request_sequence to catch failure as soon as it's reported, without
  // waiting for the fence at all.
  const int32_t buffers[1] = {fd};
  xcb_void_cookie_t import_cookie = xcb_dri3_pixmap_from_buffers_checked(
      impl_->conn, pixmap, window, 1, width, height, stride, 0, 0, 0, 0, 0,
      0, 0, depth, bpp, modifier, buffers);
  xcb_get_input_focus_cookie_t fence_cookie =
      xcb_get_input_focus(impl_->conn);
  xcb_flush(impl_->conn);

  // The kernel duplicated fd into the X server's namespace via SCM_RIGHTS
  // when xcb flushed the request above. We own our copy and must close it
  // explicitly regardless of how the import eventually resolves.
  ::close(fd);

  ::ww::diag::note(0, ::ww::diag::Ev::ImportStart, window);
  impl_->present.pending_dmabuf_imports.push_back(PendingDmabufImport{
      import_cookie.sequence, fence_cookie.sequence, pixmap, bgr_order,
      bgr_pictformat, source_pictformat, width, height, depth,
      std::move(on_imported)});
  WW_LOG_DEBUG("[x11] DRI3 import requested (pixmap={}, window={}, size={}x{}, stride={}, depth={}, bpp={}, bgr_order={}, mod=0x{:016x})",
               pixmap, window, width, height, stride, depth, bpp, bgr_order,
               modifier);
}

void X11Backend::Impl::finish_dmabuf_import(Impl &im,
                                            PendingDmabufImport pending,
                                            bool ok) {
  auto callback = std::move(pending.callback);
  ::ww::diag::note(0, ok ? ::ww::diag::Ev::ImportOk : ::ww::diag::Ev::ImportFail,
                   pending.pixmap);
  if (!ok) {
    if (callback)
      callback(0);
    return;
  }

  im.present.pixmap_metadata[pending.pixmap] = PixmapMetadata{
      pending.width, pending.height, pending.depth,
      pending.source_pictformat, pending.bgr_order};

  // A source Picture is retained for BGR channel correction and optional
  // buffer-scale resampling. Native scale-1 buffers still use Present.
  if (pending.source_pictformat) {
    const xcb_render_picture_t pic = xcb_generate_id(im.conn);
    xcb_render_create_picture(im.conn, pic,
                              static_cast<xcb_drawable_t>(pending.pixmap),
                              pending.source_pictformat, 0, nullptr);
    im.present.render_source_pictures[pending.pixmap] =
        RenderSource{pic, pending.width, pending.height, pending.bgr_order,
                     pending.source_pictformat};
    xcb_flush(im.conn);
    WW_LOG_DEBUG("[x11-render] source Picture requested (pixmap={}, picture={}, format={}, bgr={})",
                 pending.pixmap, pic, pending.source_pictformat, pending.bgr_order);
  }

  if (callback)
    callback(pending.pixmap);
}

void X11Backend::present_pixmap(uint32_t window, uint32_t pixmap,
                                BufferRegion src, int32_t buffer_scale,
                                FramePresentedCallback on_presented,
                                FramePresentedCallback on_source_consumed,
                                uint16_t stretch_width,
                                uint16_t stretch_height) {
  if (!impl_->present.has_dri3_present || !pixmap) {
    if (on_source_consumed)
      on_source_consumed();
    if (on_presented)
      on_presented();
    return;
  }

  // Keep an X11-owned snapshot of the source so a future Expose redraw does
  // not depend on the lifetime or mutability of the client's wl_buffer.
  // The source and snapshot are on the same X connection, so the copy is
  // ordered before the presentation request below.
  const uint32_t snapshot = Impl::snapshot_pixmap(*impl_, pixmap);
  if (!snapshot) {
    // Never fall back to presenting the client-owned pixmap directly: doing
    // so would make wl_buffer.release unsafe. Drop this frame and tell the
    // caller that we no longer read the source.
    ::ww::diag::note(window, ::ww::diag::Ev::SnapshotFail);
    WW_LOG_WARN("[x11] could not snapshot dmabuf pixmap {}; dropping frame", pixmap);
    if (on_source_consumed)
      on_source_consumed();
    if (on_presented)
      on_presented();
    return;
  }

  // The copy request is ordered before this fence on the same X connection.
  // Release the Wayland buffer only after the server has executed that copy;
  // waiting for Present completion would be unnecessarily late and does not
  // protect a source pixmap from being modified during the copy itself.
  if (on_source_consumed) {
    const auto source_fence = xcb_get_input_focus(impl_->conn);
    impl_->present.pending_frame_fences.push_back(PendingFrameFence{
        0, source_fence.sequence, std::move(on_source_consumed), false});
  }

  uint32_t old_snapshot = 0;
  if (const auto old = impl_->present.retained_frames.find(window);
      old != impl_->present.retained_frames.end() && old->second.is_pixmap)
    old_snapshot = old->second.pixmap;
  RetainedFrame frame;
  frame.is_pixmap = true;
  frame.region = src;
  frame.buffer_scale = buffer_scale;
  frame.pixmap = snapshot;
  if (const auto meta = impl_->present.pixmap_metadata.find(snapshot);
      meta != impl_->present.pixmap_metadata.end()) {
    frame.width = meta->second.width;
    frame.height = meta->second.height;
  }
  impl_->present.retained_frames[window] = std::move(frame);
  if (old_snapshot && old_snapshot != snapshot)
    Impl::retire_retained_pixmap(*impl_, window, old_snapshot);
  pixmap = snapshot;

  if (auto it = impl_->present.render_source_pictures.find(pixmap);
      it != impl_->present.render_source_pictures.end() &&
      (it->second.bgr_order || buffer_scale > 1 ||
       (stretch_width != 0 && stretch_height != 0))) {
    WW_LOG_TRACE("[x11] Present pixmap via Render (window={}, pixmap={}, scale={})",
                window, pixmap, buffer_scale);
    Impl::present_pixmap_via_render(
        *impl_, static_cast<xcb_window_t>(window), it->second, src,
        buffer_scale, stretch_width, stretch_height);
    if (on_presented) {
      ::ww::diag::note(window, ::ww::diag::Ev::RenderSubmit);
      FramePresentedCallback wrapped =
          [window, cb = std::move(on_presented)]() mutable {
            ::ww::diag::note(window, ::ww::diag::Ev::RenderDone);
            if (cb)
              cb();
          };
      const auto fence = xcb_get_input_focus(impl_->conn);
      impl_->present.pending_frame_fences.push_back(
          PendingFrameFence{window, fence.sequence, std::move(wrapped)});
      xcb_flush(impl_->conn);
    }
    return;
  }
  if (buffer_scale > 1 || (stretch_width != 0 && stretch_height != 0)) {
    ::ww::diag::note(window, ::ww::diag::Ev::ScaleDrop);
    WW_LOG_WARN("[x11] cannot scale dmabuf pixmap {} without a Render source Picture",
                pixmap);
    if (on_presented)
      on_presented();
    return;
  }

  // Window-geometry crop (xdg_surface.set_window_geometry). Present has no
  // source rectangle — it always copies the whole pixmap — but it does
  // take a destination offset, and a *negative* one is exactly equivalent:
  // shifting the buffer up and left by the geometry origin puts that
  // origin at the window's top-left, and X clips whatever now falls
  // outside the window, which is precisely the client's drop shadow. The
  // window is sized to the geometry rect, so the margin on the far side
  // goes the same way.
  //
  // This costs a page flip, in the sense that present_check_flip() only
  // flips at x_off == y_off == 0. Nothing is lost in practice: a flip also
  // requires the pixmap to match the window size exactly, which a buffer
  // carrying a shadow margin never does.
  const int16_t x_off = src.declared() ? static_cast<int16_t>(-src.x) : 0;
  const int16_t y_off = src.declared() ? static_cast<int16_t>(-src.y) : 0;

  const uint32_t serial = impl_->present.next_present_serial++;
  // Fires every frame — trace only.
  WW_LOG_TRACE("[x11] Present pixmap (window={}, pixmap={}, serial={}, off=({},{}))",
               window, pixmap, serial, x_off, y_off);

  {
    // Counted for every present, with or without a frame callback attached.
    ::ww::diag::note(window, ::ww::diag::Ev::PresentSubmit, serial);
    FramePresentedCallback wrapped =
        [window, cb = std::move(on_presented)]() mutable {
          ::ww::diag::note(window, ::ww::diag::Ev::PresentDone);
          if (cb)
            cb();
        };
    impl_->present.present_frame_callbacks[serial] =
        PendingPresentCallback{window, std::move(wrapped)};
  }
  impl_->present.present_in_flight_pixmaps.insert(pixmap);
  xcb_present_pixmap(impl_->conn, window, static_cast<xcb_pixmap_t>(pixmap),
                     /*serial=*/serial,
                     /*valid=*/XCB_NONE,  // valid region (0 = whole pixmap)
                     /*update=*/XCB_NONE, // update region (0 = whole)
                     x_off, y_off,
                     /*target_crtc=*/XCB_NONE,
                     /*wait_fence=*/XCB_NONE,
                     /*idle_fence=*/XCB_NONE,
                     /*options=*/XCB_PRESENT_OPTION_ASYNC, // no vblank wait
                     /*target_msc=*/0,
                     /*divisor=*/1,
                     /*remainder=*/0,
                     /*notifies_len*/ 0,
                     /*notifies=*/nullptr);

  xcb_flush(impl_->conn);
}

void X11Backend::Impl::free_pixmap_resource(Impl &im, uint32_t pixmap) {
  if (!pixmap)
    return;
  if (auto it = im.present.render_source_pictures.find(pixmap);
      it != im.present.render_source_pictures.end()) {
    xcb_render_free_picture(im.conn, it->second.picture);
    im.present.render_source_pictures.erase(it);
  }
  im.present.pixmap_metadata.erase(pixmap);
  im.present.retired_retained_pixmaps.erase(pixmap);
  im.present.retired_retained_pixmap_windows.erase(pixmap);
  im.present.present_in_flight_pixmaps.erase(pixmap);
  xcb_free_pixmap(im.conn, static_cast<xcb_pixmap_t>(pixmap));
}

void X11Backend::Impl::retire_retained_pixmap(Impl &im, uint32_t window,
                                               uint32_t pixmap) {
  if (!pixmap)
    return;
  if (im.present.present_in_flight_pixmaps.contains(pixmap)) {
    im.present.retired_retained_pixmaps.insert(pixmap);
    im.present.retired_retained_pixmap_windows[pixmap] = window;
    return;
  }
  free_pixmap_resource(im, pixmap);
}

void X11Backend::Impl::release_retired_pixmaps_for_window(Impl &im,
                                                           uint32_t window) {
  for (auto it = im.present.retired_retained_pixmap_windows.begin();
       it != im.present.retired_retained_pixmap_windows.end();) {
    if (it->second != window || im.present.present_in_flight_pixmaps.contains(it->first)) {
      // The GetInputFocus teardown fence orders our requests, but only
      // Present's IDLE_NOTIFY establishes that a pixmap used by Present is no
      // longer being read. Keep those entries retired until that event arrives.
      ++it;
      continue;
    }
    const uint32_t pixmap = it->first;
    it = im.present.retired_retained_pixmap_windows.erase(it);
    free_pixmap_resource(im, pixmap);
  }
  xcb_flush(im.conn);
}

void X11Backend::free_pixmap(uint32_t pixmap) {
  Impl::free_pixmap_resource(*impl_, pixmap);
  xcb_flush(impl_->conn);
}

} // namespace ww
