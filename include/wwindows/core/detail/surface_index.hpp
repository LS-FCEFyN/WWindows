#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct wl_resource;

namespace ww {


// Owns the lookup/index structures shared by SurfaceManager operations.
// It deliberately owns no Wayland resources and performs no protocol work:
// callers retain ownership of the Surface and OS resources.
class SurfaceIndex final {
public:
  using WindowMap = std::unordered_map<uint32_t, wl_resource *>;
  using WindowIterator = WindowMap::iterator;
  using ConstWindowIterator = WindowMap::const_iterator;

  WindowIterator find_window(uint32_t window) noexcept;
  ConstWindowIterator find_window(uint32_t window) const noexcept;
  WindowIterator window_end() noexcept;
  ConstWindowIterator window_end() const noexcept;

  void bind_window(uint32_t window, wl_resource *surface);
  void unbind_window(uint32_t window) noexcept;

  void push_popup(wl_resource *surface);
  [[nodiscard]] bool popups_empty() const noexcept;
  [[nodiscard]] std::size_t popup_count() const noexcept;
  [[nodiscard]] wl_resource *top_popup() const noexcept;
  void discard_top_popup() noexcept;

  void remove_popup(wl_resource *surface) noexcept;

private:
  WindowMap windows_;
  std::vector<wl_resource *> popup_stack_;
};

} // namespace ww
