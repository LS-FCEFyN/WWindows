#pragma once

// Pure decisions for talking to i3 -- no sockets, no X, no event loop -- so
// the protocol handling can be unit-tested against literal byte strings and
// JSON captured from a real i3, and so production calls the very same
// definitions the tests exercise.
//
// i3's IPC framing is: the six bytes "i3-ipc", a uint32 payload length, a
// uint32 message type, then the payload. Both integers are in *native* byte
// order (i3 and its clients always share a machine, it being a unix
// socket). Replies reuse the request's type; asynchronous events set the
// high bit.

#include <wwindows/adapters/i3/json.hpp>
#include <wwindows/ports/window_manager_watcher.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ww::i3::policy {

inline constexpr uint32_t kMsgSubscribe = 2;
inline constexpr uint32_t kMsgGetTree = 4;
inline constexpr uint32_t kEventWindow = 0x80000003u;

inline constexpr std::size_t kHeaderSize = 14; // "i3-ipc" + length + type

// Refuse absurd lengths rather than buffering them: a corrupt header must
// not turn into a multi-gigabyte allocation. A very large real i3 tree is a
// few megabytes.
inline constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;

inline std::string encode_message(uint32_t type, std::string_view payload) {
  std::string out;
  out.reserve(kHeaderSize + payload.size());
  out.append("i3-ipc", 6);
  const auto length = static_cast<uint32_t>(payload.size());
  out.append(reinterpret_cast<const char *>(&length), sizeof(length));
  out.append(reinterpret_cast<const char *>(&type), sizeof(type));
  out.append(payload);
  return out;
}

struct Frame {
  uint32_t type{0};
  std::string payload;
};

enum class FrameStatus {
  Incomplete, // not enough bytes yet -- read more and try again
  Ready,      // one frame was removed from the buffer into `out`
  Corrupt,    // the stream is not i3 IPC; the connection cannot be trusted
};

// Removes at most one complete frame from the front of `buffer`.
inline FrameStatus take_frame(std::string &buffer, Frame &out) {
  if (buffer.size() < kHeaderSize)
    return FrameStatus::Incomplete;
  if (std::memcmp(buffer.data(), "i3-ipc", 6) != 0)
    return FrameStatus::Corrupt;

  uint32_t length = 0;
  uint32_t type = 0;
  std::memcpy(&length, buffer.data() + 6, sizeof(length));
  std::memcpy(&type, buffer.data() + 10, sizeof(type));
  if (length > kMaxPayload)
    return FrameStatus::Corrupt;
  if (buffer.size() < kHeaderSize + length)
    return FrameStatus::Incomplete;

  out.type = type;
  out.payload.assign(buffer, kHeaderSize, length);
  buffer.erase(0, kHeaderSize + length);
  return FrameStatus::Ready;
}

// One window's layout, as i3 reported it.
struct TilingUpdate {
  uint32_t window{0};       // the client's X11 window id
  TiledEdges edges;         // all-false for a floating window
  bool closed{false};       // the window is gone; forget it
};

// i3's per-container "floating" field: "auto_off" / "user_off" for a tiled
// window, "auto_on" / "user_on" for a floating one.
inline bool is_tiled(std::string_view floating_field) {
  return floating_field != "auto_on" && floating_field != "user_on";
}

// A container's on-screen rect, i3's "rect" field: absolute pixel
// coordinates (x/y can be negative -- an output placed left of or above
// the primary one in a multi-monitor layout). Defaults to all-zero when
// the container carries no rect at all (a hand-written test fixture, most
// realistically); see edges_within_workspace's own comment for why that
// default degrades safely rather than misbehaving.
struct Rect {
  double x{0};
  double y{0};
  double width{0};
  double height{0};

  [[nodiscard]] double left() const { return x; }
  [[nodiscard]] double right() const { return x + width; }
  [[nodiscard]] double top() const { return y; }
  [[nodiscard]] double bottom() const { return y + height; }
};

inline Rect rect_of(const json::Value &container) {
  const json::Value *r = container.find("rect");
  if (!r || r->kind != json::Value::Kind::Object)
    return {};
  const json::Value *x = r->find("x");
  const json::Value *y = r->find("y");
  const json::Value *w = r->find("width");
  const json::Value *h = r->find("height");
  if (!x || !y || !w || !h || x->kind != json::Value::Kind::Number ||
      y->kind != json::Value::Kind::Number ||
      w->kind != json::Value::Kind::Number ||
      h->kind != json::Value::Kind::Number)
    return {};
  return Rect{x->number, y->number, w->number, h->number};
}

// A container that is a client window carries its X11 id as a positive
// number; the split/workspace/output containers around it carry null.
// Floating state and rect only -- a single container never has enough
// context (its siblings, its workspace's own bounds) to say which of its
// edges are actually part of the tiling arrangement; that is
// edges_within_workspace's job, over a whole workspace at once.
struct LeafInfo {
  uint32_t window{0};
  bool tiled{false};
  Rect rect;
};

inline std::optional<LeafInfo> leaf_info(const json::Value &container) {
  const json::Value *window = container.find("window");
  if (!window || window->kind != json::Value::Kind::Number ||
      window->number < 1.0 || window->number > 4294967295.0)
    return std::nullopt;

  LeafInfo info;
  info.window = static_cast<uint32_t>(window->number);
  const json::Value *floating = container.find("floating");
  // A container that does not say is treated as tiled, which is i3's own
  // default for a managed window.
  info.tiled = !floating || floating->kind != json::Value::Kind::String ||
               is_tiled(floating->string);
  info.rect = rect_of(container);
  return info;
}

// Whether a `window` event's `change` means the tiling layout may have
// changed -- for any of these, the one container the event itself carries
// is not enough to know the new state correctly (no sibling rects, no
// workspace bounds), so the only honest response is to ask for a fresh
// GET_TREE; see I3Watcher::Impl::request_refetch. focus/title/urgent/mark/
// fullscreen_mode change nothing about layout and are not this module's
// business.
inline bool window_event_may_change_tiling(std::string_view payload) {
  const auto doc = json::parse(payload);
  if (!doc)
    return false;

  const json::Value *change = doc->find("change");
  if (!change || change->kind != json::Value::Kind::String)
    return false;

  const std::string_view what = change->string;
  return what == "new" || what == "floating" || what == "move" ||
         what == "close";
}

// Two closed ranges on one axis overlap by more than a single point --
// touching only at a corner does not count as sharing an edge.
inline bool ranges_overlap(double a0, double a1, double b0, double b1) {
  return a0 < b1 && b0 < a1;
}

// How close two edges have to be to count as touching. i3 gaps (both
// `gaps inner`/`gaps outer` and the native `gaps` command) shrink each
// tiled container's reported rect by the configured amount, so two
// genuinely adjacent windows do not report numerically equal edges once
// gaps are on -- an exact-equality test would silently see "no neighbour"
// on every edge of every tiled window for any i3 config with gaps enabled,
// which is common. i3's IPC has no direct "current gap size" query (GET_
// CONFIG returns the raw config text, which single-session runtime
// changes via `gaps inner all set N` would not even be reflected in), so
// rather than add a second IPC round trip to approximate a value that can
// change without it, this is a fixed tolerance comfortably above any gap
// size anyone configures (typically single or low-double-digit pixels)
// and comfortably below any real window dimension.
inline constexpr double kEdgeTolerance = 128.0;

inline bool edges_touch(double a, double b) {
  return std::fabs(a - b) < kEdgeTolerance;
}

// Per-edge tiling for one workspace's worth of leaves: edge E of a tiled
// leaf is set when another tiled leaf's opposite edge is within
// kEdgeTolerance and the two overlap along the perpendicular axis (a
// neighbour), or when the leaf's own edge is within kEdgeTolerance of the
// *workspace's* corresponding edge (the outer boundary -- outer gaps
// shrink the workspace's own reported rect the same way, so the same
// tolerance test applies there too).
//
// The workspace boundary case is what makes this, for real i3 trees,
// always come out all four edges true together or all four false: i3's
// splits always fully partition their parent's rect among their children,
// so a tiled leaf's every edge borders either a sibling or the workspace
// boundary -- there is no third case where it borders neither. A lone
// tiled window filling its whole workspace, the single most common i3
// layout, is exactly the case this exists to get right: no neighbours on
// any side, but all four edges still correctly come out tiled via the
// boundary test, matching how i3 (and every tiling compositor's own
// handling of this, e.g. Sway's) has always treated it. The per-edge
// machinery stays real and independently useful -- see TiledEdges' own
// comment -- it is i3's own fully-partitioning layout model, not this
// code, that happens to never produce a genuine subset.
inline std::vector<TiledEdges>
edges_within_workspace(const std::vector<LeafInfo> &tiled_leaves,
                       const Rect &workspace_rect) {
  std::vector<TiledEdges> out;
  out.resize(tiled_leaves.size());
  for (std::size_t i = 0; i < tiled_leaves.size(); ++i) {
    const Rect &a = tiled_leaves[i].rect;
    TiledEdges &e = out[i];

    e.left = edges_touch(a.left(), workspace_rect.left());
    e.right = edges_touch(a.right(), workspace_rect.right());
    e.top = edges_touch(a.top(), workspace_rect.top());
    e.bottom = edges_touch(a.bottom(), workspace_rect.bottom());

    for (std::size_t j = 0; j < tiled_leaves.size(); ++j) {
      if (i == j)
        continue;
      const Rect &b = tiled_leaves[j].rect;
      if (!e.left && edges_touch(a.left(), b.right()) &&
          ranges_overlap(a.top(), a.bottom(), b.top(), b.bottom()))
        e.left = true;
      if (!e.right && edges_touch(a.right(), b.left()) &&
          ranges_overlap(a.top(), a.bottom(), b.top(), b.bottom()))
        e.right = true;
      if (!e.top && edges_touch(a.top(), b.bottom()) &&
          ranges_overlap(a.left(), a.right(), b.left(), b.right()))
        e.top = true;
      if (!e.bottom && edges_touch(a.bottom(), b.top()) &&
          ranges_overlap(a.left(), a.right(), b.left(), b.right()))
        e.bottom = true;
    }
  }
  return out;
}

// Every leaf (tiled or floating) directly under `node`, at any depth, not
// descending past a nested workspace (there should not be one, but a
// hostile or merely unusual tree must not be walked past 64 levels deep
// either way).
inline void collect_leaves(const json::Value &node,
                           std::vector<LeafInfo> &out, int depth) {
  if (depth > 64)
    return;
  if (auto info = leaf_info(node))
    out.push_back(*info);
  for (const char *key : {"nodes", "floating_nodes"}) {
    const json::Value *children = node.find(key);
    if (!children || children->kind != json::Value::Kind::Array)
      continue;
    for (const json::Value &child : children->items)
      collect_leaves(child, out, depth + 1);
  }
}

// Every workspace node in the tree, at any depth. A workspace does not
// nest inside another, so this does not recurse into one once found.
inline void collect_workspaces(const json::Value &node,
                               std::vector<const json::Value *> &out,
                               int depth) {
  if (depth > 64)
    return;
  if (const json::Value *type = node.find("type");
      type && type->kind == json::Value::Kind::String &&
      type->string == "workspace") {
    out.push_back(&node);
    return;
  }
  for (const char *key : {"nodes", "floating_nodes"}) {
    const json::Value *children = node.find(key);
    if (!children || children->kind != json::Value::Kind::Array)
      continue;
    for (const json::Value &child : children->items)
      collect_workspaces(child, out, depth + 1);
  }
}

// Every client window in a GET_TREE reply, tiled and floating alike, each
// with real per-edge tiling -- see edges_within_workspace. Adjacency is
// scoped to one workspace at a time deliberately: an invisible workspace's
// windows share the same on-screen rect bounds as whichever workspace *is*
// showing on that output (i3 does not move them off-screen in the tree),
// so comparing rects across workspaces would find numerically-overlapping
// windows that are not actually anywhere near each other on screen.
inline std::vector<TilingUpdate> tiling_from_tree(std::string_view payload) {
  std::vector<TilingUpdate> out;
  const auto doc = json::parse(payload);
  if (!doc)
    return out;

  std::vector<const json::Value *> workspaces;
  collect_workspaces(*doc, workspaces, 0);
  if (workspaces.empty())
    // No workspace-typed node found at all (a minimal/unusual tree, most
    // realistically a hand-written test fixture) -- scope to the whole
    // document rather than silently report nothing.
    workspaces.push_back(&*doc);

  for (const json::Value *ws : workspaces) {
    std::vector<LeafInfo> leaves;
    collect_leaves(*ws, leaves, 0);

    std::vector<LeafInfo> tiled_leaves;
    for (const auto &leaf : leaves)
      if (leaf.tiled)
        tiled_leaves.push_back(leaf);
    const auto edges = edges_within_workspace(tiled_leaves, rect_of(*ws));

    std::size_t next_tiled = 0;
    for (const auto &leaf : leaves) {
      TilingUpdate update;
      update.window = leaf.window;
      if (leaf.tiled)
        update.edges = edges[next_tiled++];
      // else: default-constructed TiledEdges, all false -- floating.
      out.push_back(update);
    }
  }
  return out;
}

} // namespace ww::i3::policy
