#pragma once

#include <memory>
#include <string>
#include <functional>
#include <wwindows/ports/window_manager_watcher.hpp>
#include <wwindows/support/error.hpp>

namespace ww {

class EventLoop;

// IWindowManagerWatcher backed by i3's IPC socket.
//
// i3 does not put "this window is tiled" anywhere an X client can read it,
// but it will tell anyone who asks over its IPC socket: a GET_TREE reply
// describes every container -- including the rects real per-edge tiling
// needs, which no single `window` event payload carries enough of (no
// sibling or workspace context) -- and a subscription to `window` events
// reports each new window and each floating/tiling toggle as it happens.
// This connects, subscribes, seeds itself from one GET_TREE, and from then
// on keeps the socket in the EventLoop: a relevant event triggers a fresh
// GET_TREE rather than being parsed for its own tiling state directly, and
// reporting every window in that reply through the TilingCallback. The
// watcher itself keeps no per-window state; deduplication is the receiver's.
//
// Read-only: it never sends i3 a command, so it cannot change the layout.
//
// If the connection is lost (i3 exited, or restarted in place -- which,
// for an already-running i3, is the only way its IPC connections ever
// die) the watcher stops watching its socket and calls
// set_disconnected_callback's callback exactly once, then does nothing
// further itself: reconnecting is deliberately not this class's job (see
// that callback's own doc comment for why). Everything reported so far
// simply stands until a new I3Watcher is connected, which for the common
// case -- an in-place restart, where every window keeps its container --
// is also still correct; a genuine layout change made between the
// disconnect and the reconnect is caught by the fresh GET_TREE the new
// connection seeds itself from.
class I3Watcher final : public IWindowManagerWatcher {
public:
  ~I3Watcher() override;
  I3Watcher(const I3Watcher &) = delete;
  I3Watcher &operator=(const I3Watcher &) = delete;
  I3Watcher(I3Watcher &&) = delete;
  I3Watcher &operator=(I3Watcher &&) = delete;

  // Connects to the i3 IPC socket at `socket_path` (what i3 publishes in the
  // I3_SOCKET_PATH property of the X root window), subscribes to window
  // events, requests the current tree, and registers the socket with
  // `loop`. Blocks only for the connect and the two small requests, which
  // is fine at startup.
  static Result<std::unique_ptr<I3Watcher>> connect(EventLoop &loop,
                                                    const std::string &socket_path);

  void set_tiling_callback(TilingCallback cb) override;

  // Called exactly once, the moment the IPC connection is found to be
  // gone (i3 closed it, or a malformed frame made the stream
  // unrecoverable) -- never on a clean shutdown of WWindows itself, only
  // when the watcher can no longer do its job. This class does not
  // retry its own connection: i3 has no fixed listening socket to retry
  // against (the path comes from X11's I3_SOCKET_PATH root property,
  // which is how a *new* i3 -- or the same one, restarted -- announces
  // itself again), so reconnecting means going back through that
  // property, which is the composition root's job, not this class's.
  // The callback runs with the watcher already fully torn down (its fd
  // closed and removed from the EventLoop) but not yet destroyed; it may
  // safely destroy this I3Watcher itself (e.g. by resetting the
  // unique_ptr that owns it) -- nothing in this class touches itself
  // again afterward.
  using DisconnectedCallback = std::function<void()>;
  void set_disconnected_callback(DisconnectedCallback cb);

  // Reads and handles whatever i3 has sent. The EventLoop calls this when
  // the socket is readable; it is public so tests can drive it directly.
  void handle_readable();

private:
  I3Watcher();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww
