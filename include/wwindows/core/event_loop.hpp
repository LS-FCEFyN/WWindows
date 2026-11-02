#pragma once

#include <cstdint>
#include <chrono>
#include <wwindows/ports/resize_timer.hpp>
#include <functional>
#include <memory>
#include <wwindows/support/error.hpp>

namespace ww {

// Portable readiness flags for EventLoop::add/modify and the fd-ready
// callback. Deliberately NOT epoll's EPOLLIN/EPOLLOUT/... — those are a
// Linux-only ABI. EventLoop translates to/from whatever the underlying
// multiplexer uses (poll() on every *NIX target we support: Linux,
// FreeBSD, and other POSIX systems).
enum EventFlags : uint32_t {
  IoIn = 1u << 0,  // fd is readable
  IoOut = 1u << 1, // fd is writable
  IoErr = 1u << 2, // error condition on fd
  IoHup = 1u << 3, // peer closed / hangup
};

// Generic POSIX-poll-based fd multiplexer.
// Wayland and X11 subsystems register their fds here.
class EventLoop : public IResizeTimer {
public:
  // Called when a registered fd becomes ready.
  // Receives a bitmask of EventFlags (IoIn, IoOut, IoErr, IoHup).
  using Callback = std::function<void(uint32_t events)>;

  ~EventLoop();
  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;
  EventLoop(EventLoop &&) = delete;
  EventLoop &operator=(EventLoop &&) = delete;

  static Result<std::unique_ptr<EventLoop>> create();

  // Watch a fd. Events is an EventFlags bitmask. Replaces any existing entry.
  VoidResult add(int fd, uint32_t events, Callback cb);

  // Update the event mask for a watched fd.
  VoidResult modify(int fd, uint32_t events);

  // Stop watching a fd.
  VoidResult remove(int fd);

  // One-shot timers share the same poll loop as fd readiness, so callbacks
  // run on the compositor thread and are never dispatched by a second loop.
  [[nodiscard]] TimePoint now() const noexcept override;
  TimerId schedule_at(TimePoint deadline, IResizeTimer::Callback callback) override;
  void cancel(TimerId id) noexcept override;

  // Block until stop() is called. Returns the exit code passed to stop().
  [[nodiscard]] int run();

  // Thread-safe: wake the loop and tell run() to return.
  void stop(int exit_code = 0);

private:
  EventLoop();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww
