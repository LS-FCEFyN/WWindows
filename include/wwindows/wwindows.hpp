#pragma once

#include <memory>
#include <wwindows/support/error.hpp>

namespace ww {

class EventLoop;

class WWindows {
public:
  ~WWindows();

  // Non-copyable, non-movable — single owner, single lifetime
  WWindows(const WWindows &) = delete;
  WWindows &operator=(const WWindows &) = delete;
  WWindows(WWindows &&) = delete;
  WWindows &operator=(WWindows &&) = delete;

  // Factory — returns a fully initialised instance or an Error
  static Result<std::unique_ptr<WWindows>> create();

  // Blocks until the server shuts down. Returns process exit code.
  int run();

  // Returns the event loop for external signal handling
  EventLoop &event_loop() const;

private:
  WWindows();

  // Pimpl: keeps heavy headers (wayland, xcb) out of this header
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww