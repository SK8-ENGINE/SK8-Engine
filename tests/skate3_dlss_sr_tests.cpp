#include "skate3_dlss_sr_types.h"

#include <cassert>
#include <cmath>
#include <cstdint>

using namespace skate3::dlss;

namespace {

struct MockOptimalSettings final : OptimalSettingsApi {
  bool succeed = true;
  RenderSize result{1280, 720};
  Mode seen_mode = Mode::kOff;
  RenderSize seen_output{};

  bool Query(Mode mode, RenderSize output,
             RenderSize &optimal_render) override {
    seen_mode = mode;
    seen_output = output;
    optimal_render = result;
    return succeed;
  }
};

void SettingsAndQualityMapping() {
  assert(SanitizeMode(-1) == Mode::kOff);
  assert(SanitizeMode(5) == Mode::kOff);
  assert(SanitizeMode(2) == Mode::kBalanced);
  assert(ToSdkQuality(Mode::kQuality) == SdkQuality::kMaxQuality);
  assert(ToSdkQuality(Mode::kBalanced) == SdkQuality::kBalanced);
  assert(ToSdkQuality(Mode::kPerformance) == SdkQuality::kMaxPerformance);
  assert(ToSdkQuality(Mode::kDlaa) == SdkQuality::kDlaa);
}

void RenderSizeUsesTheSdk() {
  MockOptimalSettings api;
  const RenderSize output{2560, 1440};
  assert(SelectRenderSize(api, Mode::kQuality, output) == api.result);
  assert(api.seen_mode == Mode::kQuality);
  assert(api.seen_output == output);
  api.succeed = false;
  assert(!SelectRenderSize(api, Mode::kPerformance, output).valid());
  assert(SelectRenderSize(api, Mode::kOff, output) == output);
  assert(std::abs(RecommendedMipLodBias({1280, 720}, output) + 2.0f) < 0.0001f);
}

void JitterAndHistoryReset() {
  const Jitter first = JitterForFrame(0);
  const Jitter wrapped = JitterForFrame(32);
  assert(first.x == wrapped.x && first.y == wrapped.y);
  assert(first.x >= -0.5f && first.x < 0.5f);
  assert(first.y >= -0.5f && first.y < 0.5f);

  HistoryTracker history;
  ResetInputs input{Mode::kQuality, {1280, 720},        {1920, 1080}, 10,
                    1000,           {0.0f, 0.0f, 0.0f}, true};
  assert(history.Update(input));
  ++input.scene_generation;
  input.sample_time_us += 16667;
  assert(!history.Update(input));
  ++input.scene_generation;
  input.sample_time_us += 16667;
  input.camera_position[0] = 6.0f;
  assert(history.Update(input));
  ++input.scene_generation;
  input.sample_time_us += 16667;
  input.render.width = 1279;
  assert(history.Update(input));
  history.Invalidate();
  assert(history.Update(input));
}

void RequiredTagsAndLifecycle() {
  std::uint32_t resources[4] = {};
  TaggedResources tags{&resources[0], &resources[1], &resources[2],
                       &resources[3], {1280, 720},   {1920, 1080}};
  assert(tags.valid());
  tags.motion = nullptr;
  assert(!tags.valid());

  LifecycleTracker lifecycle;
  assert(lifecycle.state() == LifecycleState::kCold);
  lifecycle.Initialized(true);
  assert(lifecycle.state() == LifecycleState::kReady);
  lifecycle.ViewportAllocated();
  assert(lifecycle.state() == LifecycleState::kViewportAllocated);
  lifecycle.ResizeOrDeviceLoss();
  assert(lifecycle.state() == LifecycleState::kReady);
  lifecycle.Shutdown();
  assert(lifecycle.state() == LifecycleState::kCold);
  lifecycle.Initialized(false);
  assert(lifecycle.state() == LifecycleState::kFailed);
}

} // namespace

int main() {
  SettingsAndQualityMapping();
  RenderSizeUsesTheSdk();
  JitterAndHistoryReset();
  RequiredTagsAndLifecycle();
  return 0;
}
