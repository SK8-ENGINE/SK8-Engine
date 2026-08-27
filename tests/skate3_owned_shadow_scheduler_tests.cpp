#include "skate3_owned_shadow_scheduler.h"

#include <cmath>
#include <iostream>
#include <numbers>
#include <string_view>

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

void ExpectNear(float actual, float expected, float tolerance,
                std::string_view message) {
  if (std::abs(actual - expected) > tolerance) {
    std::cerr << "FAIL: " << message << " (actual=" << actual
              << ", expected=" << expected << ")\n";
    ++g_failures;
  }
}

void TestUnchangedProjectionStaysCached() {
  const float sun[3] = {0.2f, 0.9f, 0.3f};
  const float center[3] = {4.0f, 2.0f, -8.0f};
  const float light_x[3] = {1.0f, 0.0f, 0.0f};
  const float light_y[3] = {0.0f, 0.0f, 1.0f};
  const float dot = skate3::owned_shadow::NormalizedDot(sun, sun);
  const float sun_error =
      skate3::owned_shadow::SunProjectionErrorTexels(dot, 2048);
  const float camera_error = skate3::owned_shadow::CameraProjectionErrorTexels(
      center, center, light_x, light_y, 0.02f);
  ExpectNear(sun_error, 0.0f, 1.0e-4f,
             "unchanged sun accumulated projection error");
  ExpectNear(camera_error, 0.0f, 1.0e-4f,
             "unchanged camera accumulated projection error");
  Expect(
      !skate3::owned_shadow::ProjectionChanged(
          sun_error, camera_error, skate3::owned_shadow::kCascadePolicies[0]),
      "unchanged cascade requested a rebuild");
}

void TestCameraErrorUsesShadowTexels() {
  const float current[3] = {0.0f, 0.0f, 0.0f};
  const float desired[3] = {0.16f, 2.0f, 0.12f};
  const float light_x[3] = {1.0f, 0.0f, 0.0f};
  const float light_y[3] = {0.0f, 0.0f, 1.0f};
  const float error = skate3::owned_shadow::CameraProjectionErrorTexels(
      current, desired, light_x, light_y, 0.02f);
  ExpectNear(error, 10.0f, 1.0e-4f,
             "camera displacement was not measured in map texels");
  Expect(skate3::owned_shadow::ProjectionChanged(
             0.0f, error, skate3::owned_shadow::kCascadePolicies[0]),
         "near cascade ignored a camera displacement beyond its guard band");
}

void TestSunErrorScalesWithResolution() {
  constexpr float angle = std::numbers::pi_v<float> / 180.0f;
  const float dot = std::cos(angle);
  const float error_1024 =
      skate3::owned_shadow::SunProjectionErrorTexels(dot, 1024);
  const float error_2048 =
      skate3::owned_shadow::SunProjectionErrorTexels(dot, 2048);
  ExpectNear(error_2048, error_1024 * 2.0f, 1.0e-3f,
             "sun projection error did not scale with cascade resolution");
  ExpectNear(error_2048, 2048.0f * std::sin(angle * 0.5f), 1.0e-3f,
             "sun projection error did not match the half-chord bound");
}

void TestAdaptivePoliciesAreQualityOrdered() {
  const auto &near_policy = skate3::owned_shadow::kCascadePolicies[0];
  const auto &middle_policy = skate3::owned_shadow::kCascadePolicies[1];
  const auto &far_policy = skate3::owned_shadow::kCascadePolicies[2];
  Expect(near_policy.maximum_updates_per_second >
                 middle_policy.maximum_updates_per_second &&
             middle_policy.maximum_updates_per_second >
                 far_policy.maximum_updates_per_second,
         "cascade update ceilings are not ordered near-to-far");
  Expect(near_policy.sun_error_threshold_texels <
                 middle_policy.sun_error_threshold_texels &&
             middle_policy.sun_error_threshold_texels <
                 far_policy.sun_error_threshold_texels,
         "sun error tolerances are not ordered near-to-far");
  Expect(near_policy.camera_error_threshold_texels <
                 middle_policy.camera_error_threshold_texels &&
             middle_policy.camera_error_threshold_texels <
                 far_policy.camera_error_threshold_texels,
         "camera guard bands are not ordered near-to-far");
}

void TestTimeJumpBypassesOrdinaryScheduling() {
  const float small_motion_dot =
      std::cos(0.25f * std::numbers::pi_v<float> / 180.0f);
  const float time_jump_dot =
      std::cos(3.0f * std::numbers::pi_v<float> / 180.0f);
  Expect(!skate3::owned_shadow::SuddenSunJump(small_motion_dot),
         "ordinary day/night motion was classified as a time jump");
  Expect(skate3::owned_shadow::SuddenSunJump(time_jump_dot),
         "large time-of-day edit was not classified as a time jump");
}

} // namespace

int main() {
  TestUnchangedProjectionStaysCached();
  TestCameraErrorUsesShadowTexels();
  TestSunErrorScalesWithResolution();
  TestAdaptivePoliciesAreQualityOrdered();
  TestTimeJumpBypassesOrdinaryScheduling();
  if (g_failures != 0) {
    std::cerr << g_failures << " owned-shadow scheduler test(s) failed\n";
    return 1;
  }
  std::cout << "owned-shadow scheduler tests passed\n";
  return 0;
}
