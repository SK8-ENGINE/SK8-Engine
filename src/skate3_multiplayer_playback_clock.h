#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace skate3::multiplayer::playback {

// Converts a noisy ideal sender-time cursor into a monotonic presentation
// cursor. Network jitter estimates and clock-offset refinement are allowed to
// change the desired buffering delay, but those changes must not directly
// jump or rewind a visible remote skater.
class PresentationClock {
 public:
  // Timing-estimator changes are corrected at no more than 2.5% of elapsed
  // real time. This is intentionally much slower than ordinary skater
  // acceleration: the cursor absorbs small network-clock corrections without
  // making the remote player visibly surge forward or pull backward.
  static constexpr std::int64_t kCorrectionDivisor = 40;

  std::int64_t Advance(std::int64_t local_time_us,
                       std::int64_t ideal_sender_time_us) {
    return AdvanceBounded(
        local_time_us, ideal_sender_time_us,
        std::numeric_limits<std::int64_t>::max());
  }

  // Advances the presentation cursor without allowing a receiver scheduling
  // stall to run it beyond the newest interval that can actually be
  // interpolated. The upper bound must itself be monotonic; an older packet
  // can never rewind a pose that was already displayed.
  std::int64_t AdvanceBounded(
      std::int64_t local_time_us,
      std::int64_t ideal_sender_time_us,
      std::int64_t maximum_sender_time_us) {
    if (!valid_) {
      valid_ = true;
      last_local_time_us_ = local_time_us;
      target_sender_time_us_ =
          std::min(ideal_sender_time_us, maximum_sender_time_us);
      ideal_error_us_ = SaturatingSubtract(
          ideal_sender_time_us, target_sender_time_us_);
      applied_correction_us_ = 0;
      return target_sender_time_us_;
    }

    const std::int64_t elapsed_us =
        std::max<std::int64_t>(
            0, local_time_us - last_local_time_us_);
    last_elapsed_us_ = elapsed_us;
    last_local_time_us_ =
        std::max(last_local_time_us_, local_time_us);

    // Advance at local real time, then converge on the desired buffer depth
    // at no more than 2.5 percent speed change. This absorbs delay-estimator
    // noise without creating visible timestamp steps or backwards motion.
    const std::int64_t natural_target_us =
        SaturatingAdd(target_sender_time_us_, elapsed_us);
    const std::int64_t desired_correction_us =
        SaturatingSubtract(
            ideal_sender_time_us, natural_target_us);
    const std::int64_t maximum_correction_us =
        elapsed_us / kCorrectionDivisor;
    applied_correction_us_ =
        std::clamp(
            desired_correction_us,
            -maximum_correction_us,
            maximum_correction_us);
    const std::int64_t proposed_target_us =
        SaturatingAdd(
            natural_target_us, applied_correction_us_);
    const std::int64_t monotonic_maximum_us =
        std::max(
            target_sender_time_us_, maximum_sender_time_us);
    target_sender_time_us_ = std::clamp(
        proposed_target_us, target_sender_time_us_,
        monotonic_maximum_us);
    ideal_error_us_ =
        SaturatingSubtract(
            ideal_sender_time_us, target_sender_time_us_);
    return target_sender_time_us_;
  }

  void Reset() {
    *this = {};
  }

  bool valid() const {
    return valid_;
  }

  std::int64_t target_sender_time_us() const {
    return target_sender_time_us_;
  }

  std::int64_t ideal_error_us() const {
    return ideal_error_us_;
  }

  std::int64_t applied_correction_us() const {
    return applied_correction_us_;
  }

  std::int64_t last_elapsed_us() const {
    return last_elapsed_us_;
  }

 private:
  static std::int64_t SaturatingAdd(
      std::int64_t left, std::int64_t right) {
    if (right > 0 &&
        left > std::numeric_limits<std::int64_t>::max() - right) {
      return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 &&
        left < std::numeric_limits<std::int64_t>::min() - right) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
  }

  static std::int64_t SaturatingSubtract(
      std::int64_t left, std::int64_t right) {
    if (right == std::numeric_limits<std::int64_t>::min()) {
      if (left >= 0) {
        return std::numeric_limits<std::int64_t>::max();
      }
      return std::numeric_limits<std::int64_t>::max() + left + 1;
    }
    return SaturatingAdd(left, -right);
  }

  bool valid_ = false;
  std::int64_t last_local_time_us_ = 0;
  std::int64_t target_sender_time_us_ = 0;
  std::int64_t ideal_error_us_ = 0;
  std::int64_t applied_correction_us_ = 0;
  std::int64_t last_elapsed_us_ = 0;
};

}  // namespace skate3::multiplayer::playback
