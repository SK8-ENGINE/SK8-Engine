#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace skate3::multiplayer::motion {

struct Snapshot {
  std::uint64_t samples = 0;
  double average_interval_ms = 0.0;
  double minimum_interval_ms = 0.0;
  double maximum_interval_ms = 0.0;
  double average_speed = 0.0;
  double average_speed_change = 0.0;
  double maximum_speed_change = 0.0;
};

class Window {
 public:
  void Record(std::uint64_t time_us, const float position[3]) {
    if (time_us == 0 || position == nullptr ||
        !std::isfinite(position[0]) ||
        !std::isfinite(position[1]) ||
        !std::isfinite(position[2])) {
      return;
    }
    if (!valid_) {
      valid_ = true;
      last_time_us_ = time_us;
      std::copy_n(position, 3, last_position_);
      return;
    }
    if (time_us <= last_time_us_) {
      return;
    }

    const std::uint64_t interval_us =
        time_us - last_time_us_;
    const double interval_seconds =
        static_cast<double>(interval_us) / 1000000.0;
    const double dx =
        static_cast<double>(position[0]) - last_position_[0];
    const double dy =
        static_cast<double>(position[1]) - last_position_[1];
    const double dz =
        static_cast<double>(position[2]) - last_position_[2];
    const double distance =
        std::sqrt(dx * dx + dy * dy + dz * dz);
    const double speed =
        interval_seconds > 0.0
            ? distance / interval_seconds
            : 0.0;

    ++samples_;
    interval_sum_us_ += interval_us;
    minimum_interval_us_ =
        std::min(minimum_interval_us_, interval_us);
    maximum_interval_us_ =
        std::max(maximum_interval_us_, interval_us);
    speed_sum_ += speed;
    if (speed_valid_) {
      const double change = std::fabs(speed - last_speed_);
      speed_change_sum_ += change;
      maximum_speed_change_ =
          std::max(maximum_speed_change_, change);
      ++speed_change_samples_;
    }
    speed_valid_ = true;
    last_speed_ = speed;
    last_time_us_ = time_us;
    std::copy_n(position, 3, last_position_);
  }

  Snapshot ReadAndReset() {
    Snapshot result;
    result.samples = samples_;
    if (samples_ != 0) {
      result.average_interval_ms =
          static_cast<double>(interval_sum_us_) /
          static_cast<double>(samples_) / 1000.0;
      result.minimum_interval_ms =
          static_cast<double>(minimum_interval_us_) / 1000.0;
      result.maximum_interval_ms =
          static_cast<double>(maximum_interval_us_) / 1000.0;
      result.average_speed =
          speed_sum_ / static_cast<double>(samples_);
    }
    if (speed_change_samples_ != 0) {
      result.average_speed_change =
          speed_change_sum_ /
          static_cast<double>(speed_change_samples_);
      result.maximum_speed_change =
          maximum_speed_change_;
    }

    samples_ = 0;
    interval_sum_us_ = 0;
    minimum_interval_us_ =
        std::numeric_limits<std::uint64_t>::max();
    maximum_interval_us_ = 0;
    speed_sum_ = 0.0;
    speed_change_samples_ = 0;
    speed_change_sum_ = 0.0;
    maximum_speed_change_ = 0.0;
    return result;
  }

 private:
  bool valid_ = false;
  std::uint64_t last_time_us_ = 0;
  float last_position_[3] = {};
  bool speed_valid_ = false;
  double last_speed_ = 0.0;

  std::uint64_t samples_ = 0;
  std::uint64_t interval_sum_us_ = 0;
  std::uint64_t minimum_interval_us_ =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t maximum_interval_us_ = 0;
  double speed_sum_ = 0.0;
  std::uint64_t speed_change_samples_ = 0;
  double speed_change_sum_ = 0.0;
  double maximum_speed_change_ = 0.0;
};

}  // namespace skate3::multiplayer::motion
