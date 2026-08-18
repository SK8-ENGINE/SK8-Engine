#pragma once

#include "skate3_trick_types.h"

#include <cstdint>
#include <optional>
#include <string_view>

struct PPCContext;

namespace skate3::trick_overrides {

enum class Action {
  None,
  ReplacePrimaryWithSecondary,
  ReplacePrimaryWithNamedDescriptor,
  Disable,
};

struct GestureMatch {
  uint32_t group{};
  std::string_view key_name;
  std::string_view primary_name;
  std::string_view secondary_name;
};

struct Result {
  Action action{Action::None};
  std::string_view rule_name;
  std::string_view source_name;
  std::string_view target_name;
  trick::TrickIntentDescriptor original_primary{};
  trick::TrickIntentDescriptor final_primary{};

  [[nodiscard]] bool applied() const noexcept {
    return action != Action::None;
  }
};

struct ScorableOverrideResult {
  bool applied{};
  bool base_points_applied{};
  std::string_view rule_name;
  std::string_view scorable_name;
  std::string_view display_name;
  uint32_t definition_index{};
  uint32_t original_base_points{};
  uint32_t final_base_points{};
};

struct DisplayNameOverride {
  bool applied{};
  std::string_view rule_name;
  std::string_view scorable_name;
  std::string_view display_name;
};

struct AnimationOverrideResult {
  bool applied{};
  std::string_view rule_name;
  std::string_view source_animation;
  std::string_view target_animation;
  trick::TrickIntentDescriptor original_descriptor{};
  trick::TrickIntentDescriptor final_descriptor{};
};

struct AnimationAssetRuleView {
  std::string_view name;
  std::string_view animation;
  std::string_view database_name;
  std::string_view path;
  std::string_view playback_data;
  uint32_t memory_group{};
  bool load_only{};
};

struct AnimationLeafReplacementRuleView {
  std::string_view name;
  std::string_view source_animation;
  std::string_view target_animation;
  std::string_view asset;
};

// Applies the first enabled exact-match rule. All rules are default-off and
// intentionally identify whether they reuse a retail descriptor or suppress
// a retail intent; neither operation is a genuinely custom trick.
using DescriptorResolver = bool (*)(
    std::string_view name, trick::TrickIntentDescriptor& descriptor);

Result ApplyGestureOverride(uint8_t* guest_base, uint32_t output_address,
                            const GestureMatch& match,
                            DescriptorResolver resolve_descriptor);
ScorableOverrideResult ApplyScorableOverride(
    uint8_t* guest_base, uint32_t scorable_address,
    std::string_view scorable_name);
DisplayNameOverride FindDisplayNameOverride(uint32_t definition_index);
AnimationOverrideResult ApplyAnimationOverride(
    PPCContext& source_context, uint8_t* guest_base,
    uint32_t selected_descriptor_address);
std::optional<AnimationAssetRuleView> FindAnimationAsset(
    std::string_view animation);
std::optional<AnimationAssetRuleView> FindAnimationAssetByIndex(
    uint32_t index);
std::optional<AnimationLeafReplacementRuleView>
FindAnimationLeafReplacementByIndex(uint32_t index);

[[nodiscard]] bool KickflipReplacementEnabled();
[[nodiscard]] bool KickflipDisableEnabled();
[[nodiscard]] bool OverrideFileConfigured();
[[nodiscard]] bool OverrideFileLoaded();
[[nodiscard]] uint32_t LoadedRuleCount();
[[nodiscard]] uint32_t LoadedScorableRuleCount();
[[nodiscard]] uint32_t LoadedAnimationRuleCount();
[[nodiscard]] uint32_t LoadedAnimationAssetRuleCount();
[[nodiscard]] uint32_t LoadedAnimationLeafReplacementRuleCount();
[[nodiscard]] std::string_view OverrideFileStatus();

}  // namespace skate3::trick_overrides
