#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace skate3::multiplayer::pose_curve {

inline float InterpolateHermite(
    float previous, float first, float second, float next,
    std::uint64_t previous_time, std::uint64_t first_time,
    std::uint64_t second_time, std::uint64_t next_time,
    float amount) {
  const double segment =
      static_cast<double>(second_time - first_time);
  if (!(segment > 0.0)) {
    return second;
  }
  const double first_span =
      static_cast<double>(second_time - previous_time);
  const double second_span =
      static_cast<double>(next_time - first_time);
  const double first_tangent =
      first_span > 0.0
          ? (static_cast<double>(second) -
             static_cast<double>(previous)) *
                segment / first_span
          : static_cast<double>(second) -
                static_cast<double>(first);
  const double second_tangent =
      second_span > 0.0
          ? (static_cast<double>(next) -
             static_cast<double>(first)) *
                segment / second_span
          : static_cast<double>(second) -
                static_cast<double>(first);
  const double t = std::clamp(
      static_cast<double>(amount), 0.0, 1.0);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
  const double h10 = t3 - 2.0 * t2 + t;
  const double h01 = -2.0 * t3 + 3.0 * t2;
  const double h11 = t3 - t2;
  return static_cast<float>(
      h00 * static_cast<double>(first) +
      h10 * first_tangent +
      h01 * static_cast<double>(second) +
      h11 * second_tangent);
}

inline float InterpolateBoundedHermite(
    float previous, float first, float second, float next,
    std::uint64_t previous_time, std::uint64_t first_time,
    std::uint64_t second_time, std::uint64_t next_time,
    float amount) {
  const float interpolated = InterpolateHermite(
      previous, first, second, next,
      previous_time, first_time, second_time, next_time,
      amount);
  const auto [minimum, maximum] =
      std::minmax({previous, first, second, next});
  return std::clamp(interpolated, minimum, maximum);
}

// Final skinning rows are affine coefficients, and a weighted vertex is a
// linear combination of those coefficients. Interpolating every coefficient
// with the same C1 four-sample curve therefore gives the visible skinned
// vertex continuous velocity at packet boundaries. The component envelope
// prevents a noisy neighbour from producing an unbounded matrix overshoot.
inline void InterpolateBoundedAffine(
    const float previous[12], const float first[12],
    const float second[12], const float next[12],
    std::uint64_t previous_time, std::uint64_t first_time,
    std::uint64_t second_time, std::uint64_t next_time,
    float amount, float out[12]) {
  for (std::size_t component = 0; component < 12; ++component) {
    out[component] = InterpolateBoundedHermite(
        previous[component], first[component],
        second[component], next[component],
        previous_time, first_time, second_time, next_time,
        amount);
  }
}

}  // namespace skate3::multiplayer::pose_curve
