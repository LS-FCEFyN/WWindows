#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace ww {

// Injectable one-shot timer/clock used for resize-settle decisions. Production
// uses EventLoop; tests can provide a manually advanced clock that invokes due
// callbacks synchronously, without sleeping or depending on wall-clock timing.
class IResizeTimer {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using TimerId = uint64_t;
  using Callback = std::function<void()>;

  virtual ~IResizeTimer() = default;
  [[nodiscard]] virtual TimePoint now() const noexcept = 0;
  virtual TimerId schedule_at(TimePoint deadline, Callback callback) = 0;
  virtual void cancel(TimerId id) noexcept = 0;
};

} // namespace ww
