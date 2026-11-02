#include "wwindows/core/event_loop.hpp"

#include <wwindows/support/log.hpp>
#include <wwindows/support/unique_fd.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <poll.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace ww {

// ---------------------------------------------------------------------------
// Portable poll()-based implementation.
//
// poll(2) (unlike epoll/kqueue) is plain POSIX and behaves identically on
// every *NIX target we care about — Linux, FreeBSD, and friends — so this
// is the multiplexer EventLoop is built on. We only ever watch a handful of
// fds (the Wayland fd, the X11 connection fd, the wakeup pipe), so poll()'s
// O(n) scan per call costs nothing in practice.
//
// Wakeups (stop()) use a classic self-pipe instead of Linux's eventfd,
// since eventfd has no POSIX equivalent.
// ---------------------------------------------------------------------------

namespace {

// Make a fd close-on-exec and non-blocking without relying on
// Linux-only flags to open()/pipe() (e.g. O_CLOEXEC on pipe2, which isn't
// available everywhere). fcntl() works identically on every POSIX system.
bool make_cloexec_nonblock(int fd) {
  int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    return false;
  flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return false;
  return true;
}

// EventFlags <-> struct pollfd::events/revents translation.
short to_poll_events(uint32_t ev) {
  short p = 0;
  if (ev & IoIn)
    p |= POLLIN;
  if (ev & IoOut)
    p |= POLLOUT;
  return p;
}

uint32_t from_poll_revents(short rev) {
  uint32_t ev = 0;
  if (rev & POLLIN)
    ev |= IoIn;
  if (rev & POLLOUT)
    ev |= IoOut;
  if (rev & POLLERR)
    ev |= IoErr;
  if (rev & (POLLHUP
#ifdef POLLRDHUP
             | POLLRDHUP
#endif
             ))
    ev |= IoHup;
  if (rev & POLLNVAL)
    ev |= IoErr;
  return ev;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct EventLoop::Impl {
  UniqueFd stop_read;  // self-pipe read end
  UniqueFd stop_write; // self-pipe write end — stop() writes to this

  // fd -> requested events / callback. Iteration order doesn't matter;
  // the pollfd vector is rebuilt from this each time run() loops.
  std::unordered_map<int, uint32_t> watched;
  std::unordered_map<int, Callback> callbacks;

  struct TimerEntry {
    IResizeTimer::TimePoint deadline;
    IResizeTimer::Callback callback;
  };
  std::map<IResizeTimer::TimerId, TimerEntry> timers;
  IResizeTimer::TimerId next_timer_id{1};

  std::atomic<bool> running{false};
  std::atomic<int> exit_code{0};
};

// ---------------------------------------------------------------------------
// EventLoop
// ---------------------------------------------------------------------------

EventLoop::EventLoop() : impl_{std::make_unique<Impl>()} {}

EventLoop::~EventLoop() = default;

Result<std::unique_ptr<EventLoop>> EventLoop::create() {
  auto loop = std::unique_ptr<EventLoop>(new EventLoop());
  auto &im = *loop->impl_;

  // Self-pipe for stop() wakeups.
  int fds[2];
  if (::pipe(fds) < 0) {
    WW_LOG_ERROR("[event-loop] pipe() failed: {}", ::strerror(errno));
    return std::unexpected(
        Error::unknown(std::string{"pipe: "} + ::strerror(errno)));
  }

  if (!make_cloexec_nonblock(fds[0]) || !make_cloexec_nonblock(fds[1])) {
    std::string msg = std::string{"fcntl (stop pipe): "} + ::strerror(errno);
    WW_LOG_ERROR("[event-loop] {}", msg);
    ::close(fds[0]);
    ::close(fds[1]);
    return std::unexpected(Error::unknown(msg));
  }

  im.stop_read = UniqueFd{fds[0]};
  im.stop_write = UniqueFd{fds[1]};

  WW_LOG_INFO("[event-loop] created (stop_read={}, stop_write={})",
              im.stop_read.get(), im.stop_write.get());
  return loop;
}

// ---------------------------------------------------------------------------
// add / modify / remove
// ---------------------------------------------------------------------------

VoidResult EventLoop::add(int fd, uint32_t events, Callback cb) {
  impl_->watched[fd] = events;
  // insert_or_assign, not emplace: emplace keeps the old callback when fd is
  // already registered, leaving the mask and callback maps disagreeing.
  impl_->callbacks.insert_or_assign(fd, std::move(cb));
  WW_LOG_DEBUG("[event-loop] watching fd={} (events=0x{:x})", fd, events);
  return {};
}

VoidResult EventLoop::modify(int fd, uint32_t events) {
  auto it = impl_->watched.find(fd);
  if (it == impl_->watched.end()) {
    WW_LOG_WARN("[event-loop] modify: fd {} not watched", fd);
    return std::unexpected(Error::unknown(
        std::string{"EventLoop::modify: fd not watched: "} +
        std::to_string(fd)));
  }
  it->second = events;
  WW_LOG_DEBUG("[event-loop] fd={} events now 0x{:x}", fd, events);
  return {};
}

VoidResult EventLoop::remove(int fd) {
  const size_t erased = impl_->watched.erase(fd);
  impl_->callbacks.erase(fd);
  if (erased)
    WW_LOG_DEBUG("[event-loop] stopped watching fd={}", fd);
  return {};
}

// ---------------------------------------------------------------------------
// One-shot timers
// ---------------------------------------------------------------------------

IResizeTimer::TimePoint EventLoop::now() const noexcept {
  return IResizeTimer::Clock::now();
}

IResizeTimer::TimerId EventLoop::schedule_at(
    TimePoint deadline, IResizeTimer::Callback callback) {
  if (!callback)
    return 0;
  TimerId id = impl_->next_timer_id++;
  if (id == 0)
    id = impl_->next_timer_id++;
  impl_->timers.emplace(id, Impl::TimerEntry{deadline, std::move(callback)});
  return id;
}

void EventLoop::cancel(TimerId id) noexcept {
  if (id != 0)
    impl_->timers.erase(id);
}

namespace {

template <typename LoopImpl>
int poll_timeout_ms(const LoopImpl &im, IResizeTimer::TimePoint now) {
  if (im.timers.empty())
    return -1;
  auto earliest = IResizeTimer::TimePoint::max();
  for (const auto &[_, timer] : im.timers)
    earliest = std::min(earliest, timer.deadline);
  if (earliest <= now)
    return 0;
  const auto remaining = earliest - now;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  const auto rounded = ms + (ms < remaining ? std::chrono::milliseconds{1}
                                             : std::chrono::milliseconds{0});
  return static_cast<int>(std::clamp<int64_t>(rounded.count(), 0,
                                              std::numeric_limits<int>::max()));
}

template <typename LoopImpl>
void dispatch_due_timers(LoopImpl &im, IResizeTimer::TimePoint now) {
  std::vector<IResizeTimer::Callback> callbacks;
  for (auto it = im.timers.begin(); it != im.timers.end();) {
    if (it->second.deadline <= now) {
      callbacks.push_back(std::move(it->second.callback));
      it = im.timers.erase(it);
    } else {
      ++it;
    }
  }
  for (auto &callback : callbacks)
    if (callback)
      callback();
}

} // namespace

// ---------------------------------------------------------------------------
// run / stop
// ---------------------------------------------------------------------------

int EventLoop::run() {
  auto &im = *impl_;
  im.running = true;
  im.exit_code = 0;

  WW_LOG_INFO("[event-loop] entering dispatch loop ({} fds watched)",
              im.watched.size());

  std::vector<pollfd> pfds;
  std::vector<int> pfd_owners; // pfds[i] <-> pfd_owners[i], skip index 0 (stop)

  while (im.running) {
    pfds.clear();
    pfd_owners.clear();

    pfds.push_back(pollfd{im.stop_read.get(), POLLIN, 0});
    pfd_owners.push_back(im.stop_read.get());

    for (const auto &[fd, ev] : im.watched) {
      pfds.push_back(pollfd{fd, to_poll_events(ev), 0});
      pfd_owners.push_back(fd);
    }

    // pfds.size() is size_t (64-bit here), but poll(2)'s count parameter is
    // nfds_t, whose width isn't portable: glibc/Linux defines it as a
    // 64-bit unsigned long, while FreeBSD (and other *BSDs) define it as a
    // 32-bit unsigned int. We only ever watch a handful of fds — nowhere
    // near UINT32_MAX — so the narrowing is safe; cast explicitly so it
    // reads as intentional instead of tripping -Wconversion on platforms
    // where nfds_t is the smaller type.
    const int timeout_ms = poll_timeout_ms(im, IResizeTimer::Clock::now());
    int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);

    if (n < 0) {
      if (errno == EINTR)
        continue; // interrupted by signal, retry
      WW_LOG_ERROR("[event-loop] poll() failed: {}", ::strerror(errno));
      im.exit_code = 1;
      break;
    }

    // Drain the wakeup pipe first if it fired.
    if (pfds[0].revents != 0) {
      char buf[64];
      while (::read(im.stop_read.get(), buf, sizeof(buf)) > 0) {
      }
      WW_LOG_DEBUG("[event-loop] stop() requested, leaving dispatch loop");
      im.running = false;
      continue;
    }

    for (size_t i = 1; i < pfds.size(); ++i) {
      if (pfds[i].revents == 0)
        continue;

      uint32_t mask = from_poll_revents(pfds[i].revents);
      auto it = im.callbacks.find(pfd_owners[i]);
      if (it != im.callbacks.end()) {
        // Invoke a copy: the callback is free to remove its own fd (a peer
        // that hung up, a completed transfer), which erases this map entry
        // -- and with it the std::function still executing. Later fds in
        // this pass are looked up afresh, so removing another is already
        // safe.
        const Callback cb = it->second;
        cb(mask);
      } else {
        WW_LOG_WARN("[event-loop] fd={} fired but has no callback (mask=0x{:x})",
                    pfd_owners[i], mask);
      }
    }

    // poll() also returns when the earliest timer expires. Dispatch after fd
    // callbacks so a configure acknowledgement arriving at the deadline can
    // satisfy the resize-settle conditions in this same loop iteration.
    dispatch_due_timers(im, IResizeTimer::Clock::now());
  }

  WW_LOG_INFO("[event-loop] exited (code={})", im.exit_code.load());
  return im.exit_code;
}

void EventLoop::stop(int exit_code) {
  WW_LOG_DEBUG("[event-loop] stop() requested (code={})", exit_code);
  impl_->exit_code = exit_code;

  // Write a byte to the nonblocking self-pipe — wakes poll() from any thread.
  // EINTR means no byte was written, so retry. EAGAIN/EWOULDBLOCK means the
  // pipe is already full; at least one wakeup byte is queued, which is enough
  // to make poll() return and stop the loop. Do not discard the result: on
  // fortified libc builds, write() is marked warn_unused_result, and treating
  // an unexpected error as success could leave run() blocked indefinitely.
  constexpr char byte = 1;
  for (;;) {
    const ssize_t written =
        ::write(impl_->stop_write.get(), &byte, sizeof(byte));
    if (written == static_cast<ssize_t>(sizeof(byte)))
      return;

    if (written < 0) {
      const int error = errno;
      if (error == EINTR)
        continue;
      if (error == EAGAIN || error == EWOULDBLOCK)
        return;
      WW_LOG_ERROR("[event-loop] stop-pipe write failed: {}",
                   ::strerror(error));
      return;
    }

    // A one-byte write to a pipe is atomic. A zero/short result is not an
    // expected retry case, and retrying blindly could hide a broken pipe.
    WW_LOG_ERROR("[event-loop] stop-pipe write returned {} bytes (expected 1)",
                 written);
    return;
  }
}

} // namespace ww
