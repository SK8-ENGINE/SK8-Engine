#include "skate3_trick_overrides.h"

#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_BOOL(
    skate3_trick_replace_kickflip_with_heelflip, false, "Skate 3",
    "Experimental: route recognized Kickflip intent to the built-in "
    "Heelflip descriptor (existing-trick replacement, not a custom trick)");
REXCVAR_DEFINE_BOOL(
    skate3_trick_disable_kickflip, false, "Skate 3",
    "Experimental: suppress the recognized retail Kickflip gesture intent");
REXCVAR_DEFINE_STRING(
    skate3_trick_override_file, "", "Skate 3",
    "Project-relative TOML file containing exact-match gesture override rules");

namespace skate3::trick_overrides {
namespace {

enum class EnableFlag {
  ReplaceKickflipWithHeelflip,
  DisableKickflip,
};

struct GestureOverrideRule {
  std::string name;
  uint32_t group;
  std::string key_name;
  std::string primary_name;
  std::string secondary_name;
  Action action;
  EnableFlag enable_flag;
};

// Ordering is policy: disabling a gesture wins if both experimental flags are
// present. This table is the first readable, extensible override registry.
const std::array kGestureOverrideRules{
    GestureOverrideRule{
        .name = "disable-kickflip",
        .group = 0,
        .key_name = "Kickflip",
        .primary_name = "Kickflip",
        .secondary_name = "Heelflip",
        .action = Action::Disable,
        .enable_flag = EnableFlag::DisableKickflip,
    },
    GestureOverrideRule{
        .name = "replace-kickflip-with-heelflip",
        .group = 0,
        .key_name = "Kickflip",
        .primary_name = "Kickflip",
        .secondary_name = "Heelflip",
        .action = Action::ReplacePrimaryWithSecondary,
        .enable_flag = EnableFlag::ReplaceKickflipWithHeelflip,
    },
};

struct LoadedGestureOverrideRule {
  std::string name;
  uint32_t group;
  std::string key_name;
  std::string primary_name;
  std::string secondary_name;
  std::string target_name;
  Action action;
};

struct LoadedScorableOverrideRule {
  std::string name;
  std::string scorable_name;
  std::optional<uint32_t> base_points;
  std::optional<std::string> display_name;
};

struct LoadedAnimationOverrideRule {
  std::string name;
  std::string source_animation;
  std::string target_animation;
};

struct LoadedAnimationAssetRule {
  std::string name;
  std::string animation;
  std::string database_name;
  std::string path;
  std::string playback_data;
  uint32_t memory_group;
  bool load_only;
};

struct LoadedAnimationLeafReplacementRule {
  std::string name;
  std::string source_animation;
  std::string target_animation;
  std::string asset;
};

enum class FileStatus : uint32_t {
  NotConfigured,
  Loaded,
  ParseError,
  SchemaError,
  IoError,
};

std::once_flag g_load_once;
std::vector<LoadedGestureOverrideRule> g_loaded_rules;
std::vector<LoadedScorableOverrideRule> g_loaded_scorable_rules;
std::vector<LoadedAnimationOverrideRule> g_loaded_animation_rules;
std::vector<LoadedAnimationAssetRule> g_loaded_animation_asset_rules;
std::vector<LoadedAnimationLeafReplacementRule>
    g_loaded_animation_leaf_replacement_rules;
std::mutex g_display_name_mutex;
std::map<uint32_t, size_t> g_display_name_rules_by_definition_index;
std::atomic<bool> g_file_configured{false};
std::atomic<FileStatus> g_file_status{FileStatus::NotConfigured};
std::atomic<uint32_t> g_loaded_rule_count{0};
std::atomic<uint32_t> g_loaded_scorable_rule_count{0};
std::atomic<uint32_t> g_loaded_animation_rule_count{0};
std::atomic<uint32_t> g_loaded_animation_asset_rule_count{0};
std::atomic<uint32_t> g_loaded_animation_leaf_replacement_rule_count{0};

bool IsEnabled(EnableFlag flag) {
  switch (flag) {
    case EnableFlag::ReplaceKickflipWithHeelflip:
      return KickflipReplacementEnabled();
    case EnableFlag::DisableKickflip:
      return KickflipDisableEnabled();
  }
  return false;
}

bool Matches(const GestureOverrideRule& rule, const GestureMatch& match) {
  return rule.group == match.group && rule.key_name == match.key_name &&
         rule.primary_name == match.primary_name &&
         rule.secondary_name == match.secondary_name;
}

bool Matches(const LoadedGestureOverrideRule& rule,
             const GestureMatch& match) {
  return rule.group == match.group && rule.key_name == match.key_name &&
         rule.primary_name == match.primary_name &&
         rule.secondary_name == match.secondary_name;
}

bool IsTelemetryToken(std::string_view value) {
  if (value.empty() || value.size() > 63) {
    return false;
  }
  for (const unsigned char character : value) {
    if (!std::isalnum(character) && character != '_' && character != '-' &&
        character != '.') {
      return false;
    }
  }
  return true;
}

bool IsDisplayName(std::string_view value) {
  if (value.empty() || value.size() > 63) {
    return false;
  }
  for (const unsigned char character : value) {
    if (character < 0x20 || character > 0x7E || character == ':' ||
        character == ',' || character == '#') {
      return false;
    }
  }
  return true;
}

bool IsFastString36(std::string_view value) {
  if (value.empty() || value.size() > 36) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') ||
          character == '_')) {
      return false;
    }
  }
  return true;
}

bool IsAnimationAssetPath(std::string_view value) {
  if (value.empty() || value.size() > 127 ||
      value.find("..") != std::string_view::npos ||
      !(value.ends_with(".abin") || value.ends_with(".ABIN"))) {
    return false;
  }
  const bool under_animation_directory =
      value.starts_with("data/anim/") ||
      value.starts_with("data\\anim\\");
  if (!under_animation_directory) {
    return false;
  }
  for (const unsigned char character : value) {
    if (!std::isalnum(character) && character != '_' && character != '-' &&
        character != '.' && character != '/' && character != '\\') {
      return false;
    }
  }
  return true;
}

std::optional<Action> ParseAction(std::string_view value) {
  if (value == "disable") {
    return Action::Disable;
  }
  if (value == "replace-primary-with-secondary") {
    return Action::ReplacePrimaryWithSecondary;
  }
  if (value == "replace-primary") {
    return Action::ReplacePrimaryWithNamedDescriptor;
  }
  return std::nullopt;
}

void LoadOverrideFile() {
  const std::string path = REXCVAR_GET(skate3_trick_override_file);
  if (path.empty()) {
    return;
  }
  g_file_configured.store(true, std::memory_order_release);

  try {
    const auto document = toml::parse_file(path);
    const auto* rules = document["gesture_overrides"].as_array();
    const auto* scorable_rules =
        document["scorable_overrides"].as_array();
    const auto* animation_rules =
        document["animation_overrides"].as_array();
    const auto* animation_asset_rules =
        document["animation_assets"].as_array();
    const auto* animation_leaf_replacement_rules =
        document["animation_leaf_replacements"].as_array();
    if ((!rules && !scorable_rules && !animation_rules &&
         !animation_asset_rules && !animation_leaf_replacement_rules) ||
        (rules && rules->size() > 256) ||
        (scorable_rules && scorable_rules->size() > 256) ||
        (animation_rules && animation_rules->size() > 256) ||
        (animation_asset_rules && animation_asset_rules->size() > 16) ||
        (animation_leaf_replacement_rules &&
         animation_leaf_replacement_rules->size() > 256)) {
      g_file_status.store(FileStatus::SchemaError,
                          std::memory_order_release);
      return;
    }

    std::vector<LoadedGestureOverrideRule> parsed;
    if (rules) {
      parsed.reserve(rules->size());
      for (const auto& node : *rules) {
        const auto* table = node.as_table();
        if (!table) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        const auto name = (*table)["name"].value<std::string>();
        const auto action_name =
            (*table)["action"].value<std::string>();
        const auto group = (*table)["group"].value<int64_t>();
        const auto key = (*table)["key"].value<std::string>();
        const auto primary = (*table)["primary"].value<std::string>();
        const auto secondary =
            (*table)["secondary"].value<std::string>();
        const auto target = (*table)["target"].value<std::string>();
        if (!name || !action_name || !group || !key || !primary ||
            !secondary || *group < 0 ||
            *group >= trick::GestureTrickMappingLayout::kGroupCount ||
            !IsTelemetryToken(*name) || !IsTelemetryToken(*key) ||
            !IsTelemetryToken(*primary) ||
            !IsTelemetryToken(*secondary)) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        const auto action = ParseAction(*action_name);
        if (!action ||
            (*action == Action::ReplacePrimaryWithNamedDescriptor &&
             (!target || !IsTelemetryToken(*target)))) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        parsed.push_back({
            .name = *name,
            .group = static_cast<uint32_t>(*group),
            .key_name = *key,
            .primary_name = *primary,
            .secondary_name = *secondary,
            .target_name = target.value_or(std::string{}),
            .action = *action,
        });
      }
    }

    std::vector<LoadedScorableOverrideRule> parsed_scorable;
    if (scorable_rules) {
      parsed_scorable.reserve(scorable_rules->size());
      for (const auto& node : *scorable_rules) {
        const auto* table = node.as_table();
        if (!table) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        const auto name = (*table)["name"].value<std::string>();
        const auto scorable = (*table)["scorable"].value<std::string>();
        const auto base_points =
            (*table)["base_points"].value<int64_t>();
        const auto display_name =
            (*table)["display_name"].value<std::string>();
        if (!name || !scorable || (!base_points && !display_name) ||
            !IsTelemetryToken(*name) ||
            !IsTelemetryToken(*scorable) ||
            (base_points &&
             (*base_points < 0 || *base_points > 1000000)) ||
            (display_name && !IsDisplayName(*display_name))) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        parsed_scorable.push_back({
            .name = *name,
            .scorable_name = *scorable,
            .base_points =
                base_points
                    ? std::optional<uint32_t>{
                          static_cast<uint32_t>(*base_points)}
                    : std::nullopt,
            .display_name = display_name,
        });
      }
    }

    std::vector<LoadedAnimationOverrideRule> parsed_animation;
    if (animation_rules) {
      parsed_animation.reserve(animation_rules->size());
      for (const auto& node : *animation_rules) {
        const auto* table = node.as_table();
        if (!table) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        const auto name = (*table)["name"].value<std::string>();
        const auto source =
            (*table)["source_animation"].value<std::string>();
        const auto target =
            (*table)["target_animation"].value<std::string>();
        if (!name || !source || !target || !IsTelemetryToken(*name) ||
            !IsTelemetryToken(*source) || !IsTelemetryToken(*target)) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        parsed_animation.push_back({
            .name = *name,
            .source_animation = *source,
            .target_animation = *target,
        });
      }
    }

    std::vector<LoadedAnimationAssetRule> parsed_animation_assets;
    if (animation_asset_rules) {
      parsed_animation_assets.reserve(animation_asset_rules->size());
      for (const auto& node : *animation_asset_rules) {
        const auto* table = node.as_table();
        if (!table) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        const auto name = (*table)["name"].value<std::string>();
        const auto animation =
            (*table)["animation"].value<std::string>();
        const auto database_name =
            (*table)["database_name"].value<std::string>();
        const auto path = (*table)["path"].value<std::string>();
        const auto playback_data =
            (*table)["playback_data"].value<std::string>();
        const auto memory_group =
            (*table)["memory_group"].value<int64_t>();
        const bool load_only =
            (*table)["load_only"].value_or(false);
        if (!name || !animation || !database_name || !path ||
            !playback_data || !memory_group ||
            !IsTelemetryToken(*name) ||
            !IsFastString36(*animation) ||
            !IsFastString36(*database_name) ||
            !IsAnimationAssetPath(*path) ||
            !IsFastString36(*playback_data) ||
            *memory_group < 1 || *memory_group > 16) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        parsed_animation_assets.push_back({
            .name = *name,
            .animation = *animation,
            .database_name = *database_name,
            .path = *path,
            .playback_data = *playback_data,
            .memory_group = static_cast<uint32_t>(*memory_group),
            .load_only = load_only,
        });
      }
    }

    std::vector<LoadedAnimationLeafReplacementRule>
        parsed_animation_leaf_replacements;
    if (animation_leaf_replacement_rules) {
      parsed_animation_leaf_replacements.reserve(
          animation_leaf_replacement_rules->size());
      for (const auto& node : *animation_leaf_replacement_rules) {
        const auto* table = node.as_table();
        if (!table) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        const auto name = (*table)["name"].value<std::string>();
        const auto source =
            (*table)["source_animation"].value<std::string>();
        const auto target =
            (*table)["target_animation"].value<std::string>();
        const auto asset = (*table)["asset"].value<std::string>();
        if (!name || !source || !target || !asset ||
            !IsTelemetryToken(*name) ||
            !IsFastString36(*source) ||
            !IsFastString36(*target) ||
            !IsTelemetryToken(*asset) ||
            std::none_of(
                parsed_animation_assets.begin(),
                parsed_animation_assets.end(),
                [&](const LoadedAnimationAssetRule& candidate) {
                  return candidate.name == *asset;
                })) {
          g_file_status.store(FileStatus::SchemaError,
                              std::memory_order_release);
          return;
        }
        parsed_animation_leaf_replacements.push_back({
            .name = *name,
            .source_animation = *source,
            .target_animation = *target,
            .asset = *asset,
        });
      }
    }

    g_loaded_rules = std::move(parsed);
    g_loaded_scorable_rules = std::move(parsed_scorable);
    g_loaded_animation_rules = std::move(parsed_animation);
    g_loaded_animation_asset_rules =
        std::move(parsed_animation_assets);
    g_loaded_animation_leaf_replacement_rules =
        std::move(parsed_animation_leaf_replacements);
    g_loaded_rule_count.store(
        static_cast<uint32_t>(g_loaded_rules.size()),
        std::memory_order_release);
    g_loaded_scorable_rule_count.store(
        static_cast<uint32_t>(g_loaded_scorable_rules.size()),
        std::memory_order_release);
    g_loaded_animation_rule_count.store(
        static_cast<uint32_t>(g_loaded_animation_rules.size()),
        std::memory_order_release);
    g_loaded_animation_asset_rule_count.store(
        static_cast<uint32_t>(g_loaded_animation_asset_rules.size()),
        std::memory_order_release);
    g_loaded_animation_leaf_replacement_rule_count.store(
        static_cast<uint32_t>(
            g_loaded_animation_leaf_replacement_rules.size()),
        std::memory_order_release);
    g_file_status.store(FileStatus::Loaded, std::memory_order_release);
  } catch (const toml::parse_error&) {
    g_file_status.store(FileStatus::ParseError, std::memory_order_release);
  } catch (const std::exception&) {
    g_file_status.store(FileStatus::IoError, std::memory_order_release);
  }
}

trick::TrickIntentDescriptor LoadDescriptor(uint8_t* base, uint32_t address) {
  trick::TrickIntentDescriptor descriptor{};
  if (!address) {
    return descriptor;
  }
  for (size_t index = 0; index < descriptor.words.size(); ++index) {
    descriptor.words[index] =
        REX_LOAD_U32(address + static_cast<uint32_t>(index * sizeof(uint32_t)));
  }
  return descriptor;
}

void StoreDescriptor(uint8_t* base, uint32_t address,
                     const trick::TrickIntentDescriptor& descriptor) {
  if (!address) {
    return;
  }
  for (size_t index = 0; index < descriptor.words.size(); ++index) {
    REX_STORE_U32(address + static_cast<uint32_t>(index * sizeof(uint32_t)),
                  descriptor.words[index]);
  }
}

void ClearIntents(uint8_t* base, uint32_t address) {
  if (!address) {
    return;
  }
  for (size_t index = 0; index < trick::TrickIntents::kWordCount; ++index) {
    REX_STORE_U32(address + static_cast<uint32_t>(index * sizeof(uint32_t)), 0);
  }
}

trick::TrickIntentDescriptor ConstructAnimationDescriptor(
    PPCContext& source_context, uint8_t* guest_base,
    std::string_view animation_name, uint32_t workspace_offset) {
  trick::TrickIntentDescriptor descriptor{};
  if (!guest_base || animation_name.empty()) {
    return descriptor;
  }

  uint8_t* base = guest_base;
  PPCContext constructor_context = source_context;
  constructor_context.r1.u32 =
      (source_context.r1.u32 - 0x400) & ~0xFu;
  const uint32_t descriptor_address =
      constructor_context.r1.u32 + workspace_offset;
  const uint32_t name_address = descriptor_address + 0x40;

  for (size_t index = 0;
       index < trick::TrickIntentDescriptor::kWordCount; ++index) {
    REX_STORE_U32(
        descriptor_address + static_cast<uint32_t>(index * sizeof(uint32_t)),
        0);
  }
  for (size_t index = 0; index < animation_name.size(); ++index) {
    REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(animation_name[index]));
  }
  REX_STORE_U8(
      name_address + static_cast<uint32_t>(animation_name.size()), 0);

  constructor_context.r3.u64 = descriptor_address;
  constructor_context.r4.u64 = name_address;
  sub_823C3B00(constructor_context, base);
  return LoadDescriptor(base, descriptor_address);
}

}  // namespace

Result ApplyGestureOverride(uint8_t* guest_base, uint32_t output_address,
                            const GestureMatch& match,
                            DescriptorResolver resolve_descriptor) {
  if (!guest_base) {
    return {};
  }

  std::call_once(g_load_once, LoadOverrideFile);
  for (const auto& rule : g_loaded_rules) {
    if (!Matches(rule, match)) {
      continue;
    }

    Result result{
        .action = rule.action,
        .rule_name = rule.name,
        .source_name = rule.primary_name,
        .target_name =
            rule.action == Action::Disable ? std::string_view{"DISABLED"}
            : rule.action == Action::ReplacePrimaryWithNamedDescriptor
                ? std::string_view{rule.target_name}
                : std::string_view{rule.secondary_name},
    };
    if (output_address) {
      result.original_primary = LoadDescriptor(guest_base, output_address);
    }
    if (rule.action == Action::Disable) {
      ClearIntents(guest_base, output_address);
      return result;
    }
    if (rule.action == Action::ReplacePrimaryWithSecondary &&
        output_address) {
      result.final_primary = LoadDescriptor(
          guest_base,
          output_address +
              trick::GestureMappingValueLayout::kSecondaryDescriptor);
      StoreDescriptor(guest_base, output_address, result.final_primary);
      return result;
    }
    if (rule.action == Action::ReplacePrimaryWithNamedDescriptor &&
        output_address && resolve_descriptor &&
        resolve_descriptor(rule.target_name, result.final_primary)) {
      StoreDescriptor(guest_base, output_address, result.final_primary);
      return result;
    }
    return {};
  }

  for (const auto& rule : kGestureOverrideRules) {
    if (!IsEnabled(rule.enable_flag) || !Matches(rule, match)) {
      continue;
    }

    Result result{
        .action = rule.action,
        .rule_name = rule.name,
        .source_name = rule.primary_name,
        .target_name =
            rule.action == Action::Disable ? std::string_view{"DISABLED"}
                                           : rule.secondary_name,
    };

    if (output_address) {
      result.original_primary = LoadDescriptor(guest_base, output_address);
    }

    switch (rule.action) {
      case Action::ReplacePrimaryWithSecondary:
        if (!output_address) {
          return {};
        }
        result.final_primary = LoadDescriptor(
            guest_base,
            output_address + trick::GestureMappingValueLayout::
                                 kSecondaryDescriptor);
        StoreDescriptor(guest_base, output_address, result.final_primary);
        break;
      case Action::ReplacePrimaryWithNamedDescriptor:
        return {};
      case Action::Disable:
        ClearIntents(guest_base, output_address);
        break;
      case Action::None:
        return {};
    }
    return result;
  }
  return {};
}

ScorableOverrideResult ApplyScorableOverride(
    uint8_t* guest_base, uint32_t scorable_address,
    std::string_view scorable_name) {
  if (!guest_base || !scorable_address || scorable_name.empty()) {
    return {};
  }
  uint8_t* base = guest_base;

  std::call_once(g_load_once, LoadOverrideFile);
  for (const auto& rule : g_loaded_scorable_rules) {
    if (rule.scorable_name != scorable_name) {
      continue;
    }
    const uint32_t value_address =
        scorable_address + trick::ScorableLayout::kBasePointValue;
    const uint32_t original = REX_LOAD_U32(value_address);
    const uint32_t definition_index =
        REX_LOAD_U32(scorable_address +
                     trick::ScorableLayout::kDefinitionWord8);
    if (rule.display_name) {
      const size_t rule_index =
          static_cast<size_t>(&rule - g_loaded_scorable_rules.data());
      std::lock_guard lock(g_display_name_mutex);
      g_display_name_rules_by_definition_index[definition_index] =
          rule_index;
    }
    if (rule.base_points) {
      REX_STORE_U32(value_address, *rule.base_points);
    }
    return {
        .applied = true,
        .base_points_applied = rule.base_points.has_value(),
        .rule_name = rule.name,
        .scorable_name = rule.scorable_name,
        .display_name =
            rule.display_name ? std::string_view{*rule.display_name}
                              : std::string_view{},
        .definition_index = definition_index,
        .original_base_points = original,
        .final_base_points = rule.base_points.value_or(original),
    };
  }
  return {};
}

DisplayNameOverride FindDisplayNameOverride(uint32_t definition_index) {
  std::call_once(g_load_once, LoadOverrideFile);
  std::lock_guard lock(g_display_name_mutex);
  const auto found =
      g_display_name_rules_by_definition_index.find(definition_index);
  if (found == g_display_name_rules_by_definition_index.end() ||
      found->second >= g_loaded_scorable_rules.size()) {
    return {};
  }
  const auto& rule = g_loaded_scorable_rules[found->second];
  if (!rule.display_name) {
    return {};
  }
  return {
      .applied = true,
      .rule_name = rule.name,
      .scorable_name = rule.scorable_name,
      .display_name = *rule.display_name,
  };
}

AnimationOverrideResult ApplyAnimationOverride(
    PPCContext& source_context, uint8_t* guest_base,
    uint32_t selected_descriptor_address) {
  if (!guest_base || !selected_descriptor_address) {
    return {};
  }
  uint8_t* base = guest_base;

  std::call_once(g_load_once, LoadOverrideFile);
  if (g_loaded_animation_rules.empty()) {
    return {};
  }

  const auto selected =
      LoadDescriptor(base, selected_descriptor_address);
  for (const auto& rule : g_loaded_animation_rules) {
    const auto source = ConstructAnimationDescriptor(
        source_context, base, rule.source_animation, 0x80);
    if (selected != source) {
      continue;
    }
    const auto target = ConstructAnimationDescriptor(
        source_context, base, rule.target_animation, 0x100);
    if (target.empty()) {
      return {};
    }
    StoreDescriptor(base, selected_descriptor_address, target);
    return {
        .applied = true,
        .rule_name = rule.name,
        .source_animation = rule.source_animation,
        .target_animation = rule.target_animation,
        .original_descriptor = selected,
        .final_descriptor = target,
    };
  }
  return {};
}

std::optional<AnimationAssetRuleView> FindAnimationAsset(
    std::string_view animation) {
  std::call_once(g_load_once, LoadOverrideFile);
  for (const auto& rule : g_loaded_animation_asset_rules) {
    if (rule.animation == animation) {
      return AnimationAssetRuleView{
          .name = rule.name,
          .animation = rule.animation,
          .database_name = rule.database_name,
          .path = rule.path,
          .playback_data = rule.playback_data,
          .memory_group = rule.memory_group,
          .load_only = rule.load_only,
      };
    }
  }
  return std::nullopt;
}

std::optional<AnimationAssetRuleView> FindAnimationAssetByIndex(
    uint32_t index) {
  std::call_once(g_load_once, LoadOverrideFile);
  if (index >= g_loaded_animation_asset_rules.size()) {
    return std::nullopt;
  }
  const auto& rule = g_loaded_animation_asset_rules[index];
  return AnimationAssetRuleView{
      .name = rule.name,
      .animation = rule.animation,
      .database_name = rule.database_name,
      .path = rule.path,
      .playback_data = rule.playback_data,
      .memory_group = rule.memory_group,
      .load_only = rule.load_only,
  };
}

std::optional<AnimationLeafReplacementRuleView>
FindAnimationLeafReplacementByIndex(uint32_t index) {
  std::call_once(g_load_once, LoadOverrideFile);
  if (index >= g_loaded_animation_leaf_replacement_rules.size()) {
    return std::nullopt;
  }
  const auto& rule = g_loaded_animation_leaf_replacement_rules[index];
  return AnimationLeafReplacementRuleView{
      .name = rule.name,
      .source_animation = rule.source_animation,
      .target_animation = rule.target_animation,
      .asset = rule.asset,
  };
}

bool KickflipReplacementEnabled() {
  return REXCVAR_GET(skate3_trick_replace_kickflip_with_heelflip);
}

bool KickflipDisableEnabled() {
  return REXCVAR_GET(skate3_trick_disable_kickflip);
}

bool OverrideFileConfigured() {
  return g_file_configured.load(std::memory_order_acquire);
}

bool OverrideFileLoaded() {
  return g_file_status.load(std::memory_order_acquire) ==
         FileStatus::Loaded;
}

uint32_t LoadedRuleCount() {
  return g_loaded_rule_count.load(std::memory_order_acquire);
}

uint32_t LoadedScorableRuleCount() {
  return g_loaded_scorable_rule_count.load(std::memory_order_acquire);
}

uint32_t LoadedAnimationRuleCount() {
  return g_loaded_animation_rule_count.load(std::memory_order_acquire);
}

uint32_t LoadedAnimationAssetRuleCount() {
  std::call_once(g_load_once, LoadOverrideFile);
  return g_loaded_animation_asset_rule_count.load(
      std::memory_order_acquire);
}

uint32_t LoadedAnimationLeafReplacementRuleCount() {
  std::call_once(g_load_once, LoadOverrideFile);
  return g_loaded_animation_leaf_replacement_rule_count.load(
      std::memory_order_acquire);
}

std::string_view OverrideFileStatus() {
  switch (g_file_status.load(std::memory_order_acquire)) {
    case FileStatus::NotConfigured:
      return "not-configured";
    case FileStatus::Loaded:
      return "loaded";
    case FileStatus::ParseError:
      return "parse-error";
    case FileStatus::SchemaError:
      return "schema-error";
    case FileStatus::IoError:
      return "io-error";
  }
  return "unknown";
}

}  // namespace skate3::trick_overrides
