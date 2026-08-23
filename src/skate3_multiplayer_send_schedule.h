#pragma once

#include <chrono>

namespace skate3::multiplayer::schedule {

// A periodic latest-sample gate which retains the target-rate phase between
// worker ticks. Committing a late send advances directly to the first future
// deadline, so a stall never creates a burst of catch-up packets.
class PeriodicDeadline {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  [[nodiscard]] bool Due(TimePoint now) const {
    return !armed_ || now >= next_;
  }

  void Commit(TimePoint now, Clock::duration interval) {
    if (interval <= Clock::duration::zero()) {
      next_ = now;
      armed_ = true;
      return;
    }
    if (!armed_) {
      next_ = now + interval;
      armed_ = true;
      return;
    }

    next_ += interval;
    if (next_ <= now) {
      const auto skipped = (now - next_) / interval + 1;
      next_ += interval * skipped;
    }
  }

  void Reset() {
    next_ = {};
    armed_ = false;
  }

  [[nodiscard]] TimePoint next() const { return next_; }

 private:
  TimePoint next_{};
  bool armed_ = false;
};

}  // namespace skate3::multiplayer::schedule
