#pragma once

#include <wwindows/ports/resize_timer.hpp>

#include <chrono>
#include <map>
#include <utility>

namespace ww_test {

// Deterministic, manually advanced timer for state-machine tests. Advancing
// time executes due callbacks synchronously; callbacks may cancel or schedule
// further timers safely.
class FakeResizeTimer final : public ww::IResizeTimer {
public:
  [[nodiscard]] TimePoint now() const noexcept override { return now_; }

  TimerId schedule_at(TimePoint deadline, Callback callback) override {
    if (!callback)
      return 0;
    TimerId id = next_id_++;
    if (id == 0)
      id = next_id_++;
    timers_.emplace(id, Entry{deadline, std::move(callback)});
    return id;
  }

  void cancel(TimerId id) noexcept override {
    if (id)
      timers_.erase(id);
  }

  template <typename Rep, typename Period>
  void advance(std::chrono::duration<Rep, Period> delta) {
    now_ += std::chrono::duration_cast<Clock::duration>(delta);
    for (;;) {
      auto due = timers_.end();
      for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.deadline <= now_ &&
            (due == timers_.end() ||
             it->second.deadline < due->second.deadline ||
             (it->second.deadline == due->second.deadline &&
              it->first < due->first)))
          due = it;
      }
      if (due == timers_.end())
        break;
      Callback callback = std::move(due->second.callback);
      timers_.erase(due);
      if (callback)
        callback();
    }
  }

  [[nodiscard]] std::size_t pending_count() const noexcept {
    return timers_.size();
  }

private:
  struct Entry {
    TimePoint deadline;
    Callback callback;
  };
  TimePoint now_{};
  TimerId next_id_{1};
  std::map<TimerId, Entry> timers_;
};

} // namespace ww_test
