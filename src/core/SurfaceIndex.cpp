#include "wwindows/core/detail/surface_index.hpp"

#include <algorithm>

namespace ww {

SurfaceIndex::WindowIterator SurfaceIndex::find_window(uint32_t window) noexcept {
  return windows_.find(window);
}

SurfaceIndex::ConstWindowIterator
SurfaceIndex::find_window(uint32_t window) const noexcept {
  return windows_.find(window);
}

SurfaceIndex::WindowIterator SurfaceIndex::window_end() noexcept {
  return windows_.end();
}

SurfaceIndex::ConstWindowIterator SurfaceIndex::window_end() const noexcept {
  return windows_.end();
}

void SurfaceIndex::bind_window(uint32_t window, wl_resource *surface) {
  if (window != 0)
    windows_.insert_or_assign(window, surface);
}

void SurfaceIndex::unbind_window(uint32_t window) noexcept {
  if (window != 0)
    windows_.erase(window);
}

void SurfaceIndex::push_popup(wl_resource *surface) {
  popup_stack_.push_back(surface);
}

bool SurfaceIndex::popups_empty() const noexcept {
  return popup_stack_.empty();
}

std::size_t SurfaceIndex::popup_count() const noexcept {
  return popup_stack_.size();
}

wl_resource *SurfaceIndex::top_popup() const noexcept {
  return popup_stack_.empty() ? nullptr : popup_stack_.back();
}

void SurfaceIndex::discard_top_popup() noexcept {
  if (!popup_stack_.empty())
    popup_stack_.pop_back();
}

void SurfaceIndex::remove_popup(wl_resource *surface) noexcept {
  popup_stack_.erase(std::remove(popup_stack_.begin(), popup_stack_.end(), surface),
                     popup_stack_.end());
}

} // namespace ww
