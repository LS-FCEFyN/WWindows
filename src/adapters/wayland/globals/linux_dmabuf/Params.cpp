// zwp_linux_buffer_params_v1 — collects the planes of a client-supplied
// dmabuf and turns them into a wl_buffer (Buffer.cpp). See impl.hpp for
// how this fits with the other files in this directory.
#include <wwindows/ports/imported_buffer.hpp>
#include "wwindows/adapters/wayland/globals/linux_dmabuf/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"
#include <wwindows/support/log.hpp>

#include <cstdint>
#include <format>
#include <wwindows/adapters/wayland/protocols/linux-dmabuf-unstable-v1-server-protocol.h>
#include <string_view>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace ww {

namespace {
void params_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

void params_add(wl_client *, wl_resource *resource, int32_t fd,
                uint32_t plane_idx, uint32_t offset, uint32_t stride,
                uint32_t modifier_hi, uint32_t modifier_lo) {
  auto *p = static_cast<ParamsData *>(wl_resource_get_user_data(resource));
  if (!p || plane_idx >= 4 || p->used) {
    WW_LOG_WARN(
        "[wayland] dmabuf.add rejected (plane_idx={}, already_used={})",
        plane_idx, p ? p->used : false);
    ::close(fd);
    return;
  }

  if (p->planes[plane_idx].fd >= 0)
    ::close(p->planes[plane_idx].fd);
  p->planes[plane_idx] =
      Plane{fd, offset, stride,
            (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo};
  if (static_cast<int>(plane_idx) >= p->plane_count)
    p->plane_count = static_cast<int>(plane_idx) + 1;

  WW_LOG_DEBUG("[wayland] dmabuf.add plane={} fd={} off={} stride={} mod=0x{:016x}",
               plane_idx, fd, offset, stride, p->planes[plane_idx].modifier);
}

void do_create(wl_client *client, wl_resource *params_res,
               uint32_t buffer_id, bool send_created, uint32_t width,
               uint32_t height, uint32_t format, uint32_t /*flags*/) {
  auto *p = static_cast<ParamsData *>(wl_resource_get_user_data(params_res));
  wl_resource *buf = nullptr;
  auto fail = [&](std::string_view reason) {
    WW_LOG_WARN("[wayland] dmabuf.create{} failed: {}",
                send_created ? "" : "_immed", reason);
    if (!send_created && buf)
      wl_resource_destroy(buf);
    zwp_linux_buffer_params_v1_send_failed(params_res);
  };

  if (!p || p->used) {
    fail(!p ? "no params" : "params already used");
    return;
  }
  p->used = true;
  if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX) {
    fail("buffer dimensions are outside the supported 1..65535 range");
    return;
  }

  WW_LOG_DEBUG("[wayland] dmabuf.create{} width={} height={} format=0x{:08x} plane_count={}",
               send_created ? "" : "_immed", width, height, format, p->plane_count);

  if (!send_created) {
    buf = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (!buf) {
      fail("wl_resource_create failed");
      return;
    }
  }

  const bool nv12 = format == DRM_FORMAT_NV12;
  if (nv12) {
    const uint32_t uv_row_bytes = ((width + 1u) / 2u) * 2u;
    if (p->plane_count != 2 || p->planes[0].fd < 0 ||
        p->planes[1].fd < 0) {
      fail("NV12 requires exactly two populated planes");
      return;
    }
    if (p->planes[0].modifier != DRM_FORMAT_MOD_LINEAR ||
        p->planes[1].modifier != DRM_FORMAT_MOD_LINEAR) {
      fail("NV12 currently requires the linear modifier for CPU conversion");
      return;
    }
    if (p->planes[0].stride < width ||
        p->planes[1].stride < uv_row_bytes) {
      fail("NV12 plane stride is smaller than the required row width");
      return;
    }
  } else if (p->plane_count != 1 || p->planes[0].fd < 0) {
    fail("RGB dmabufs require exactly one populated plane");
    return;
  }

  // NOTE: DRI3 import + Present blit the client's bytes onto the window
  // as-is for the native-order formats below — there is no channel-reorder
  // step in that path, so ARGB8888/XRGB8888 (whose byte layout already
  // matches this window's visual) go through it untouched. ABGR8888 and
  // XBGR8888 have red and blue swapped relative to that; silently
  // bucketing them with their native-order counterpart (as this used to
  // do) displays them with red/blue swapped. They're still accepted —
  // Android's default RGBA_8888 surface format *is* ABGR8888, so real
  // clients like Waydroid request it outright — but flagged via
  // `bgr_order` so X11Backend::import_dmabuf composites them through the
  // Render extension instead, which actually swaps the channels back. See
  // linux_dmabuf/impl.hpp and X11Backend_render.cpp.
  uint8_t depth, bpp;
  bool bgr_order = false;
  switch (format) {
  case DRM_FORMAT_ARGB8888:
    depth = 32;
    bpp = 32;
    break;
  case DRM_FORMAT_XRGB8888:
    depth = 24;
    bpp = 32;
    break;
  case DRM_FORMAT_ABGR8888:
    depth = 32;
    bpp = 32;
    bgr_order = true;
    break;
  case DRM_FORMAT_XBGR8888:
    depth = 24;
    bpp = 32;
    bgr_order = true;
    break;
  case DRM_FORMAT_NV12:
    depth = 24; // converted to XRGB8888 before presentation
    bpp = 32;
    break;
  default:
    fail("unsupported format 0x" +
         std::format("{:08x}", format) +
         " (accepted: ARGB8888/XRGB8888/ABGR8888/XBGR8888/NV12)");
    return;
  }

  const uint64_t modifier = p->planes[0].modifier;

  // RGB buffers retain the existing zero-copy DRI3 import path. Linear NV12
  // keeps both plane fds and is converted to XRGB on presentation, since an
  // X11 drawable/Render Picture has no way to interpret a YUV pixmap directly.
  auto *buf_data = new ImportedBuffer{
      .fd = nv12 ? -1 : p->planes[0].fd,
      .plane_count = static_cast<uint8_t>(nv12 ? 2 : 0),
      .format = nv12 ? ImportedBufferFormat::Nv12 : ImportedBufferFormat::Rgb,
      .pixmap = 0,
      .width = static_cast<uint16_t>(width),
      .height = static_cast<uint16_t>(height),
      .depth = depth,
      .bpp = bpp,
      .stride = static_cast<uint32_t>(p->planes[0].stride),
      .modifier = modifier,
      .backend = p->backend,
      .bgr_order = bgr_order};
  if (nv12) {
    for (std::size_t i = 0; i < 2; ++i) {
      buf_data->planes[i] = ImportedDmabufPlane{
          p->planes[i].fd, p->planes[i].offset, p->planes[i].stride,
          p->planes[i].modifier};
      p->planes[i].fd = -1;
    }
  } else {
    p->planes[0].fd = -1;
  }

  if (send_created) {
    // zwp_linux_buffer_params_v1.create (as opposed to create_immed) has no
    // client-supplied buffer_id: the server allocates the new wl_buffer's
    // id itself and reports it via the "created" event below. Passing 0 to
    // wl_resource_create asks libwayland to allocate that id, which is what
    // the now-deprecated wl_client_new_object did internally.
    buf = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    if (buf)
      wl_resource_set_implementation(buf, &dmabuf_buffer_iface, buf_data,
                                     dmabuf_buffer_resource_destroy);
  } else {
    wl_resource_set_user_data(buf, buf_data);
    wl_resource_set_implementation(buf, &dmabuf_buffer_iface, buf_data,
                                   dmabuf_buffer_resource_destroy);
  }

  if (!buf) {
    if (buf_data->fd >= 0)
      ::close(buf_data->fd);
    for (auto &plane : buf_data->planes) {
      if (plane.fd >= 0)
        ::close(plane.fd);
    }
    delete buf_data;
    fail("wl_resource_create failed for buffer");
    return;
  }

  WW_LOG_DEBUG("[wayland] dmabuf buffer created (id={}, mode={}, bgr_order={}, mod=0x{:016x})",
               wl_resource_get_id(buf), send_created ? "created" : "immed",
               bgr_order, modifier);

  if (send_created)
    zwp_linux_buffer_params_v1_send_created(params_res, buf);
}

void params_create(wl_client *client, wl_resource *resource, int32_t width,
                   int32_t height, uint32_t format, uint32_t flags) {
  do_create(client, resource, 0, /*send_created=*/true,
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), format,
            flags);
}

void params_create_immed(wl_client *client, wl_resource *resource,
                         uint32_t id, int32_t width, int32_t height,
                         uint32_t format, uint32_t flags) {
  do_create(client, resource, id, /*send_created=*/false,
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), format,
            flags);
}

void params_resource_destroy(wl_resource *r) {
  auto *p = static_cast<ParamsData *>(wl_resource_get_user_data(r));
  if (p) {
    for (auto &plane : p->planes) {
      if (plane.fd >= 0) {
        ::close(plane.fd);
        plane.fd = -1;
      }
    }
    delete p;
  }
}

const struct zwp_linux_buffer_params_v1_interface params_iface = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};
} // namespace

void dmabuf_create_params(wl_client *client, wl_resource *resource,
                          uint32_t id) {
  auto *backend =
      static_cast<IWindowBackend *>(wl_resource_get_user_data(resource));
  wl_resource *params =
      wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                         wl_resource_get_version(resource), id);
  auto *p = new ParamsData{backend};
  wl_resource_set_implementation(params, &params_iface, p,
                                 params_resource_destroy);
}

} // namespace ww
