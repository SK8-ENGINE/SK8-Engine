#pragma once

#include <algorithm>
#include <cstdint>

namespace skate3::multiplayer::interpolation {

// Complete final-pose frames arrive as multi-datagram groups and are prepared
// on the replication worker. Two source periods place the presentation cursor
// behind a complete bracketing pair. Additional measured-jitter headroom
// absorbs scheduling and fragment-assembly variation without multiplying a
// deliberately lower internet snapshot interval into excessive view lag.
inline std::int64_t RecommendedDelayMicroseconds(
    std::int32_t configured_delay_ms,
    std::int64_t measured_period_us,
    std::int64_t measured_jitter_us,
    bool have_animation_samples) {
  const std::int64_t configured_delay_us =
      std::int64_t{
          std::clamp(configured_delay_ms, 0, 250)} *
      1000;
  // Zero is an explicit diagnostic bypass. Without this escape hatch the
  // adaptive floor would silently turn a requested zero-delay comparison
  // back into roughly 100-130 ms at the normal 20 Hz pose cadence.
  if (configured_delay_ms <= 0) {
    return 0;
  }
  if (!have_animation_samples) {
    return configured_delay_us;
  }

  const std::int64_t stable_period_us =
      std::clamp<std::int64_t>(
          measured_period_us, 8000, 150000);
  const std::int64_t stable_jitter_us =
      std::clamp<std::int64_t>(
          measured_jitter_us, 0, 50000);
  const std::int64_t safety_delay_us =
      stable_period_us * 2 + stable_jitter_us * 4;
  return std::clamp<std::int64_t>(
      std::max(configured_delay_us, safety_delay_us),
      0, 250000);
}

}  // namespace skate3::multiplayer::interpolation
