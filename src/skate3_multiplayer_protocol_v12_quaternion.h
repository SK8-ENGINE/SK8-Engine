#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace skate3::multiplayer::protocol_v12 {

struct SmallestThreeQuaternion {
  std::uint8_t omitted_component = 0;
  std::array<std::int16_t, 3> components{};
};

[[nodiscard]] inline bool EncodeSmallestThreeQuaternion(
    std::array<float, 4> quaternion,
    SmallestThreeQuaternion& output) {
  double length_squared = 0.0;
  for (float component : quaternion) {
    if (!std::isfinite(component)) {
      return false;
    }
    length_squared +=
        static_cast<double>(component) * component;
  }
  if (length_squared < 1.0e-12) {
    return false;
  }
  const float inverse_length =
      static_cast<float>(1.0 / std::sqrt(length_squared));
  for (float& component : quaternion) {
    component *= inverse_length;
  }
  std::size_t omitted = 0;
  for (std::size_t index = 1; index < 4; ++index) {
    if (std::fabs(quaternion[index]) >
        std::fabs(quaternion[omitted])) {
      omitted = index;
    }
  }
  if (quaternion[omitted] < 0.0f) {
    for (float& component : quaternion) {
      component = -component;
    }
  }
  constexpr float kScale = 32767.0f * 1.4142135623730951f;
  output = {};
  output.omitted_component = static_cast<std::uint8_t>(omitted);
  std::size_t packed = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    if (index == omitted) {
      continue;
    }
    output.components[packed++] = static_cast<std::int16_t>(
        std::clamp(
            std::lround(quaternion[index] * kScale),
            static_cast<long>(
                std::numeric_limits<std::int16_t>::min()),
            static_cast<long>(
                std::numeric_limits<std::int16_t>::max())));
  }
  return true;
}

[[nodiscard]] inline bool DecodeSmallestThreeQuaternion(
    const SmallestThreeQuaternion& encoded,
    std::array<float, 4>& output) {
  if (encoded.omitted_component >= 4) {
    return false;
  }
  constexpr float kInverseScale =
      1.0f / (32767.0f * 1.4142135623730951f);
  output = {};
  float sum_squared = 0.0f;
  std::size_t packed = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    if (index == encoded.omitted_component) {
      continue;
    }
    const float component =
        static_cast<float>(encoded.components[packed++]) *
        kInverseScale;
    output[index] = component;
    sum_squared += component * component;
  }
  if (!std::isfinite(sum_squared) || sum_squared > 1.0001f) {
    return false;
  }
  output[encoded.omitted_component] =
      std::sqrt(std::max(0.0f, 1.0f - sum_squared));
  return true;
}

}  // namespace skate3::multiplayer::protocol_v12
