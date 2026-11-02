#pragma once
#include <cstdint>
#include <memory>
#include <wwindows/support/error.hpp>
#include <wwindows/ports/imported_buffer.hpp>

struct wl_display;

namespace ww {

class IWindowBackend;

// Payload stored in the wl_buffer user-data for dmabuf-backed buffers.
// SurfaceManager::commit reads this to detect and present dmabuf buffers.
// uint32_t maps 1-to-1 to xcb_pixmap_t (typedef uint32_t).


// Registers zwp_linux_dmabuf_v1 v4 on the display — only when
// backend.dmabuf_main_device() has a value, since a dmabuf global with no
// device behind it can't import anything (clients fall back to wl_shm
// instead). Advertises ARGB8888 and XRGB8888 with DRM_FORMAT_MOD_LINEAR,
// both directly (format/modifier events) and via
// zwp_linux_dmabuf_feedback_v1 (get_default_feedback/get_surface_feedback).
VoidResult register_linux_dmabuf(wl_display *display, IWindowBackend &backend);

} // namespace ww