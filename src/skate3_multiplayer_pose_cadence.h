#pragma once

#include <algorithm>
#include <cstdint>

namespace skate3::multiplayer::pose_cadence {

struct Snapshot {
  std::uint64_t samples = 0;
  std::uint64_t changes = 0;
  std::uint64_t repeats = 0;
  std::uint64_t sequence_changes = 0;
  std::uint64_t alternations = 0;
  std::uint64_t maximum_repeat_run = 0;
  double maximum_hold_ms = 0.0;
};

// Tracks whether a complete skeleton/palette actually changes at each stage
// of the capture-to-render path. Timestamp cadence alone cannot distinguish
// a smoothly advancing pose from the same pose being republished repeatedly.
class Window {
 public:
  void Record(std::uint64_t wall_time_us,
              std::uint32_t sequence,
              std::uint64_t payload_hash) {
    if (wall_time_us == 0 || payload_hash == 0) {
      return;
    }
    if (!valid_) {
      valid_ = true;
      last_change_time_us_ = wall_time_us;
      last_sequence_ = sequence;
      last_hash_ = payload_hash;
      return;
    }

    ++samples_;
    if (sequence != last_sequence_) {
      ++sequence_changes_;
      last_sequence_ = sequence;
    }
    if (payload_hash == last_hash_) {
      ++repeats_;
      ++current_repeat_run_;
      maximum_repeat_run_ =
          std::max(maximum_repeat_run_, current_repeat_run_);
      if (wall_time_us >= last_change_time_us_) {
        maximum_hold_us_ =
            std::max(
                maximum_hold_us_,
                wall_time_us - last_change_time_us_);
      }
    } else {
      ++changes_;
      if (previous_hash_ != 0 &&
          payload_hash == previous_hash_) {
        ++alternations_;
      }
      previous_hash_ = last_hash_;
      last_hash_ = payload_hash;
      last_change_time_us_ = wall_time_us;
      current_repeat_run_ = 0;
    }
  }

  Snapshot ReadAndReset() {
    Snapshot result;
    result.samples = samples_;
    result.changes = changes_;
    result.repeats = repeats_;
    result.sequence_changes = sequence_changes_;
    result.alternations = alternations_;
    result.maximum_repeat_run = maximum_repeat_run_;
    result.maximum_hold_ms =
        static_cast<double>(maximum_hold_us_) / 1000.0;

    samples_ = 0;
    changes_ = 0;
    repeats_ = 0;
    sequence_changes_ = 0;
    alternations_ = 0;
    maximum_repeat_run_ = 0;
    maximum_hold_us_ = 0;
    return result;
  }

 private:
  bool valid_ = false;
  std::uint64_t last_change_time_us_ = 0;
  std::uint32_t last_sequence_ = 0;
  std::uint64_t last_hash_ = 0;
  std::uint64_t previous_hash_ = 0;
  std::uint64_t current_repeat_run_ = 0;

  std::uint64_t samples_ = 0;
  std::uint64_t changes_ = 0;
  std::uint64_t repeats_ = 0;
  std::uint64_t sequence_changes_ = 0;
  std::uint64_t alternations_ = 0;
  std::uint64_t maximum_repeat_run_ = 0;
  std::uint64_t maximum_hold_us_ = 0;
};

}  // namespace skate3::multiplayer::pose_cadence
