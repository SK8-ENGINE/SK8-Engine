#pragma once

#include "skate3_dlss_sr_types.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace skate3::dlss {

enum class NeuralSupportFailure {
  kPluginMissing,
  kPreviewDriverRequired,
  kOther,
};

constexpr std::string_view
NeuralSupportFailureText(NeuralSupportFailure failure) {
  switch (failure) {
  case NeuralSupportFailure::kPluginMissing:
    return "NVIDIA Neural Rendering plugin is unavailable";
  case NeuralSupportFailure::kPreviewDriverRequired:
    return "The NVIDIA preview runtime rejected the installed driver as "
           "out-of-date; install the matching DLSS 5 preview driver";
  case NeuralSupportFailure::kOther:
    return "NVIDIA feature 1004 failed its capability check";
  }
  return "NVIDIA feature 1004 failed its capability check";
}

// DLSS Neural Rendering is an optional post-pass. It never makes DLSS SR
// active by itself and therefore cannot disturb the native/Off path.
struct NeuralSettings {
  bool enabled = false;
  float intensity = 1.0f;
  float local_tone_strength = 1.0f;
  float local_structure_strength = 1.0f;
  float global_tone_strength = 1.0f;
  std::uint32_t style = 0;
  std::uint32_t preset = 0;
  bool use_auto_mask = false;
  float skin_structure_strength = 1.0f;
  std::uint32_t performance_mode = 3;

  constexpr bool operator==(const NeuralSettings &) const = default;
};

constexpr float ClampNeuralStrength(double value) {
  return static_cast<float>(std::clamp(value, 0.0, 2.0));
}

constexpr std::uint32_t ClampNeuralStyle(std::int32_t value) {
  return static_cast<std::uint32_t>(std::clamp(value, 0, 2));
}

constexpr std::uint32_t ClampNeuralPreset(std::int32_t value) {
  return static_cast<std::uint32_t>(std::clamp(value, 0, 3));
}

constexpr std::uint32_t ClampNeuralPerformanceMode(std::int32_t value) {
  return static_cast<std::uint32_t>(std::clamp(value, 0, 3));
}

struct NeuralTaggedResources {
  const void *input = nullptr;
  const void *depth = nullptr;
  const void *motion = nullptr;
  const void *output = nullptr;
  RenderSize render;
  RenderSize output_size;

  constexpr bool valid() const {
    return input != nullptr && depth != nullptr && motion != nullptr &&
           output != nullptr && render.valid() && output_size.valid() &&
           input != output;
  }
};

} // namespace skate3::dlss
