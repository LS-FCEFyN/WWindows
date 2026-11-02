#include "wwindows/adapters/i3/i3_watcher.hpp"

#include <wwindows/adapters/i3/policy/i3_wire.hpp>
#include <wwindows/core/event_loop.hpp>
#include <wwindows/support/log.hpp>
#include <wwindows/support/unique_fd.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ww {

namespace {

// Blocking write of a whole small message. Only used before the socket goes
// non-blocking, for the two requests sent at startup.
bool write_all(int fd, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
                             MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

} // namespace

struct I3Watcher::Impl {
  EventLoop *loop{nullptr};
  UniqueFd fd;
  bool watching{false};
  std::string inbox; // bytes received but not yet forming a whole frame
  TilingCallback on_tiling;
  I3Watcher::DisconnectedCallback on_disconnected;

  // Coalesces GET_TREE re-fetches triggered by window events: at most one
  // request in flight at a time, with at most one more queued behind it
  // rather than one request per event. A burst of events in a single read
  // (common -- i3 often emits several for one user action) would otherwise
  // queue a request per event; since every request asks the exact same
  // question ("the tree, now"), only the *last* one queued during a burst
  // still needs to actually go out once the in-flight one answers, and the
  // in-flight one already covers everything that happened before it was
  // sent.
  bool refetch_in_flight{false};
  bool refetch_queued{false};

  // Sends a fresh GET_TREE request, honoring the coalescing above. Safe to
  // call from handle_frame() for every tiling-relevant window event; most
  // calls just set refetch_queued and return. A send failure (the socket's
  // write buffer is somehow full for a 14-byte message, or the connection
  // is already gone) is not treated as fatal here -- the next relevant
  // event tries again, and if the connection is genuinely dead the next
  // read will discover that through the normal disconnect path instead.
  void request_refetch() {
    if (!fd.valid())
      return;
    if (refetch_in_flight) {
      refetch_queued = true;
      return;
    }
    using namespace i3::policy;
    if (!write_all(fd.get(), encode_message(kMsgGetTree, {}))) {
      WW_LOG_WARN("[i3] GET_TREE re-fetch request failed to send; will retry "
                  "on the next relevant window event");
      return;
    }
    refetch_in_flight = true;
  }

  // Reports every window in a full tree snapshot (the startup one, or any
  // re-fetch's reply). Level-triggered on purpose: the watcher keeps no copy
  // of what it last said. The fact that matters -- which edges a toplevel is
  // currently tiled on -- lives in SurfaceManager, which already ignores a
  // report that changes nothing, so a second per-window map here could only
  // disagree with it (and did have one failure mode: a report for a window
  // the core had not bound yet was remembered as delivered and never
  // repeated). A window absent from the snapshot has closed and is simply not
  // mentioned -- see TilingCallback's doc comment: never called for a closed
  // window.
  void report_tree(const std::vector<i3::policy::TilingUpdate> &updates) {
    if (!on_tiling)
      return;
    for (const auto &u : updates)
      on_tiling(u.window, u.edges);
  }

  void handle_frame(const i3::policy::Frame &frame) {
    using namespace i3::policy;
    if (frame.type == kEventWindow) {
      if (window_event_may_change_tiling(frame.payload))
        request_refetch();
    } else if (frame.type == kMsgGetTree) {
      // Both the startup snapshot (sent directly by connect(), not through
      // request_refetch()) and every subsequent re-fetch arrive this same
      // way; refetch_in_flight is simply already false for the former.
      report_tree(tiling_from_tree(frame.payload));
      if (refetch_in_flight) {
        refetch_in_flight = false;
        if (refetch_queued) {
          refetch_queued = false;
          request_refetch();
        }
      }
    }
    // The SUBSCRIBE reply and every other event are of no interest.
  }

  // Tears down the socket and its EventLoop registration. Deliberately
  // does NOT touch on_disconnected -- invoking it is the caller's job
  // (I3Watcher::handle_readable(), the only caller), done as the very
  // last thing that caller does, since the callback may destroy this
  // Impl (see set_disconnected_callback's doc comment). Keeping that
  // invocation out of here means disconnect() itself never has to worry
  // about `this` disappearing out from under it mid-call.
  void disconnect(const char *why) {
    WW_LOG_WARN("[i3] IPC connection closed ({}); window layout hints stop here",
                why);
    if (watching && loop)
      (void)loop->remove(fd.get());
    watching = false;
    fd.reset();
    inbox.clear();
  }
};

I3Watcher::I3Watcher() : impl_{std::make_unique<Impl>()} {}

I3Watcher::~I3Watcher() {
  if (impl_->watching && impl_->loop)
    (void)impl_->loop->remove(impl_->fd.get());
}

void I3Watcher::set_tiling_callback(TilingCallback cb) {
  impl_->on_tiling = std::move(cb);
}

void I3Watcher::set_disconnected_callback(DisconnectedCallback cb) {
  impl_->on_disconnected = std::move(cb);
}

Result<std::unique_ptr<I3Watcher>>
I3Watcher::connect(EventLoop &loop, const std::string &socket_path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path))
    return std::unexpected(Error::unknown("i3 socket path is empty or too long"));
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);

  UniqueFd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (!fd.valid())
    return std::unexpected(Error::unknown(std::string{"i3 socket(): "} +
                                          ::strerror(errno)));
  if (::connect(fd.get(), reinterpret_cast<const sockaddr *>(&addr),
                sizeof(addr)) < 0)
    return std::unexpected(Error::unknown("connect " + socket_path + ": " +
                                          ::strerror(errno)));

  // Subscribe *before* asking for the tree: anything that changes between
  // the two arrives as an event after the snapshot and corrects it, whereas
  // the other order leaves a window in which a change is never seen.
  using namespace i3::policy;
  if (!write_all(fd.get(), encode_message(kMsgSubscribe, R"(["window"])")) ||
      !write_all(fd.get(), encode_message(kMsgGetTree, {})))
    return std::unexpected(Error::unknown(std::string{"i3 request: "} +
                                          ::strerror(errno)));

  const int flags = ::fcntl(fd.get(), F_GETFL);
  if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) < 0)
    return std::unexpected(Error::unknown(std::string{"i3 fcntl: "} +
                                          ::strerror(errno)));

  std::unique_ptr<I3Watcher> watcher{new I3Watcher()};
  auto &im = *watcher->impl_;
  im.loop = &loop;
  im.fd = std::move(fd);

  I3Watcher *self = watcher.get();
  if (auto r = loop.add(im.fd.get(), IoIn, [self](uint32_t) {
        self->handle_readable();
      });
      !r)
    return std::unexpected(r.error());
  im.watching = true;

  WW_LOG_INFO("[i3] connected to {} (subscribed to window events)",
              socket_path);
  return watcher;
}

void I3Watcher::handle_readable() {
  auto &im = *impl_;
  if (!im.fd.valid())
    return;

  // Tears the socket down, then fires the disconnected callback (if any)
  // as the very last action. See set_disconnected_callback's doc
  // comment: that callback may destroy this I3Watcher outright (e.g. by
  // resetting the unique_ptr that owns it), so nothing below may touch
  // `im` or `this` again afterward. Moving on_disconnected out of `im`
  // into a local `cb` is the last touch of `im`, strictly before the
  // call that might free its storage.
  auto disconnect_and_notify = [&im](const char *why) {
    im.disconnect(why);
    auto cb = std::move(im.on_disconnected);
    if (cb)
      cb();
  };

  bool eof = false;
  char chunk[8192];
  for (;;) {
    const ssize_t n = ::recv(im.fd.get(), chunk, sizeof(chunk), 0);
    if (n > 0) {
      im.inbox.append(chunk, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      // Deliver what arrived before the close, then give up.
      eof = true;
      break;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    disconnect_and_notify(::strerror(errno));
    return;
  }

  i3::policy::Frame frame;
  for (;;) {
    const auto status = i3::policy::take_frame(im.inbox, frame);
    if (status == i3::policy::FrameStatus::Incomplete)
      break;
    if (status == i3::policy::FrameStatus::Corrupt) {
      disconnect_and_notify("not an i3 IPC stream");
      return;
    }
    im.handle_frame(frame);
  }

  if (eof)
    disconnect_and_notify("i3 closed the socket");
}

} // namespace ww
