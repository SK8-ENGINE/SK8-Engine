#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace skate3::owned_shadow {

struct CascadePolicy {
  // This is a ceiling, not a target. A cached cascade does no work until its
  // accumulated camera or sun projection error reaches a visible threshold.
  float maximum_updates_per_second = 0.0f;
  float sun_error_threshold_texels = 0.0f;
  float camera_error_threshold_texels = 0.0f;
};

inline constexpr std::array<CascadePolicy, 3> kCascadePolicies = {{
    {.maximum_updates_per_second = 30.0f,
     .sun_error_threshold_texels = 0.75f,
     .camera_error_threshold_texels = 8.0f},
    {.maximum_updates_per_second = 15.0f,
     .sun_error_threshold_texels = 1.0f,
     .camera_error_threshold_texels = 16.0f},
    {.maximum_updates_per_second = 8.0f,
     .sun_error_threshold_texels = 1.5f,
     .camera_error_threshold_texels = 32.0f},
}};

// A two-degree discontinuity is a time-of-day edit or other lighting jump,
// rather than ordinary cycle motion. Rebuild all cascades immediately so the
// cached shadow direction cannot visibly disagree with the current lighting.
inline constexpr float kSuddenSunJumpCosine = 0.9993908270190958f;

inline float NormalizedDot(const float current[3], const float desired[3]) {
  const float current_length =
      std::sqrt(current[0] * current[0] + current[1] * current[1] +
                current[2] * current[2]);
  const float desired_length =
      std::sqrt(desired[0] * desired[0] + desired[1] * desired[1] +
                desired[2] * desired[2]);
  if (current_length <= 1.0e-6f || desired_length <= 1.0e-6f) {
    return -1.0f;
  }
  return std::clamp((current[0] * desired[0] + current[1] * desired[1] +
                     current[2] * desired[2]) /
                        (current_length * desired_length),
                    -1.0f, 1.0f);
}

// Rotating a light-space basis by theta moves a point at the cascade radius
// by at most 2*r*sin(theta/2). Dividing by the shadow texel width 2*r/N gives
// N*sin(theta/2), an exact conservative error in shadow-map texels.
inline float SunProjectionErrorTexels(float normalized_dot, uint32_t map_size) {
  const float half_chord =
      std::sqrt(std::max(0.0f, (1.0f - normalized_dot) * 0.5f));
  return half_chord * float(std::max(1u, map_size));
}

inline float CameraProjectionErrorTexels(const float current_center[3],
                                         const float desired_center[3],
                                         const float desired_light_x[3],
                                         const float desired_light_y[3],
                                         float meters_per_texel) {
  if (meters_per_texel <= 1.0e-8f) {
    return 0.0f;
  }
  const float delta[3] = {
      desired_center[0] - current_center[0],
      desired_center[1] - current_center[1],
      desired_center[2] - current_center[2],
  };
  const float x = delta[0] * desired_light_x[0] +
                  delta[1] * desired_light_x[1] + delta[2] * desired_light_x[2];
  const float y = delta[0] * desired_light_y[0] +
                  delta[1] * desired_light_y[1] + delta[2] * desired_light_y[2];
  return std::sqrt(x * x + y * y) / meters_per_texel;
}

inline bool ProjectionChanged(float sun_error_texels, float camera_error_texels,
                              const CascadePolicy &policy) {
  return sun_error_texels >= policy.sun_error_threshold_texels ||
         camera_error_texels >= policy.camera_error_threshold_texels;
}

inline float UpdatePressure(float sun_error_texels, float camera_error_texels,
                            const CascadePolicy &policy) {
  const float sun_pressure =
      sun_error_texels / std::max(1.0e-6f, policy.sun_error_threshold_texels);
  const float camera_pressure =
      camera_error_texels /
      std::max(1.0e-6f, policy.camera_error_threshold_texels);
  return std::max(sun_pressure, camera_pressure);
}

inline bool SuddenSunJump(float normalized_dot) {
  return normalized_dot < kSuddenSunJumpCosine;
}

} // namespace skate3::owned_shadow
