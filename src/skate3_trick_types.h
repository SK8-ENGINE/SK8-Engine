#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace skate3::trick {

// Guest objects are big-endian and must still be read through REX_LOAD_*.
// These types describe semantic shape and verified offsets; they are not
// direct host-memory overlays.

struct TrickIntentDescriptor {
  static constexpr size_t kWordCount = 6;
  static constexpr size_t kSizeBytes = 24;

  std::array<uint32_t, kWordCount> words{};

  [[nodiscard]] bool empty() const noexcept {
    for (uint32_t word : words) {
      if (word != 0) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(const TrickIntentDescriptor&,
                         const TrickIntentDescriptor&) = default;
};

struct TrickIntents {
  static constexpr size_t kWordCount = 13;
  static constexpr size_t kSizeBytes = 52;

  TrickIntentDescriptor primary{};
  TrickIntentDescriptor secondary{};
  uint32_t trailing_word{};
};

static_assert(sizeof(TrickIntentDescriptor) ==
              TrickIntentDescriptor::kSizeBytes);
static_assert(sizeof(TrickIntents) == TrickIntents::kSizeBytes);

struct GestureTrickMappingLayout {
  // GetGestureIntents accepts groups 0..6 and selects one fixed EASTL
  // FastString<36> -> const TrickIntents* hash table per group.
  static constexpr uint32_t kGroupCount = 7;
  static constexpr uint32_t kGroupTableStride = 1892;
  static constexpr uint32_t kObjectSizeBytes =
      kGroupCount * kGroupTableStride;

  static constexpr uint32_t GroupTableOffset(uint32_t group) noexcept {
    // Retail's switch routes enum 0 to physical table 6, then enum 1..6
    // to physical tables 0..5.
    return group == 0 ? 6 * kGroupTableStride
                      : (group - 1) * kGroupTableStride;
  }
};

struct ActionGraphInputListenerLayout {
  // Actor::Actor allocates 480 bytes, passes the Actor as r4 and player index
  // as r5, then stores the constructed listener at Actor+1792.
  static constexpr uint32_t kPlayerIndex = 0;
  static constexpr uint32_t kActor = 4;
  static constexpr uint32_t kFirstActionGraphEvent = 408;
  static constexpr uint32_t kActionGraphEventStride = 12;
  static constexpr uint32_t kActionGraphEventCount = 5;
  static constexpr uint32_t kSizeBytes = 480;
};

struct GestureMappingValueLayout {
  // GestureTrickMapping::Init allocates this exact size, constructs one
  // 24-byte descriptor at +0 and another at +24, then writes +48.
  static constexpr uint32_t kPrimaryDescriptor = 0;
  static constexpr uint32_t kSecondaryDescriptor = 24;
  static constexpr uint32_t kTrailingWord = 48;
  static constexpr uint32_t kSizeBytes = 52;
};

struct CreateTrickIntentInstanceLayout {
  static constexpr uint32_t kActive = 9;
  static constexpr uint32_t kDescriptor = 12;
};

struct ScoreModuleLayout {
  static constexpr uint32_t kPhysOut = 0;
  static constexpr uint32_t kActiveCollector = 4;
  static constexpr uint32_t kAirCollector = 8;
  static constexpr uint32_t kGrindCollector = 12;
  static constexpr uint32_t kGroundCollector = 16;
  static constexpr uint32_t kHandplantCollector = 20;
  static constexpr uint32_t kOffboardCollector = 24;
  static constexpr uint32_t kOtherCollector = 28;
  static constexpr uint32_t kScoreHolder = 36;
  static constexpr uint32_t kCollectorState = 56;
};

enum class ScoreCollectorState : uint32_t {
  Ground = 0,
  Air = 1,
  Grind = 2,
  Other3 = 3,
  Other4 = 4,
  Offboard = 5,
  Handplant = 6,
};

struct ScoreHolderLayout {
  static constexpr uint32_t kCumulativePublishedAirReward = 28;
  // RewaredAirSequence adds
  // (pending component 0 + pending component 1) * multiplier here.
  static constexpr uint32_t kAirSequenceRewardAccumulator = 32;
  static constexpr uint32_t kLastAirSequenceReward = 36;
  static constexpr uint32_t kPendingGeneralReward = 40;
  static constexpr uint32_t kPendingFingerFlipReward = 44;
  // Compatibility aliases for existing telemetry field ordering.
  static constexpr uint32_t kPendingRewardComponent0 =
      kPendingGeneralReward;
  static constexpr uint32_t kPendingRewardComponent1 =
      kPendingFingerFlipReward;
  // GrindCollector::Exit snapshots its current grind reward and transfers it
  // here. The air-sequence publish/reset boundary emits and clears it.
  static constexpr uint32_t kPendingGrindReward = 4776;
  // Compatibility alias retained for older telemetry/evidence readers.
  static constexpr uint32_t kPendingAuxiliaryReward =
      kPendingGrindReward;
  static constexpr uint32_t kPendingRewardCount = 7256;
  static constexpr uint32_t kAirSequenceRewardPending = 7260;
};

enum class TrickPatternClass : uint32_t {
  None = 0,
  Air = 1,
  Flip = 2,
  FingerFlip = 3,
  Grab = 4,
  Grind = 5,
  Handplant = 6,
  NoComply = 7,
  Manual = 8,
  Slide = 9,
  HippyJump = 10,
  Revert = 11,
  Boneless = 12,
  Footplant = 13,
};

struct ScorableMetadataTableLayout {
  // TU3's fixed 332-entry definition table. EndAirTrick indexes this table
  // directly by EScorableID and branches on the pattern-class word at +12.
  static constexpr uint32_t kGuestAddress = 0x820862A8;
  static constexpr uint32_t kEntryCount = 332;
  static constexpr uint32_t kEntryStride = 24;
  static constexpr uint32_t kId = 0;
  static constexpr uint32_t kPatternClass = 12;
  static constexpr uint32_t kName = 20;

  static constexpr uint32_t EntryAddress(uint32_t id) noexcept {
    return kGuestAddress + id * kEntryStride;
  }
};

struct AirCollectorLayout {
  static constexpr uint32_t kCurrentScorableId = 52;
  static constexpr uint32_t kCurrentScorable = 584;
  static constexpr uint32_t kCurrentScorableActive = 708;
  static constexpr uint32_t kEndAirValue = 760;
};

struct GrindCollectorLayout {
  static constexpr uint32_t kScoreHolder = 12;
  static constexpr uint32_t kCurrentReward = 340;
  static constexpr uint32_t kPublishedRewardSnapshot = 408;
};

struct ScorableLayout {
  // sub_82DA2340 copies fields from the resolved trick-definition record.
  // Names remain deliberately structural until matched runtime experiments
  // establish each field's gameplay meaning.
  static constexpr uint32_t kDefinitionWord0 = 0;
  static constexpr uint32_t kBasePointValue = 4;
  static constexpr uint32_t kValue = kBasePointValue;
  static constexpr uint32_t kDefinitionWord8 = 8;
  static constexpr uint32_t kId = 12;
  static constexpr uint32_t kDefinitionBlocks = 16;
  static constexpr uint32_t kDefinitionBlockCount = 6;
  static constexpr uint32_t kDefinitionBlockSizeBytes = 16;
  static constexpr uint32_t kDefinitionWord112 = 112;
  static constexpr uint32_t kDefinitionWord120 = 120;
  static constexpr uint32_t kActive = 124;
  static constexpr uint32_t kDefinitionWord128 = 128;
  // This word is resolved through an interface/GUID lookup in
  // sub_82DA2340. RefreshTrickDisplay does not read it, so it must not be
  // labeled as the display name/resource without further evidence.
  static constexpr uint32_t kDefinitionWord132 = 132;
  static constexpr uint32_t kDefinitionWord136 = 136;
  static constexpr uint32_t kDefinitionWord140 = 140;
  static constexpr uint32_t kDefinitionFloat144 = 144;
  static constexpr uint32_t kDefinitionWord148 = 148;
  static constexpr uint32_t kNameHash = 152;
  static constexpr uint32_t kDefinitionWord160 = 160;
  static constexpr uint32_t kCalculatedValue = 168;
  static constexpr uint32_t kStartValue = 172;
  static constexpr uint32_t kAccumulatedValue = 176;
  static constexpr uint32_t kStartWord180 = 180;
  static constexpr uint32_t kCalculatedWord184 = 184;
  static constexpr uint32_t kRuntimeFlags = 188;
  static constexpr uint32_t kRuntimeFlagCount = 6;
  static constexpr uint32_t kSizeBytes = 200;
};

static_assert(AirCollectorLayout::kCurrentScorable +
                  ScorableLayout::kId ==
              596);
static_assert(AirCollectorLayout::kCurrentScorable +
                  ScorableLayout::kActive ==
              AirCollectorLayout::kCurrentScorableActive);

struct AnimationConditionerLayout {
  static constexpr uint32_t kPhysOut = 4;
  // PhysOutConditioner_Animation caches the previous 60-byte producer
  // record here so UpdateCompletedTrick can detect descriptor transitions.
  static constexpr uint32_t kPreviousAnimOutTrickData = 8;
};

struct PhysOutLayout {
  static constexpr uint32_t kTrick = 48;
  static constexpr uint32_t kTrickState = kTrick;
};

struct AnimOutTrickDataLayout {
  // Verified from the constructor, Reset, and copy-assignment functions at
  // 0x8258E2A0, 0x8258E360, and 0x82596008. Semantic labels for the trailing
  // word/floats remain structural until matched experiments establish them.
  static constexpr uint32_t kCurrentDescriptor = 0;
  static constexpr uint32_t kCompletedDescriptor = 24;
  static constexpr uint32_t kWord48 = 48;
  static constexpr uint32_t kFloat52 = 52;
  static constexpr uint32_t kFloat56 = 56;
  static constexpr uint32_t kSizeBytes = 60;
};

struct PhysOutTrickLayout {
  // UpdateCurrentTrick publishes the filtered current descriptor at +0.
  // UpdateCompletedTrick publishes a detected transition at +24, copies the
  // complete producer record at +48, and marks +124 when the completed
  // descriptor is valid for this output frame.
  static constexpr uint32_t kCurrentDescriptor = 0;
  static constexpr uint32_t kCompletedDescriptor = 24;
  static constexpr uint32_t kAnimOutTrickData = 48;
  static constexpr uint32_t kWord108 = 108;
  static constexpr uint32_t kFlag112 = 112;
  static constexpr uint32_t kFlag113 = 113;
  static constexpr uint32_t kFloat116 = 116;
  static constexpr uint32_t kFloat120 = 120;
  static constexpr uint32_t kCompletedDescriptorValid = 124;
  static constexpr uint32_t kFlag125 = 125;
  static constexpr uint32_t kSizeBytes = 128;
};

struct AnimationTrickStateLayout {
  // Compatibility aliases for the original observer and harness field order.
  static constexpr uint32_t kPublishedDescriptor =
      PhysOutTrickLayout::kCompletedDescriptor;
  static constexpr uint32_t kLiveDescriptor =
      PhysOutTrickLayout::kAnimOutTrickData +
      AnimOutTrickDataLayout::kCurrentDescriptor;
  static constexpr uint32_t kActive =
      PhysOutTrickLayout::kCompletedDescriptorValid;
};

}  // namespace skate3::trick
