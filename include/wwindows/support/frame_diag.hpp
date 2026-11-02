#pragma once
// Frame-lifecycle diagnostics for the "window stops updating while the client
// still responds" investigation. Single-threaded (the event loop thread), so
// no locking. Counts, per X11 window, how far each frame got through the
// pipeline, keeps a short ring of the latest events, warns when a stage has
// work outstanding for too long, and dumps everything on SIGUSR1.
//
// Key 0 collects events that cannot be tied to a window (dmabuf imports,
// deferred wl_buffer releases, X errors, stranded events).

#include <wwindows/support/log.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace ww::diag {

enum class Ev : uint8_t {
  Commit,
  FcbQueued,
  FcbFired,
  Release,
  ShmSubmit,
  ShmDone,
  ShmPendingStored,
  ShmPendingReplaced,
  ShmPendingFlushed,
  PresentSubmit,
  PresentDone,
  RenderSubmit,
  RenderDone,
  ImportStart,
  ImportOk,
  ImportFail,
  XError,
  SnapshotFail,
  ScaleDrop,
  Stranded,
  Count_
};

inline constexpr std::array<const char *, static_cast<size_t>(Ev::Count_)>
    kNames = {"commit",       "fcb_queued",    "fcb_fired",   "release",
              "shm_submit",   "shm_done",      "shm_pend_set", "shm_pend_replaced",
              "shm_pend_flush", "present_submit", "present_done", "render_submit",
              "render_done",  "import_start",  "import_ok",   "import_fail",
              "x_error",      "snapshot_fail", "scale_drop",  "stranded_events"};

using Clock = std::chrono::steady_clock;

struct Pair {
  Ev submit;
  Ev done;
  Ev done2; // optional second completion kind (imports: ok + fail)
  const char *name;
  std::chrono::milliseconds limit;
};

inline constexpr std::array<Pair, 5> kPairs = {{
    {Ev::FcbQueued, Ev::FcbFired, Ev::FcbFired, "wl frame callback not fired",
     std::chrono::milliseconds{2000}},
    {Ev::ShmSubmit, Ev::ShmDone, Ev::ShmDone, "SHM put_image never completed",
     std::chrono::milliseconds{1500}},
    {Ev::PresentSubmit, Ev::PresentDone, Ev::PresentDone,
     "X Present never completed", std::chrono::milliseconds{1500}},
    {Ev::RenderSubmit, Ev::RenderDone, Ev::RenderDone,
     "X Render fence never returned", std::chrono::milliseconds{1500}},
    {Ev::ImportStart, Ev::ImportOk, Ev::ImportFail,
     "DRI3 import never resolved", std::chrono::milliseconds{1500}},
}};

struct Win {
  std::array<uint64_t, static_cast<size_t>(Ev::Count_)> n{};
  struct Entry {
    Clock::time_point at;
    Ev ev;
    uint32_t extra;
  };
  static constexpr size_t kRing = 48;
  std::array<Entry, kRing> ring{};
  size_t ring_pos{0};
  size_t ring_len{0};
  // Per pair: when the current wait began / last completion happened, and
  // when we last warned.
  std::array<Clock::time_point, kPairs.size()> progress{};
  std::array<Clock::time_point, kPairs.size()> warned{};
};

struct State {
  std::map<uint32_t, Win> wins;
  Clock::time_point start{Clock::now()};
};

inline State &state() {
  static State s;
  return s;
}

inline volatile std::sig_atomic_t g_dump_requested = 0;

inline void on_signal(int) { g_dump_requested = 1; }

inline void install_signal_handler() {
  static bool installed = false;
  if (installed)
    return;
  installed = true;
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  ::sigaction(SIGUSR1, &sa, nullptr);
}

inline uint64_t outstanding(const Win &w, const Pair &p) {
  const uint64_t s = w.n[static_cast<size_t>(p.submit)];
  uint64_t d = w.n[static_cast<size_t>(p.done)];
  if (p.done2 != p.done)
    d += w.n[static_cast<size_t>(p.done2)];
  return s > d ? s - d : 0;
}

inline void note(uint32_t window, Ev e, uint32_t extra = 0) {
  auto &w = state().wins[window];
  const auto now = Clock::now();
  // A wait begins when outstanding goes 0 -> 1.
  for (size_t i = 0; i < kPairs.size(); ++i) {
    if (kPairs[i].submit == e && outstanding(w, kPairs[i]) == 0)
      w.progress[i] = now;
  }
  ++w.n[static_cast<size_t>(e)];
  for (size_t i = 0; i < kPairs.size(); ++i) {
    if (kPairs[i].done == e || kPairs[i].done2 == e)
      w.progress[i] = now;
  }
  w.ring[w.ring_pos] = Win::Entry{now, e, extra};
  w.ring_pos = (w.ring_pos + 1) % Win::kRing;
  if (w.ring_len < Win::kRing)
    ++w.ring_len;
}

inline long ms_since(Clock::time_point t, Clock::time_point now) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count());
}

inline void dump_ring(uint32_t window, const Win &w, size_t max_entries,
                      Clock::time_point now) {
  const size_t count = std::min(max_entries, w.ring_len);
  for (size_t i = count; i > 0; --i) {
    const size_t idx = (w.ring_pos + Win::kRing - i) % Win::kRing;
    const auto &e = w.ring[idx];
    WW_LOG_WARN("[diag]   win={} -{}ms {} extra={}", window,
                ms_since(e.at, now), kNames[static_cast<size_t>(e.ev)], e.extra);
  }
}

inline void dump_all(const char *why) {
  const auto now = Clock::now();
  auto &s = state();
  WW_LOG_WARN("[diag] ===== state dump ({}) uptime={}s windows={} =====", why,
              ms_since(s.start, now) / 1000, s.wins.size());
  for (const auto &[window, w] : s.wins) {
    std::string line;
    for (size_t i = 0; i < w.n.size(); ++i) {
      if (w.n[i] == 0)
        continue;
      line += std::string{kNames[i]} + "=" + std::to_string(w.n[i]) + " ";
    }
    WW_LOG_WARN("[diag] win={} {}", window, line);
    for (size_t i = 0; i < kPairs.size(); ++i) {
      const auto out = outstanding(w, kPairs[i]);
      if (out > 0)
        WW_LOG_WARN("[diag] win={} OUTSTANDING {}: {} (waiting {}ms)", window,
                    kPairs[i].name, out, ms_since(w.progress[i], now));
    }
    dump_ring(window, w, 24, now);
  }
  WW_LOG_WARN("[diag] ===== end dump =====");
}

// Called periodically from the event loop. Warns (throttled) about stalls and
// handles SIGUSR1 dump requests.
inline void tick() {
  const auto now = Clock::now();
  auto &s = state();
  for (auto &[window, w] : s.wins) {
    for (size_t i = 0; i < kPairs.size(); ++i) {
      const auto out = outstanding(w, kPairs[i]);
      if (out == 0)
        continue;
      if (now - w.progress[i] < kPairs[i].limit)
        continue;
      if (w.warned[i] != Clock::time_point{} &&
          now - w.warned[i] < std::chrono::seconds{5})
        continue;
      w.warned[i] = now;
      WW_LOG_WARN("[diag] STALL win={}: {} ({} outstanding, no progress for "
                  "{}ms) -- last events follow",
                  window, kPairs[i].name, out, ms_since(w.progress[i], now));
      dump_ring(window, w, 12, now);
    }
  }
  if (g_dump_requested) {
    g_dump_requested = 0;
    dump_all("SIGUSR1");
  }
}

} // namespace ww::diag
