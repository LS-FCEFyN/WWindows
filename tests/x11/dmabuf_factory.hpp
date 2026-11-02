#pragma once

// Creates real dmabufs for the DRI3 integration suite.
//
// Testing the zero-copy path needs an actual dmabuf file descriptor, which
// means an actual GPU allocation. GBM is the least-effort way to get one
// that is both exportable (`gbm_bo_get_fd`) and CPU-writable
// (`gbm_bo_map`), and it is the same allocator Mesa clients use, so the
// buffers here are representative of what a real Wayland client hands the
// compositor rather than something synthesized specially for a test.
//
// Buffers are allocated GBM_BO_USE_LINEAR so they can be mapped and filled
// with a known pattern -- without that, a tiled allocation would still
// import fine but its bytes could not be compared against anything, and the
// "did the client's pixels actually reach the server" assertion is the whole
// point of the suite.
//
// The device is chosen by matching X11Backend::dmabuf_main_device(): the
// import has to happen on the same device the X server renders on, and
// resolving it that way doubles as a check that the backend reports a dev_t
// that actually corresponds to a device node.

#include <gbm.h>

#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace ww_test {

// A dmabuf plus the metadata import_dmabuf() needs alongside it.
struct Dmabuf {
  int fd{-1}; // owned by the caller; import_dmabuf() takes it
  uint16_t width{0};
  uint16_t height{0};
  uint16_t stride{0};
  uint64_t modifier{0};
  uint8_t depth{0};
  uint8_t bpp{0};

  Dmabuf() = default;
  Dmabuf(const Dmabuf &) = delete;
  Dmabuf &operator=(const Dmabuf &) = delete;
  Dmabuf(Dmabuf &&other) noexcept { *this = std::move(other); }
  Dmabuf &operator=(Dmabuf &&other) noexcept {
    if (this != &other) {
      close();
      fd = other.fd;
      width = other.width;
      height = other.height;
      stride = other.stride;
      modifier = other.modifier;
      depth = other.depth;
      bpp = other.bpp;
      other.fd = -1;
    }
    return *this;
  }
  ~Dmabuf() { close(); }

  void close() {
    if (fd >= 0)
      ::close(fd);
    fd = -1;
  }
  // Hand the descriptor to a callee that takes ownership of it.
  [[nodiscard]] int release() {
    const int out = fd;
    fd = -1;
    return out;
  }
};

class DmabufFactory {
public:
  // Opens the DRM node whose st_rdev matches `device` -- i.e. the one
  // X11Backend::dmabuf_main_device() named. Render nodes are tried before
  // card nodes because they need no authentication.
  static std::optional<DmabufFactory> open_matching(dev_t device) {
    const std::string path = find_node(device);
    if (path.empty())
      return std::nullopt;

    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
      return std::nullopt;

    gbm_device *dev = gbm_create_device(fd);
    if (!dev) {
      ::close(fd);
      return std::nullopt;
    }
    return DmabufFactory(fd, dev, path);
  }

  DmabufFactory(const DmabufFactory &) = delete;
  DmabufFactory &operator=(const DmabufFactory &) = delete;
  DmabufFactory(DmabufFactory &&other) noexcept { *this = std::move(other); }
  DmabufFactory &operator=(DmabufFactory &&other) noexcept {
    if (this != &other) {
      reset();
      drm_fd_ = other.drm_fd_;
      dev_ = other.dev_;
      path_ = std::move(other.path_);
      other.drm_fd_ = -1;
      other.dev_ = nullptr;
    }
    return *this;
  }
  ~DmabufFactory() { reset(); }

  [[nodiscard]] const std::string &node_path() const { return path_; }

  // Allocates a linear buffer and fills every pixel with `pixel`, which is
  // written as a native-endian 32-bit word -- so for GBM_FORMAT_ARGB8888 the
  // value 0xAARRGGBB lands as the bytes B,G,R,A, exactly as a client's own
  // memcpy would. Returns a Dmabuf with fd == -1 on failure.
  [[nodiscard]] Dmabuf create_filled(uint16_t width, uint16_t height,
                                     uint32_t fourcc, uint32_t pixel) {
    Dmabuf out;
    if (!dev_)
      return out;

    gbm_bo *bo = gbm_bo_create(dev_, width, height, fourcc,
                               GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
      return out;

    // Fill through a CPU map. GBM_BO_TRANSFER_WRITE on a linear buffer is
    // supported by every driver that supports GBM_BO_USE_LINEAR at all.
    uint32_t map_stride = 0;
    void *map_data = nullptr;
    void *addr = gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE,
                            &map_stride, &map_data);
    if (!addr) {
      gbm_bo_destroy(bo);
      return out;
    }
    auto *bytes = static_cast<uint8_t *>(addr);
    for (uint16_t y = 0; y < height; ++y) {
      auto *row = reinterpret_cast<uint32_t *>(bytes + y * map_stride);
      for (uint16_t x = 0; x < width; ++x)
        row[x] = pixel;
    }
    gbm_bo_unmap(bo, map_data);

    const int fd = gbm_bo_get_fd(bo);
    const uint32_t stride = gbm_bo_get_stride(bo);
    const uint64_t modifier = gbm_bo_get_modifier(bo);
    // The fd keeps the underlying buffer object alive on its own; the
    // gbm_bo handle is no longer needed once it is exported.
    gbm_bo_destroy(bo);
    if (fd < 0)
      return out;

    out.fd = fd;
    out.width = width;
    out.height = height;
    out.stride = static_cast<uint16_t>(stride);
    out.modifier = modifier;
    out.depth = depth_for(fourcc);
    out.bpp = 32;
    return out;
  }

  static constexpr uint8_t depth_for(uint32_t fourcc) {
    // XRGB/XBGR have no alpha channel, so they import at depth 24.
    return (fourcc == GBM_FORMAT_XRGB8888 || fourcc == GBM_FORMAT_XBGR8888)
               ? uint8_t{24}
               : uint8_t{32};
  }

private:
  DmabufFactory(int fd, gbm_device *dev, std::string path)
      : drm_fd_(fd), dev_(dev), path_(std::move(path)) {}

  void reset() {
    if (dev_)
      gbm_device_destroy(dev_);
    if (drm_fd_ >= 0)
      ::close(drm_fd_);
    dev_ = nullptr;
    drm_fd_ = -1;
  }

  static std::string find_node(dev_t device) {
    DIR *dir = ::opendir("/dev/dri");
    if (!dir)
      return {};
    std::string match;
    std::string render_fallback;
    while (dirent *e = ::readdir(dir)) {
      const std::string name = e->d_name;
      if (name.rfind("renderD", 0) != 0 && name.rfind("card", 0) != 0)
        continue;
      const std::string path = "/dev/dri/" + name;
      struct stat st{};
      if (::stat(path.c_str(), &st) != 0)
        continue;
      if (st.st_rdev == device) {
        match = path;
        break;
      }
      if (render_fallback.empty() && name.rfind("renderD", 0) == 0)
        render_fallback = path;
    }
    ::closedir(dir);
    // Falling back to any render node keeps the suite useful on a server
    // whose DRI3Open hands back a device node this process cannot stat
    // (a containerized /dev, most often); tests that specifically assert
    // on the reported dev_t check it directly rather than relying on this.
    return match.empty() ? render_fallback : match;
  }

  int drm_fd_{-1};
  gbm_device *dev_{nullptr};
  std::string path_;
};

} // namespace ww_test
