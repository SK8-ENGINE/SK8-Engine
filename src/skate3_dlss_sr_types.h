#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace skate3::dlss {

enum class Mode : std::uint8_t {
  kOff = 0,
  kQuality = 1,
  kBalanced = 2,
  kPerformance = 3,
  kDlaa = 4,
};

enum class SdkQuality : std::uint8_t {
  kOff,
  kMaxQuality,
  kBalanced,
  kMaxPerformance,
  kDlaa,
};

constexpr SdkQuality ToSdkQuality(Mode mode) {
  switch (mode) {
  case Mode::kQuality:
    return SdkQuality::kMaxQuality;
  case Mode::kBalanced:
    return SdkQuality::kBalanced;
  case Mode::kPerformance:
    return SdkQuality::kMaxPerformance;
  case Mode::kDlaa:
    return SdkQuality::kDlaa;
  case Mode::kOff:
    return SdkQuality::kOff;
  }
  return SdkQuality::kOff;
}

constexpr Mode SanitizeMode(int value) {
  return value >= static_cast<int>(Mode::kOff) &&
                 value <= static_cast<int>(Mode::kDlaa)
             ? static_cast<Mode>(value)
             : Mode::kOff;
}

constexpr std::string_view ModeName(Mode mode) {
  switch (mode) {
  case Mode::kOff:
    return "Off";
  case Mode::kQuality:
    return "Quality";
  case Mode::kBalanced:
    return "Balanced";
  case Mode::kPerformance:
    return "Performance";
  case Mode::kDlaa:
    return "DLAA";
  }
  return "Off";
}

struct RenderSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  constexpr bool valid() const { return width != 0 && height != 0; }
  constexpr bool operator==(const RenderSize &) const = default;
};

struct Jitter {
  float x = 0.0f;
  float y = 0.0f;
};

inline float Halton(std::uint32_t index, std::uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index != 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

inline Jitter JitterForFrame(std::uint32_t frame_index) {
  const std::uint32_t sample = frame_index % 32u + 1u;
  return {Halton(sample, 2u) - 0.5f, Halton(sample, 3u) - 0.5f};
}

inline float RecommendedMipLodBias(RenderSize render, RenderSize output) {
  if (!render.valid() || !output.valid()) {
    return 0.0f;
  }
  return std::log2(static_cast<float>(render.width) /
                   static_cast<float>(output.width)) -
         1.0f;
}

enum class UnavailableReason : std::uint8_t {
  kNone = 0,
  kBuildDisabled,
  kBackend,
  kMissingInterposer,
  kMissingPlugin,
  kIdentity,
  kInitialization,
  kNonNvidia,
  kUnsupportedGpu,
  kDriver,
  kOperatingSystem,
  kFeatureNotLoaded,
  kDevice,
  kOptimalSettings,
  kResources,
  kEvaluation,
};

constexpr std::string_view ReasonText(UnavailableReason reason) {
  switch (reason) {
  case UnavailableReason::kNone:
    return "Available";
  case UnavailableReason::kBuildDisabled:
    return "This build does not include DLSS Super Resolution";
  case UnavailableReason::kBackend:
    return "DLSS Super Resolution is available only on DirectX 12";
  case UnavailableReason::kMissingInterposer:
    return "Streamline interposer is absent";
  case UnavailableReason::kMissingPlugin:
    return "DLSS Super Resolution plugin is absent";
  case UnavailableReason::kIdentity:
    return "DLSS application or custom-engine project identity is missing";
  case UnavailableReason::kInitialization:
    return "Streamline initialization failed";
  case UnavailableReason::kNonNvidia:
    return "An NVIDIA RTX GPU is required";
  case UnavailableReason::kUnsupportedGpu:
    return "This NVIDIA GPU does not support DLSS Super Resolution";
  case UnavailableReason::kDriver:
    return "The NVIDIA display driver is too old";
  case UnavailableReason::kOperatingSystem:
    return "The operating system is unsupported";
  case UnavailableReason::kFeatureNotLoaded:
    return "The DLSS Super Resolution plugin failed to load";
  case UnavailableReason::kDevice:
    return "The DirectX 12 device could not be registered";
  case UnavailableReason::kOptimalSettings:
    return "NVIDIA optimal render-size query failed";
  case UnavailableReason::kResources:
    return "Required color, depth, motion, or output resource is invalid";
  case UnavailableReason::kEvaluation:
    return "DLSS Super Resolution evaluation failed";
  }
  return "Unavailable";
}

struct ResetInputs {
  Mode mode = Mode::kOff;
  RenderSize render;
  RenderSize output;
  std::uint64_t scene_generation = 0;
  std::uint64_t sample_time_us = 0;
  std::array<float, 3> camera_position{};
  bool renderer_served_previous_frame = false;
};

class HistoryTracker {
public:
  bool Update(const ResetInputs &current) {
    const float dx = current.camera_position[0] - previous_.camera_position[0];
    const float dy = current.camera_position[1] - previous_.camera_position[1];
    const float dz = current.camera_position[2] - previous_.camera_position[2];
    const bool camera_teleport = dx * dx + dy * dy + dz * dz > 25.0f;
    const bool generation_gap =
        valid_ && current.scene_generation != previous_.scene_generation + 1;
    const bool time_gap =
        valid_ && (current.sample_time_us <= previous_.sample_time_us ||
                   current.sample_time_us - previous_.sample_time_us > 500000);
    const bool reset = !valid_ || current.mode != previous_.mode ||
                       current.render != previous_.render ||
                       current.output != previous_.output || generation_gap ||
                       time_gap || camera_teleport ||
                       !current.renderer_served_previous_frame;
    previous_ = current;
    valid_ = current.mode != Mode::kOff;
    return reset;
  }

  void Invalidate() { valid_ = false; }

private:
  bool valid_ = false;
  ResetInputs previous_{};
};

struct TaggedResources {
  const void *color = nullptr;
  const void *depth = nullptr;
  const void *motion = nullptr;
  const void *output = nullptr;
  RenderSize render;
  RenderSize output_size;

  constexpr bool valid() const {
    return color != nullptr && depth != nullptr && motion != nullptr &&
           output != nullptr && render.valid() && output_size.valid();
  }
};

class OptimalSettingsApi {
public:
  virtual ~OptimalSettingsApi() = default;
  virtual bool Query(Mode mode, RenderSize output,
                     RenderSize &optimal_render) = 0;
};

inline RenderSize SelectRenderSize(OptimalSettingsApi &api, Mode mode,
                                   RenderSize output) {
  if (mode == Mode::kOff || !output.valid()) {
    return output;
  }
  RenderSize render{};
  return api.Query(mode, output, render) && render.valid() ? render
                                                           : RenderSize{};
}

enum class LifecycleState : std::uint8_t {
  kCold,
  kReady,
  kViewportAllocated,
  kFailed,
};

class LifecycleTracker {
public:
  void Initialized(bool ok) {
    state_ = ok ? LifecycleState::kReady : LifecycleState::kFailed;
  }
  void ViewportAllocated() {
    if (state_ == LifecycleState::kReady) {
      state_ = LifecycleState::kViewportAllocated;
    }
  }
  void ResizeOrDeviceLoss() {
    if (state_ != LifecycleState::kCold) {
      state_ = LifecycleState::kReady;
    }
  }
  void Shutdown() { state_ = LifecycleState::kCold; }
  LifecycleState state() const { return state_; }

private:
  LifecycleState state_ = LifecycleState::kCold;
};

} // namespace skate3::dlss
