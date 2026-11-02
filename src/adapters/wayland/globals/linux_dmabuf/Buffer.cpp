// The dmabuf-backed wl_buffer object created by
// zwp_linux_buffer_params_v1.create/create_immed (Params.cpp). See
// impl.hpp for how this fits with the other files in this directory.
#include <wwindows/ports/imported_buffer.hpp>
#include "wwindows/adapters/wayland/globals/linux_dmabuf/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"

#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace ww {

namespace {
void dmabuf_buffer_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}
} // namespace

const struct wl_buffer_interface dmabuf_buffer_iface = {
    .destroy = dmabuf_buffer_destroy,
};

void dmabuf_buffer_resource_destroy(wl_resource *r) {
  auto *d = static_cast<ImportedBuffer *>(wl_resource_get_user_data(r));
  if (d) {
    // Must happen before anything else: tells a still-in-flight
    // import_dmabuf() callback (see ImportedBuffer::alive) not to touch
    // `d` once we delete it below.
    *d->alive = false;
    if (d->pixmap)
      d->backend->free_pixmap(d->pixmap);
    if (d->fd >= 0)
      ::close(d->fd);
    for (auto &plane : d->planes) {
      if (plane.fd >= 0)
        ::close(plane.fd);
      plane.fd = -1;
    }
    delete d;
  }
}

} // namespace ww
