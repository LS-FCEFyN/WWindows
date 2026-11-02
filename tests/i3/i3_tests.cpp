// Tests for the i3 adapter: the wire/JSON policy against literal input, and
// I3Watcher against a fake i3 -- a real unix socket in a temp directory that
// speaks the framing back, so the connect / subscribe / snapshot / event
// path is exercised end to end without needing i3 itself.

#include "../support/test.hpp"

#include <wwindows/adapters/i3/i3_watcher.hpp>
#include <wwindows/adapters/i3/json.hpp>
#include <wwindows/adapters/i3/policy/i3_wire.hpp>
#include <wwindows/core/event_loop.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace i3p = ww::i3::policy;

// --- JSON -------------------------------------------------------------------

WW_TEST_CASE(json_reads_the_shapes_i3_sends) {
  const auto v = ww::json::parse(
      R"({"a": 1, "b": [true, false, null], "c": {"d": "x\ny\u00e9"}, "e": -2.5e1})");
  WW_TEST_CHECK(v.has_value());
  if (!v)
    return;
  WW_TEST_CHECK_EQ(v->find("a")->number, 1.0);
  WW_TEST_CHECK_EQ(v->find("b")->items.size(), std::size_t{3});
  WW_TEST_CHECK(v->find("b")->items[0].boolean);
  // \n and \u00e9 (UTF-8: C3 A9) both decoded.
  WW_TEST_CHECK(v->find("c")->find("d")->string == "x\ny\xC3\xA9");
  WW_TEST_CHECK_EQ(v->find("e")->number, -25.0);
  WW_TEST_CHECK(v->find("missing") == nullptr);
}

WW_TEST_CASE(json_rejects_malformed_input_instead_of_guessing) {
  for (const char *bad : {"", "{", "[1,]", R"({"a":})", R"({"a" 1})", "tru",
                          R"("unterminated)", "{} trailing", R"({"a":1,})",
                          "[\"raw\ncontrol\"]"})
    WW_TEST_CHECK(!ww::json::parse(bad).has_value());
}

WW_TEST_CASE(json_caps_nesting_depth_rather_than_overflowing_the_stack) {
  const std::string deep(100000, '[');
  WW_TEST_CHECK(!ww::json::parse(deep).has_value());
}

// --- framing ----------------------------------------------------------------

WW_TEST_CASE(a_frame_round_trips_through_encode_and_take) {
  std::string buf = i3p::encode_message(i3p::kMsgGetTree, "{}");
  i3p::Frame f;
  WW_TEST_CHECK(i3p::take_frame(buf, f) == i3p::FrameStatus::Ready);
  WW_TEST_CHECK_EQ(f.type, i3p::kMsgGetTree);
  WW_TEST_CHECK(f.payload == "{}");
  WW_TEST_CHECK(buf.empty());
}

WW_TEST_CASE(a_partial_frame_waits_and_two_frames_come_out_in_order) {
  const std::string a = i3p::encode_message(1, "first");
  const std::string b = i3p::encode_message(2, "second");
  std::string buf = a + b.substr(0, 5); // second one cut off mid-header
  i3p::Frame f;
  WW_TEST_CHECK(i3p::take_frame(buf, f) == i3p::FrameStatus::Ready);
  WW_TEST_CHECK(f.payload == "first");
  WW_TEST_CHECK(i3p::take_frame(buf, f) == i3p::FrameStatus::Incomplete);
  buf += b.substr(5);
  WW_TEST_CHECK(i3p::take_frame(buf, f) == i3p::FrameStatus::Ready);
  WW_TEST_CHECK(f.payload == "second");
}

WW_TEST_CASE(a_stream_that_is_not_i3_is_corrupt_not_incomplete) {
  std::string buf = "HTTP/1.1 200 OK\r\n\r\n";
  i3p::Frame f;
  WW_TEST_CHECK(i3p::take_frame(buf, f) == i3p::FrameStatus::Corrupt);

  // A valid magic with an absurd length must not be waited on for 4 GiB.
  std::string huge = "i3-ipc";
  const uint32_t len = 0xffffffffu;
  const uint32_t type = 0;
  huge.append(reinterpret_cast<const char *>(&len), 4);
  huge.append(reinterpret_cast<const char *>(&type), 4);
  WW_TEST_CHECK(i3p::take_frame(huge, f) == i3p::FrameStatus::Corrupt);
}

// --- window events ----------------------------------------------------------

namespace {

std::string container(uint32_t window, const char *floating,
                      const char *rect = nullptr) {
  const std::string r =
      rect ? (std::string{R"(,"rect":)"} + rect) : std::string{};
  return std::string{R"({"id":1,"type":"con","window":)"} +
         (window ? std::to_string(window) : "null") +
         R"(,"floating":")" + floating + R"(")" + r +
         R"(,"nodes":[],"floating_nodes":[]})";
}

std::string window_event(const char *change, uint32_t window,
                         const char *floating) {
  return std::string{R"({"change":")"} + change + R"(","container":)" +
         container(window, floating) + "}";
}

std::string rect_json(long long x, long long y, long long w, long long h) {
  return "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
         ",\"width\":" + std::to_string(w) + ",\"height\":" +
         std::to_string(h) + "}";
}

} // namespace

// A `window` event's payload carries only the one changed container's own
// JSON -- never its siblings, never its workspace's bounds -- so it is
// never enough on its own to say which edges (if any) are now part of the
// tiling arrangement. The only question this file's window-event handling
// answers from the event alone is "does this mean the tiling layout may
// have changed" (window_event_may_change_tiling); the actual new state
// always comes from a fresh GET_TREE afterward -- see the I3Watcher-level
// tests below for that round trip end to end.

WW_TEST_CASE(new_floating_move_and_close_may_change_tiling) {
  for (const char *change : {"new", "floating", "move", "close"})
    WW_TEST_CHECK(i3p::window_event_may_change_tiling(
        window_event(change, 42, "auto_off")));
}

WW_TEST_CASE(events_that_cannot_change_tiling_say_so) {
  for (const char *change :
       {"focus", "title", "urgent", "mark", "fullscreen_mode"})
    WW_TEST_CHECK(!i3p::window_event_may_change_tiling(
        window_event(change, 42, "auto_off")));
}

WW_TEST_CASE(malformed_or_changeless_payloads_say_no_rather_than_crash) {
  WW_TEST_CHECK(!i3p::window_event_may_change_tiling("not json"));
  WW_TEST_CHECK(!i3p::window_event_may_change_tiling(R"({"container":{}})"));
  WW_TEST_CHECK(!i3p::window_event_may_change_tiling(R"({"change":123})"));
}

// --- GET_TREE ---------------------------------------------------------------

namespace {

// root > output > workspace holding one tiled window in a split, and one
// floating window that i3 wraps in a floating_con.
std::string sample_tree(const char *leaf111_floating = "auto_off") {
  return std::string{R"({"id":1,"type":"root","window":null,"nodes":[)"} +
         R"({"id":2,"type":"output","window":null,"nodes":[)" +
         R"({"id":3,"type":"workspace","window":null,"nodes":[)" +
         R"({"id":4,"type":"con","window":null,"floating":"auto_off","nodes":[)" +
         container(111, leaf111_floating) + R"(],"floating_nodes":[]},)" +
         container(112, "user_off") + R"(],"floating_nodes":[)" +
         R"({"id":5,"type":"floating_con","window":null,"floating":"user_on","nodes":[)" +
         container(222, "user_on") + R"(],"floating_nodes":[]})" +
         R"(]}],"floating_nodes":[]}],"floating_nodes":[]})";
}

} // namespace

WW_TEST_CASE(get_tree_finds_tiled_and_floating_windows_at_any_depth) {
  const auto updates = i3p::tiling_from_tree(sample_tree());
  std::map<uint32_t, ww::TiledEdges> by_window;
  for (const auto &u : updates)
    by_window[u.window] = u.edges;

  WW_TEST_CHECK_EQ(by_window.size(), std::size_t{3});
  // sample_tree() gives none of its containers a "rect" at all, which
  // rect_of() reads back as Rect{0,0,0,0} for every one of them including
  // the workspace itself -- 111 and 112 each trivially touch that
  // (equally zero) workspace boundary on every side, the "no real
  // geometry available" degradation std::vector<TiledEdges>'s own comment
  // describes.
  WW_TEST_CHECK(by_window.at(111).any());   // nested in a split
  WW_TEST_CHECK(by_window.at(112).any());   // directly under the workspace
  WW_TEST_CHECK(!by_window.at(222).any());  // under floating_nodes
}

// --- per-edge adjacency, directly: edges_within_workspace -------------------
//
// Unit tests of the geometry itself, bypassing JSON entirely -- the
// clearest way to pin down what counts as "touching" independent of tree
// shape.

WW_TEST_CASE(two_side_by_side_windows_touch_on_every_edge) {
  // A 1920x1080 workspace split into two 960-wide columns, no gaps: the
  // single most common i3 layout after "one window alone".
  const i3p::Rect workspace{0, 0, 1920, 1080};
  const std::vector<i3p::LeafInfo> leaves = {
      {.window = 1, .tiled = true, .rect = {0, 0, 960, 1080}},
      {.window = 2, .tiled = true, .rect = {960, 0, 960, 1080}},
  };
  const auto edges = i3p::edges_within_workspace(leaves, workspace);
  WW_TEST_CHECK_EQ(edges.size(), std::size_t{2});
  // Left window: left/top/bottom via the workspace boundary, right via
  // its neighbour.
  WW_TEST_CHECK(edges[0].left);
  WW_TEST_CHECK(edges[0].right);
  WW_TEST_CHECK(edges[0].top);
  WW_TEST_CHECK(edges[0].bottom);
  // Right window: the mirror image.
  WW_TEST_CHECK(edges[1].left);
  WW_TEST_CHECK(edges[1].right);
  WW_TEST_CHECK(edges[1].top);
  WW_TEST_CHECK(edges[1].bottom);
}

WW_TEST_CASE(a_lone_tiled_window_touches_the_workspace_boundary_on_every_side) {
  // The actually-common case: one window, no neighbours anywhere, still
  // correctly tiled on all four sides via the workspace boundary alone.
  const i3p::Rect workspace{0, 0, 1920, 1080};
  const std::vector<i3p::LeafInfo> leaves = {
      {.window = 1, .tiled = true, .rect = {0, 0, 1920, 1080}},
  };
  const auto edges = i3p::edges_within_workspace(leaves, workspace);
  WW_TEST_CHECK(edges[0].left);
  WW_TEST_CHECK(edges[0].right);
  WW_TEST_CHECK(edges[0].top);
  WW_TEST_CHECK(edges[0].bottom);
}

WW_TEST_CASE(a_small_gap_still_counts_as_touching_but_an_implausible_one_does_not) {
  const i3p::Rect workspace{0, 0, 1920, 1080};

  // 10px gap on each side of the split -- i3's own `gaps` feature,
  // comfortably under kEdgeTolerance. This is the whole reason
  // kEdgeTolerance exists: an exact-equality test would see "no
  // neighbour" here and misreport every tiled window's shadow/corner
  // state for any i3 config with gaps turned on.
  const std::vector<i3p::LeafInfo> gapped = {
      {.window = 1, .tiled = true, .rect = {0, 0, 955, 1080}},
      {.window = 2, .tiled = true, .rect = {965, 0, 955, 1080}},
  };
  const auto touching = i3p::edges_within_workspace(gapped, workspace);
  WW_TEST_CHECK(touching[0].right);
  WW_TEST_CHECK(touching[1].left);

  // A gap no real i3 config would produce between two windows that are
  // otherwise each other's only horizontal neighbour, confirming the
  // tolerance has a ceiling rather than matching literally anything.
  // Window 2 is also short of the workspace's own right edge by more than
  // the tolerance, so neither of its left/right edges comes out tiled.
  const std::vector<i3p::LeafInfo> far_apart = {
      {.window = 1, .tiled = true, .rect = {0, 0, 500, 1080}},
      {.window = 2, .tiled = true, .rect = {1200, 0, 500, 1080}},
  };
  const auto apart = i3p::edges_within_workspace(far_apart, workspace);
  WW_TEST_CHECK(apart[0].left);   // still at the workspace's own left edge
  WW_TEST_CHECK(!apart[0].right); // neighbour is 700px away
  WW_TEST_CHECK(!apart[1].left);  // same neighbour, same 700px
  WW_TEST_CHECK(!apart[1].right); // workspace right (1920) is 220px further
}

WW_TEST_CASE(touching_x_coordinates_without_y_overlap_do_not_count) {
  // Two windows whose edges line up on one axis but occupy disjoint
  // ranges on the other -- e.g. one in a top-left split, the other in an
  // unrelated bottom-right one -- must not be treated as sharing that
  // edge just because the coordinate happens to match.
  const i3p::Rect workspace{0, 0, 1920, 1080};
  const std::vector<i3p::LeafInfo> leaves = {
      {.window = 1, .tiled = true, .rect = {0, 0, 960, 500}},
      {.window = 2, .tiled = true, .rect = {960, 600, 960, 480}},
  };
  const auto edges = i3p::edges_within_workspace(leaves, workspace);
  WW_TEST_CHECK(!edges[0].right);
  WW_TEST_CHECK(!edges[1].left);
}

WW_TEST_CASE(adjacency_never_crosses_a_workspace_boundary) {
  // Workspace A's window 501 occupies only the left half of A's own
  // (artificially narrow) rect; workspace B's window 502 sits exactly
  // where 501's missing right half would be -- numerically adjacent if
  // ever compared directly, which really can happen (i3 does not move an
  // invisible workspace's coordinates off-screen, so two workspaces on
  // one output can report overlapping rects). A real i3 tree never
  // actually leaves a workspace half-filled like this -- splits always
  // fully partition -- so this is a synthetic probe of the scoping
  // itself, not a layout i3 would produce.
  const std::string ws_a =
      std::string{R"({"id":10,"type":"workspace","window":null,"rect":)"} +
      rect_json(0, 0, 960, 1080) + R"(,"nodes":[)" +
      std::string{R"({"id":501,"type":"con","window":501,"floating":)"
                  R"("auto_off","rect":)"} +
      rect_json(0, 0, 960, 1080) +
      R"(,"nodes":[],"floating_nodes":[]})" +
      R"(],"floating_nodes":[]})";
  const std::string ws_b =
      std::string{R"({"id":20,"type":"workspace","window":null,"rect":)"} +
      rect_json(0, 0, 1920, 1080) + R"(,"nodes":[)" +
      std::string{R"({"id":502,"type":"con","window":502,"floating":)"
                  R"("auto_off","rect":)"} +
      rect_json(960, 0, 960, 1080) +
      R"(,"nodes":[],"floating_nodes":[]})" +
      R"(],"floating_nodes":[]})";
  const std::string tree =
      std::string{R"({"id":1,"type":"root","window":null,"nodes":[)"} +
      R"({"id":2,"type":"output","window":null,"nodes":[)" + ws_a + "," +
      ws_b + R"(],"floating_nodes":[]})" + R"(],"floating_nodes":[]})";

  const auto updates = i3p::tiling_from_tree(tree);
  std::map<uint32_t, ww::TiledEdges> by_window;
  for (const auto &u : updates)
    by_window[u.window] = u.edges;

  // 501 fills the whole of its own (narrower) workspace A, so this comes
  // out true regardless of scoping -- via A's own boundary either way.
  WW_TEST_CHECK(by_window.at(501).right);
  // The real check: unscoped, 502 would see 501 as a left neighbour
  // (both edges sit at x=960) in addition to incorrectly measuring
  // against A's narrower rect. Correctly scoped, 502 is compared only
  // against B's own (wider) rect, where it occupies exactly the right
  // half with nothing at all to its left.
  WW_TEST_CHECK(!by_window.at(502).left);
  WW_TEST_CHECK(by_window.at(502).right);
}

// --- I3Watcher against a fake i3 --------------------------------------------

namespace {

struct FakeI3 {
  std::filesystem::path dir;
  std::string path;
  int listener{-1};
  int peer{-1};
  std::string pending; // bytes received from the watcher, not yet consumed

  FakeI3() {
    char tmpl[] = "/tmp/ww-i3-test-XXXXXX";
    dir = ::mkdtemp(tmpl);
    path = (dir / "ipc.sock").string();

    listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0)
      std::abort();
  }
  ~FakeI3() {
    if (peer >= 0)
      ::close(peer);
    ::close(listener);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  // The watcher's connect() has already completed into the backlog.
  void accept_client() { peer = ::accept(listener, nullptr, nullptr); }

  // Reads one whole request the watcher sent.
  i3p::Frame read_request() {
    // `pending` outlives the call: both requests usually arrive in one
    // recv(), and the second must still be there for the next call.
    i3p::Frame f;
    char c[512];
    while (i3p::take_frame(pending, f) != i3p::FrameStatus::Ready) {
      const ssize_t n = ::recv(peer, c, sizeof(c), 0);
      if (n <= 0)
        std::abort();
      pending.append(c, static_cast<std::size_t>(n));
    }
    return f;
  }

  void send(uint32_t type, const std::string &payload) {
    const std::string m = i3p::encode_message(type, payload);
    if (::send(peer, m.data(), m.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(m.size()))
      std::abort();
  }
};

struct Reports {
  std::vector<std::pair<uint32_t, ww::TiledEdges>> seen;

  // The most recent report for `win`, or nullptr if it was not reported.
  [[nodiscard]] const ww::TiledEdges *last(uint32_t win) const {
    const ww::TiledEdges *found = nullptr;
    for (const auto &[w, e] : seen)
      if (w == win)
        found = &e;
    return found;
  }
};

} // namespace

WW_TEST_CASE(the_watcher_subscribes_to_window_events_and_asks_for_the_tree) {
  auto loop = ww::EventLoop::create();
  WW_TEST_CHECK(loop.has_value());
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  WW_TEST_CHECK(w.has_value());
  if (!w)
    return;
  i3.accept_client();

  const auto sub = i3.read_request();
  WW_TEST_CHECK_EQ(sub.type, i3p::kMsgSubscribe);
  WW_TEST_CHECK(sub.payload == R"(["window"])");
  // Subscribe first, snapshot second: see I3Watcher::connect.
  const auto tree = i3.read_request();
  WW_TEST_CHECK_EQ(tree.type, i3p::kMsgGetTree);
}

WW_TEST_CASE(the_watcher_reports_the_snapshot_then_live_changes) {
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  WW_TEST_CHECK(w.has_value());
  if (!w)
    return;
  Reports r;
  (*w)->set_tiling_callback(
      [&](uint32_t win, ww::TiledEdges e) { r.seen.emplace_back(win, e); });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();

  i3.send(i3p::kMsgSubscribe, R"({"success":true})");
  i3.send(i3p::kMsgGetTree, sample_tree());
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(r.seen.size(), std::size_t{3});

  // A relevant event triggers a fresh GET_TREE rather than reporting
  // anything itself -- see window_event_may_change_tiling's own comment --
  // so nothing is seen until that re-fetch's reply actually arrives.
  r.seen.clear();
  i3.send(i3p::kEventWindow, window_event("floating", 111, "user_on"));
  (*w)->handle_readable();
  WW_TEST_CHECK(r.seen.empty());

  const auto refetch1 = i3.read_request();
  WW_TEST_CHECK_EQ(refetch1.type, i3p::kMsgGetTree);
  i3.send(i3p::kMsgGetTree, sample_tree(/*leaf111_floating=*/"user_on"));
  (*w)->handle_readable();
  // Level-triggered: every window in the snapshot is reported, so the
  // receiver sees 111's new (floating) state alongside the unchanged others.
  WW_TEST_CHECK_EQ(r.seen.size(), std::size_t{3});
  WW_TEST_CHECK(r.last(111) != nullptr);
  if (r.last(111))
    WW_TEST_CHECK(!r.last(111)->any());

  // Same state again (i3 also emits `move` for plain workspace moves): the
  // re-fetch still happens -- the event alone cannot say nothing changed --
  // and the unchanged tree is reported again, identically. The watcher keeps
  // no memory of what it said; SurfaceManager is what ignores an unchanged
  // report (see set_tiled_edges_sends_exactly_the_edges_given).
  r.seen.clear();
  i3.send(i3p::kEventWindow, window_event("move", 111, "user_on"));
  (*w)->handle_readable();
  const auto refetch2 = i3.read_request();
  WW_TEST_CHECK_EQ(refetch2.type, i3p::kMsgGetTree);
  i3.send(i3p::kMsgGetTree, sample_tree(/*leaf111_floating=*/"user_on"));
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(r.seen.size(), std::size_t{3});
  WW_TEST_CHECK(r.last(111) != nullptr);
  if (r.last(111))
    WW_TEST_CHECK(!r.last(111)->any());

  // A window that closed and comes back with the same id, as two events
  // in one batch -- coalesced into a single re-fetch (see
  // events_in_one_batch_coalesce_into_a_single_refetch for that in
  // isolation) whose reply, by the time i3 answers it, simply shows the
  // window existing again.
  r.seen.clear();
  i3.send(i3p::kEventWindow, window_event("close", 111, "user_on"));
  i3.send(i3p::kEventWindow, window_event("new", 111, "auto_off"));
  (*w)->handle_readable();
  const auto refetch3 = i3.read_request();
  WW_TEST_CHECK_EQ(refetch3.type, i3p::kMsgGetTree);
  i3.send(i3p::kMsgGetTree, sample_tree(/*leaf111_floating=*/"auto_off"));
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(r.seen.size(), std::size_t{3});
  WW_TEST_CHECK(r.last(111) != nullptr);
  if (r.last(111))
    WW_TEST_CHECK(r.last(111)->any());
}

WW_TEST_CASE(events_in_one_batch_coalesce_into_a_single_refetch) {
  // Real i3 commonly emits several events for one user action (closing a
  // window retiles its neighbours too, each getting its own event) -- a
  // request per event would mean a burst of identical "the tree, now"
  // questions in flight at once for no reason.
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  Reports r;
  (*w)->set_tiling_callback(
      [&](uint32_t win, ww::TiledEdges e) { r.seen.emplace_back(win, e); });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();
  i3.send(i3p::kMsgSubscribe, R"({"success":true})");
  i3.send(i3p::kMsgGetTree, sample_tree());
  (*w)->handle_readable();
  r.seen.clear();

  i3.send(i3p::kEventWindow, window_event("new", 301, "auto_off"));
  i3.send(i3p::kEventWindow, window_event("move", 111, "auto_off"));
  i3.send(i3p::kEventWindow, window_event("close", 112, "auto_off"));
  (*w)->handle_readable();
  WW_TEST_CHECK(r.seen.empty()); // waiting on the one re-fetch, not reported yet

  const auto refetch = i3.read_request();
  WW_TEST_CHECK_EQ(refetch.type, i3p::kMsgGetTree);
  // All three sends land in one recv() the same way the two startup
  // requests already do (see read_request()'s own comment) -- a second,
  // uncoalesced request would show up here too.
  WW_TEST_CHECK(i3.pending.empty());

  // A reply genuinely reflecting one of the three claimed changes (301
  // newly present) -- not sample_tree() again unmodified, which would
  // make the round trip look like it worked while actually testing
  // nothing: 301 appearing is what proves the reply was acted on. 112 is
  // absent from it (closed), so it must not be reported at all.
  const std::string reply_tree =
      std::string{R"({"id":1,"type":"root","window":null,"nodes":[)"} +
      R"({"id":2,"type":"output","window":null,"nodes":[)" +
      R"({"id":3,"type":"workspace","window":null,"nodes":[)" +
      container(111, "auto_off") + "," + container(301, "auto_off") +
      R"(],"floating_nodes":[]}],"floating_nodes":[]}],"floating_nodes":[]})";
  i3.send(i3p::kMsgGetTree, reply_tree);
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(r.seen.size(), std::size_t{2});
  WW_TEST_CHECK(r.last(301) != nullptr);
  WW_TEST_CHECK(r.last(111) != nullptr);
  WW_TEST_CHECK(r.last(112) == nullptr);
}

WW_TEST_CASE(events_split_across_reads_are_reassembled) {
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  Reports r;
  (*w)->set_tiling_callback(
      [&](uint32_t win, ww::TiledEdges e) { r.seen.emplace_back(win, e); });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();

  const std::string m =
      i3p::encode_message(i3p::kEventWindow, window_event("new", 7, "auto_off"));
  ::send(i3.peer, m.data(), 9, MSG_NOSIGNAL);
  (*w)->handle_readable();
  // Still mid-header: must not have acted on a frame it does not have yet.
  WW_TEST_CHECK(r.seen.empty());
  ::send(i3.peer, m.data() + 9, m.size() - 9, MSG_NOSIGNAL);
  (*w)->handle_readable();
  // Now complete: the event was processed, which here means a GET_TREE
  // re-fetch went out -- see window_event_may_change_tiling's own
  // comment for why a direct report was never on the table for this.
  const auto refetch = i3.read_request();
  WW_TEST_CHECK_EQ(refetch.type, i3p::kMsgGetTree);
  i3.send(i3p::kMsgGetTree, sample_tree());
  (*w)->handle_readable();
  WW_TEST_CHECK(!r.seen.empty());
}

WW_TEST_CASE(a_dropped_connection_stops_the_watcher_without_crashing) {
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  Reports r;
  (*w)->set_tiling_callback(
      [&](uint32_t win, ww::TiledEdges e) { r.seen.emplace_back(win, e); });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();

  // An event (one that cannot itself trigger an outbound re-fetch write,
  // to keep this test about EOF handling specifically rather than also
  // racing a write against the peer closing) and then EOF in one go.
  i3.send(i3p::kEventWindow, window_event("focus", 9, "auto_off"));
  ::close(i3.peer);
  i3.peer = -1;
  (*w)->handle_readable();
  WW_TEST_CHECK(r.seen.empty());

  (*w)->handle_readable(); // after disconnect: harmless no-op
  WW_TEST_CHECK(r.seen.empty());
}

WW_TEST_CASE(a_dropped_connection_fires_the_disconnected_callback_exactly_once) {
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  int disconnected = 0;
  (*w)->set_disconnected_callback([&] { ++disconnected; });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();

  ::close(i3.peer);
  i3.peer = -1;
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(disconnected, 1);

  // Already torn down: calling handle_readable() again must not fire it
  // a second time.
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(disconnected, 1);
}

WW_TEST_CASE(a_malformed_stream_also_fires_the_disconnected_callback) {
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  int disconnected = 0;
  (*w)->set_disconnected_callback([&] { ++disconnected; });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();

  // Not a valid i3 IPC frame at all -- the "not an i3 IPC stream" path,
  // distinct from a clean close.
  const char garbage[] = "not-an-i3-frame";
  ::send(i3.peer, garbage, sizeof(garbage), MSG_NOSIGNAL);
  (*w)->handle_readable();
  WW_TEST_CHECK_EQ(disconnected, 1);
}

WW_TEST_CASE(the_disconnected_callback_may_destroy_the_watcher) {
  // The documented contract (see set_disconnected_callback): the callback
  // runs after the watcher has torn itself down but is free to go one
  // step further and destroy the I3Watcher itself, e.g. by resetting the
  // unique_ptr that owns it, as a composition root reconnecting would.
  // The interesting thing being tested is that doing so from inside the
  // callback does not crash or corrupt anything -- run under
  // WWINDOWS_SANITIZE=ON, ASan would catch a use-after-free here even if
  // this assertion did not.
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  auto owner = std::make_unique<std::unique_ptr<ww::I3Watcher>>(std::move(*w));
  bool destroyed_from_callback = false;
  (*owner)->set_disconnected_callback([&] {
    owner->reset();
    destroyed_from_callback = true;
  });
  i3.accept_client();
  (void)i3.read_request();
  (void)i3.read_request();

  ::close(i3.peer);
  i3.peer = -1;
  (*owner)->handle_readable();

  WW_TEST_CHECK(destroyed_from_callback);
  WW_TEST_CHECK(*owner == nullptr);
}

WW_TEST_CASE(an_ordinary_destruction_does_not_fire_the_disconnected_callback) {
  // The callback fires only when the watcher itself detects the
  // connection is gone, never as a side effect of WWindows shutting down
  // normally and simply destroying a still-live I3Watcher.
  auto loop = ww::EventLoop::create();
  FakeI3 i3;
  auto w = ww::I3Watcher::connect(**loop, i3.path);
  if (!w)
    return WW_TEST_CHECK(false);
  int disconnected = 0;
  (*w)->set_disconnected_callback([&] { ++disconnected; });
  i3.accept_client();

  w->reset();
  WW_TEST_CHECK_EQ(disconnected, 0);
}

WW_TEST_CASE(connecting_to_a_missing_socket_is_an_error_not_a_crash) {
  auto loop = ww::EventLoop::create();
  auto w = ww::I3Watcher::connect(**loop, "/nonexistent/ww-i3.sock");
  WW_TEST_CHECK(!w.has_value());
  auto too_long = ww::I3Watcher::connect(**loop, std::string(300, 'x'));
  WW_TEST_CHECK(!too_long.has_value());
}
