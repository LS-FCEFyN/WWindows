#pragma once

// Shared between the files in sources/wayland/linux_dmabuf/ that together
// implement register_linux_dmabuf() (see ../LinuxDmabuf.hpp, one directory
// up, for the public entry point): structs and functions that cross the
// file-per-interface boundary.
//
//   linux_dmabuf/Buffer.cpp — the dmabuf-backed wl_buffer object
//   linux_dmabuf/Params.cpp — zwp_linux_buffer_params_v1
//   linux_dmabuf/Dmabuf.cpp — zwp_linux_dmabuf_v1, register_linux_dmabuf()
//
// Nothing here is part of WWindows' public API.

#include <array>
#include <cstdint>

struct wl_client;
struct wl_resource;
struct wl_buffer_interface;

namespace ww {

class IWindowBackend;

constexpr uint32_t DRM_FORMAT_ARGB8888 = 0x34325241u;
// Accepted (not advertised) by linux_dmabuf/Params.cpp's do_create() — a
// buffer in one of these has red and blue swapped relative to
// ARGB8888/XRGB8888, so X11Backend imports it and composites it through
// the Render extension (see X11Backend_render.cpp) to swap them back
// instead of presenting it directly and showing it wrong. Real clients hit
// this: Android's default RGBA_8888 surface format *is* ABGR8888, so
// Waydroid's surfaces request it whether or not this compositor
// advertised it. Not in SUPPORTED_DMABUF_FORMATS below since it's a
// best-effort fallback, not the zero-copy path we'd rather clients use.
constexpr uint32_t DRM_FORMAT_ABGR8888 = 0x34324241u;
constexpr uint32_t DRM_FORMAT_XBGR8888 = 0x34324258u;
constexpr uint32_t DRM_FORMAT_XRGB8888 = 0x34325258u;
constexpr uint32_t DRM_FORMAT_NV12 = 0x3231564eu;
constexpr uint32_t DMABUF_MODIFIER_SINCE = 3u;
constexpr uint64_t DRM_FORMAT_MOD_LINEAR = 0u;

// The formats do_create() actually accepts (see Params.cpp), in a single
// place so Dmabuf.cpp's plain format/modifier advertisement and
// Feedback.cpp's format table can't drift apart — a client must see the
// same set through both zwp_linux_dmabuf_v1.format/modifier and a
// zwp_linux_dmabuf_feedback_v1 format table, or feedback would recommend
// something the non-feedback path won't actually create a buffer for.
// Every entry here is implicitly paired with DRM_FORMAT_MOD_LINEAR. RGB
// formats use DRI3 zero-copy import; NV12 uses the CPU conversion fallback
// because X11 Render/Present cannot interpret a YUV drawable directly.
constexpr std::array<uint32_t, 3> SUPPORTED_DMABUF_FORMATS = {
    DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_NV12};

// -------------------------------------------------------------------------
// linux_dmabuf/Buffer.cpp
// -------------------------------------------------------------------------
extern const struct wl_buffer_interface dmabuf_buffer_iface;
void dmabuf_buffer_resource_destroy(wl_resource *r);

// -------------------------------------------------------------------------
// linux_dmabuf/Params.cpp
// -------------------------------------------------------------------------
struct Plane {
  int fd{-1};
  uint32_t offset{0};
  uint32_t stride{0};
  uint64_t modifier{0};
};

struct ParamsData {
  IWindowBackend *backend;
  std::array<Plane, 4> planes{};
  int plane_count{0};
  bool used{false};
  uint32_t format{0};
  uint8_t depth{0};
  uint8_t bpp{0};
};

// zwp_linux_dmabuf_v1.create_params's request handler. Defined here (next
// to ParamsData, the object it creates) rather than in
// Dmabuf.cpp; the latter references it directly by name when
// building the zwp_linux_dmabuf_v1_interface vtable.
void dmabuf_create_params(wl_client *client, wl_resource *resource,
                          uint32_t id);

// -------------------------------------------------------------------------
// linux_dmabuf/Feedback.cpp
// -------------------------------------------------------------------------
// zwp_linux_dmabuf_v1.get_default_feedback / get_surface_feedback request
// handlers — same story as dmabuf_create_params above: defined next to the
// zwp_linux_dmabuf_feedback_v1 object they create, referenced by name from
// Dmabuf.cpp's zwp_linux_dmabuf_v1_interface vtable. This backend has no
// notion of per-surface dmabuf preference (single device, single format
// set), so both requests hand back the same feedback; get_surface_feedback
// only differs in accepting (and ignoring) a surface argument, exactly as
// the protocol allows for a compositor with nothing surface-specific to
// say.
void dmabuf_get_default_feedback(wl_client *client, wl_resource *resource,
                                 uint32_t id);
void dmabuf_get_surface_feedback(wl_client *client, wl_resource *resource,
                                 uint32_t id, wl_resource *surface);

} // namespace ww
