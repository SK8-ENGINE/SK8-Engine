#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>

struct PPCContext;

namespace skate3::trick_pipeline {

struct LiveSpatialSnapshot {
  uint64_t frame{};
  uint32_t phys_out{};
  uint32_t board_controller{};
  uint32_t board_body{};
  uint32_t transform_state{};
  std::array<uint32_t, 3> position_bits{};
  std::array<uint32_t, 3> x_axis_bits{};
  std::array<uint32_t, 3> z_axis_bits{};
  uint32_t board_state_flags{0xFFFFFFFFu};
};

// Player-0 ownership captured at the ActionGraphInputListener fill boundary.
// This is intentionally distinct from the actor-global ground predicate.
uint32_t LocalActionGraphActor();

// Research-only matched experiment boundary. SetTrickHeight has completed all
// retail charge/intent calculations when this is called and is about to
// publish the final float to the state-graph attribute map.
void OverrideTrickHeightForMatchedExperiment(PPCContext& ctx);

// Selects the project-owned motion graph for the experimental native custom
// trick route. The returned guest pointer remains valid for the synchronous
// StateGraph load performed by SkaterMotionGraph initialization.
uint32_t SelectMotionGraphPath(PPCContext& ctx, uint8_t* base,
                               uint32_t retail_path);
bool NativeCustomTrickGraphEnabled();
bool ShouldForceTextStateGraph(uint8_t* base, uint32_t normalized_path);
void ObserveStateGraphIncludePath(uint8_t* base, uint32_t path);
void FilterTrackedOuterCircleIntentQuery(PPCContext& ctx, uint8_t* base,
                                         uint32_t intent_name);

class HasIntentConditionScope {
 public:
  HasIntentConditionScope(PPCContext& ctx, uint8_t* base);
  ~HasIntentConditionScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t intent_name_;
};

class ActionGraphInputFillObservationScope {
 public:
  ActionGraphInputFillObservationScope(PPCContext& ctx, uint8_t* base);
  ~ActionGraphInputFillObservationScope();

  ActionGraphInputFillObservationScope(
      const ActionGraphInputFillObservationScope&) = delete;
  ActionGraphInputFillObservationScope& operator=(
      const ActionGraphInputFillObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t previous_depth_;
  uint32_t previous_listener_;
  uint32_t previous_actor_;
  uint32_t previous_intents_;
};

class ActionGraphIntentInsertObservationScope {
 public:
  ActionGraphIntentInsertObservationScope(PPCContext& ctx, uint8_t* base);
  ~ActionGraphIntentInsertObservationScope();

  ActionGraphIntentInsertObservationScope(
      const ActionGraphIntentInsertObservationScope&) = delete;
  ActionGraphIntentInsertObservationScope& operator=(
      const ActionGraphIntentInsertObservationScope&) = delete;

  bool suppressed() const { return suppressed_; }

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t listener_;
  uint32_t actor_;
  uint32_t intents_;
  uint32_t name_;
  uint32_t caller_;
  uint32_t value_bits_;
  bool suppressed_;
  bool focused_;
};

class GestureMappingInitObservationScope {
 public:
  GestureMappingInitObservationScope(PPCContext& ctx, uint8_t* base);
  ~GestureMappingInitObservationScope();

  GestureMappingInitObservationScope(
      const GestureMappingInitObservationScope&) = delete;
  GestureMappingInitObservationScope& operator=(
      const GestureMappingInitObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint32_t mapping_;
};

class FastStringMappingObservationScope {
 public:
  FastStringMappingObservationScope(PPCContext& ctx, uint8_t* base);
  ~FastStringMappingObservationScope();

  FastStringMappingObservationScope(
      const FastStringMappingObservationScope&) = delete;
 FastStringMappingObservationScope& operator=(
      const FastStringMappingObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t destination_;
  uint32_t source_;
  bool active_;
};

// RAII is intentional: generated functions have multiple retail return sites.
// A scope records entry registers and observes the final return/output state
// without changing any branch or return path.
class GestureIntentObservationScope {
 public:
  GestureIntentObservationScope(PPCContext& ctx, uint8_t* base);
  ~GestureIntentObservationScope();

  GestureIntentObservationScope(const GestureIntentObservationScope&) = delete;
  GestureIntentObservationScope& operator=(
      const GestureIntentObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t mapping_;
  uint32_t intents_;
  uint32_t group_;
  uint32_t output_;
  uint32_t caller_;
  uint32_t previous_active_group_;
  bool armed_;
  bool focused_;
};

class CreateTrickIntentObservationScope {
 public:
  CreateTrickIntentObservationScope(PPCContext& ctx, uint8_t* base);
  ~CreateTrickIntentObservationScope();

  CreateTrickIntentObservationScope(
      const CreateTrickIntentObservationScope&) = delete;
  CreateTrickIntentObservationScope& operator=(
      const CreateTrickIntentObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t behavior_;
  uint32_t context_;
  uint32_t state_context_;
  uint32_t instance_;
  uint32_t group_;
  uint32_t caller_;
  bool armed_;
  bool focused_;
};

// Observes the collector-state transition helper at 0x82DA4010. The helper
// computes a retail state, exits the old collector, and enters the collector
// selected from ScoreModule's typed collector slots.
class ScoreCollectorTransitionObservationScope {
 public:
  ScoreCollectorTransitionObservationScope(PPCContext& ctx, uint8_t* base);
  ~ScoreCollectorTransitionObservationScope();

  ScoreCollectorTransitionObservationScope(
      const ScoreCollectorTransitionObservationScope&) = delete;
  ScoreCollectorTransitionObservationScope& operator=(
      const ScoreCollectorTransitionObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t module_;
  uint32_t caller_;
  uint32_t holder_;
  uint32_t phys_out_;
  uint32_t old_state_;
  uint32_t old_collector_;
  uint32_t old_vtable_;
};

// Observes the complete Scorable state produced by sub_82DA45C8. The
// constructor snapshots call arguments while the destructor runs after the
// retail metadata lookup and runtime-field initialization have completed.
class ScorableStartObservationScope {
 public:
  ScorableStartObservationScope(PPCContext& ctx, uint8_t* base);
  ~ScorableStartObservationScope();

  ScorableStartObservationScope(const ScorableStartObservationScope&) =
      delete;
  ScorableStartObservationScope& operator=(
      const ScorableStartObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t scorable_;
  uint32_t name_;
  uint32_t context_argument_;
  uint32_t phys_out_;
  uint32_t start_word_;
  uint32_t caller_;
  uint32_t start_value_bits_;
  uint32_t definition_adjustment_bits_;
  bool armed_;
  bool focused_;
};

class CustomScorableNameResolutionScope {
 public:
  CustomScorableNameResolutionScope(PPCContext& ctx, uint8_t* base);
  ~CustomScorableNameResolutionScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t scorable_;
  uint32_t original_name_;
  bool custom_;
};

class CustomScorableIdResolutionScope {
 public:
  CustomScorableIdResolutionScope(PPCContext& ctx, uint8_t* base);
  ~CustomScorableIdResolutionScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t scorable_;
  uint32_t original_id_;
  bool custom_;
};

bool TryReturnCustomScorableName(PPCContext& ctx, uint8_t* base);

// EScorable metadata remains a fixed 332-entry retail table. These adapters
// temporarily expose the flip-class template only while AirCollector reads
// that table; the collector and constructed Scorable retain the custom ID.
class CustomAirCollectorUpdateScope {
 public:
  CustomAirCollectorUpdateScope(PPCContext& ctx, uint8_t* base);
  ~CustomAirCollectorUpdateScope();

 private:
  uint8_t* base_;
  uint32_t collector_;
  uint32_t custom_id_;
  uint32_t previous_thread_custom_id_;
  bool mapped_current_;
  bool mapped_active_;
};

void PromoteCustomScorableForAirCollectorStart(PPCContext& ctx);

class CustomAirCollectorStartMetadataScope {
 public:
  CustomAirCollectorStartMetadataScope(uint8_t* base, uint32_t collector);
  ~CustomAirCollectorStartMetadataScope();

 private:
  uint8_t* base_;
  uint32_t collector_;
  uint32_t custom_id_;
  bool mapped_;
};

class PointPenaltyObservationScope {
 public:
  PointPenaltyObservationScope(PPCContext& ctx, uint8_t* base);
  ~PointPenaltyObservationScope();

  PointPenaltyObservationScope(const PointPenaltyObservationScope&) = delete;
  PointPenaltyObservationScope& operator=(
      const PointPenaltyObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t holder_;
  int32_t retail_id_;
  uint8_t retail_count_;
  bool adapted_;
};

class CustomRepetitionEndAirScope {
 public:
  CustomRepetitionEndAirScope(PPCContext& ctx, uint8_t* base);
  ~CustomRepetitionEndAirScope();

  CustomRepetitionEndAirScope(const CustomRepetitionEndAirScope&) = delete;
  CustomRepetitionEndAirScope& operator=(
      const CustomRepetitionEndAirScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t holder_;
  int32_t retail_id_;
  uint8_t retail_id_count_;
  uint8_t retail_secondary_count_;
  uint32_t group_index_;
  uint8_t retail_group_count_;
  bool adapted_;
};

// Captures the complete ScoreHolder::EndAirTrick component routing. TU3
// metadata class FingerFlip (3) accumulates in the isolated +44 bucket,
// Grind (5) preserves that bucket, and every other class flushes it into +40
// before accumulating its own value.
class ScoreHolderEndAirObservationScope {
 public:
  ScoreHolderEndAirObservationScope(PPCContext& ctx, uint8_t* base);
  ~ScoreHolderEndAirObservationScope();

  ScoreHolderEndAirObservationScope(
      const ScoreHolderEndAirObservationScope&) = delete;
  ScoreHolderEndAirObservationScope& operator=(
      const ScoreHolderEndAirObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t holder_;
  uint32_t caller_;
  int32_t retail_id_;
  uint32_t value_bits_;
  uint32_t pattern_class_;
  uint32_t component_a_before_bits_;
  uint32_t component_b_before_bits_;
};

// Observes the map-named ScoreHolder::RewaredAirSequence function. The
// community spelling is retained as provenance; entry/exit snapshots recover
// the numeric air-sequence accumulator and pending-component dataflow.
class ScoreHolderRewardAirSequenceObservationScope {
 public:
  ScoreHolderRewardAirSequenceObservationScope(PPCContext& ctx,
                                                uint8_t* base);
  ~ScoreHolderRewardAirSequenceObservationScope();

  ScoreHolderRewardAirSequenceObservationScope(
      const ScoreHolderRewardAirSequenceObservationScope&) = delete;
  ScoreHolderRewardAirSequenceObservationScope& operator=(
      const ScoreHolderRewardAirSequenceObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t holder_;
  uint32_t caller_;
  uint32_t multiplier_bits_;
  uint32_t accumulator_before_bits_;
  uint32_t component_a_before_bits_;
  uint32_t component_b_before_bits_;
  uint32_t pending_count_before_;
  uint8_t pending_before_;
};

// Local-name hypothesis for unnamed 0x82DA6538. Static dataflow publishes f1
// into ScoreHolder/UI output, snapshots repetition histories, and resets the
// current air-sequence reward fields. Runtime entry/exit evidence is required
// before promoting the name.
class ScoreHolderPublishAirSequenceObservationScope {
 public:
  ScoreHolderPublishAirSequenceObservationScope(PPCContext& ctx,
                                                 uint8_t* base);
  ~ScoreHolderPublishAirSequenceObservationScope();

  ScoreHolderPublishAirSequenceObservationScope(
      const ScoreHolderPublishAirSequenceObservationScope&) = delete;
  ScoreHolderPublishAirSequenceObservationScope& operator=(
      const ScoreHolderPublishAirSequenceObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t holder_;
  uint32_t caller_;
  uint32_t published_reward_bits_;
  uint32_t cumulative_before_bits_;
  uint32_t accumulator_before_bits_;
  uint32_t previous_last_reward_bits_;
  uint32_t grind_before_bits_;
  bool add_to_cumulative_;
};

// Observes GrindCollector vtable slot 4 (unnamed 0x82DA9B08). ScoreModule's
// transition dispatcher calls it when leaving Grind; a true exit snapshots
// GrindCollector+340 and transfers that reward into ScoreHolder+4776.
class GrindCollectorExitObservationScope {
 public:
  GrindCollectorExitObservationScope(PPCContext& ctx, uint8_t* base);
  ~GrindCollectorExitObservationScope();

  GrindCollectorExitObservationScope(
      const GrindCollectorExitObservationScope&) = delete;
  GrindCollectorExitObservationScope& operator=(
      const GrindCollectorExitObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t collector_;
  uint32_t holder_;
  uint32_t caller_;
  uint32_t current_before_bits_;
  uint32_t snapshot_before_bits_;
  uint32_t grind_before_bits_;
  bool publish_;
};

// Observes community-named PhysicalPlayerHiLOD::IsWipeoutRequested. The
// generated function has multiple returns, so the destructor captures the
// final boolean without altering the retail decision.
class WipeoutRequestedObservationScope {
 public:
  WipeoutRequestedObservationScope(PPCContext& ctx, uint8_t* base);
  ~WipeoutRequestedObservationScope();

  WipeoutRequestedObservationScope(
      const WipeoutRequestedObservationScope&) = delete;
  WipeoutRequestedObservationScope& operator=(
      const WipeoutRequestedObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint64_t frame_;
  uint32_t player_;
  uint32_t caller_;
  bool focused_;
};

// Observes community-named Skeleton::CheckForCollisionForceWipeout. The
// destructor captures the retail boolean result and preserves it unchanged.
class CollisionForceWipeoutObservationScope {
 public:
  CollisionForceWipeoutObservationScope(PPCContext& ctx, uint8_t* base);
  ~CollisionForceWipeoutObservationScope();

  CollisionForceWipeoutObservationScope(
      const CollisionForceWipeoutObservationScope&) = delete;
  CollisionForceWipeoutObservationScope& operator=(
      const CollisionForceWipeoutObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint64_t frame_;
  uint32_t skeleton_;
  uint32_t argument_1_bits_;
  uint32_t argument_2_bits_;
  uint32_t caller_;
  bool focused_;
};

// Observes, without modifying, the native per-frame animation attributes
// consumed by Skeleton::ProcessAnimAttributes. These records are the contract
// between Andale animation playback and the retail skeleton/physics
// conditioners (landing adjustment, skateboard offset, and related flags).
class SkeletonAnimAttributesObservationScope {
 public:
  SkeletonAnimAttributesObservationScope(PPCContext& ctx, uint8_t* base);
  ~SkeletonAnimAttributesObservationScope() = default;

  SkeletonAnimAttributesObservationScope(
      const SkeletonAnimAttributesObservationScope&) = delete;
  SkeletonAnimAttributesObservationScope& operator=(
      const SkeletonAnimAttributesObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t skeleton_;
  bool focused_;
};

// Samples the compact skater's final animation matrices at the
// Skeleton::ProcessData boundary. AnimOutPhysIn+0 points at the 64-byte
// affine-matrix pose buffer after animation tree blending.
class CacGestureFinalPoseObservationScope {
 public:
  CacGestureFinalPoseObservationScope(PPCContext& ctx, uint8_t* base);

  CacGestureFinalPoseObservationScope(
      const CacGestureFinalPoseObservationScope&) = delete;
  CacGestureFinalPoseObservationScope& operator=(
      const CacGestureFinalPoseObservationScope&) = delete;
};

// Observes the retail Actor::SetUpBlendPoseStream boundary where an animation
// tree has already selected its concrete leaf Clip. This exposes the exact
// vanilla tree/leaf pairing without changing playback.
class BlendPoseStreamObservationScope {
 public:
  BlendPoseStreamObservationScope(PPCContext& ctx, uint8_t* base);
  ~BlendPoseStreamObservationScope() = default;

  BlendPoseStreamObservationScope(const BlendPoseStreamObservationScope&) =
      delete;
  BlendPoseStreamObservationScope& operator=(
      const BlendPoseStreamObservationScope&) = delete;
};

class AnimationTreeLookupObservationScope {
 public:
  AnimationTreeLookupObservationScope(PPCContext& ctx, uint8_t* base);
  ~AnimationTreeLookupObservationScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t playback_;
  uint32_t name_;
  uint32_t caller_;
  bool relevant_;
};

class PushAnimationAttributesObservationScope {
 public:
  PushAnimationAttributesObservationScope(PPCContext& ctx, uint8_t* base,
                                          const char* tag);
  ~PushAnimationAttributesObservationScope() = default;

 private:
  const char* tag_;
};

// Observes the vanilla skeleton-IK blend state for the four contact targets.
// In TU3, BlendTransforms reads four weights at +468 and four corresponding
// states at +500. This is read-only evidence for when feet are planted,
// released, or blended back to the board.
class SkeletonIkBlendObservationScope {
 public:
  SkeletonIkBlendObservationScope(PPCContext& ctx, uint8_t* base);
  ~SkeletonIkBlendObservationScope();

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t skeleton_ik_;
  bool focused_;
  std::array<uint32_t, 160> before_matrices_{};
};

// Captures the two Matrix44Affine candidates passed from
// SkeletonIK::BlendTransforms to rw::math::vpu::SLerp. Only the two TU3 call
// sites inside the focused foot-target branch are retained.
class SkeletonIkMatrixSlerpObservationScope {
 public:
  SkeletonIkMatrixSlerpObservationScope(PPCContext& ctx, uint8_t* base);
  ~SkeletonIkMatrixSlerpObservationScope();

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t caller_;
  uint32_t result_;
  uint32_t first_;
  uint32_t second_;
  uint32_t weight_;
  bool focused_;
  std::array<uint32_t, 24> candidates_{};
};

// Observes Skeleton::UpdateSkateboardOffsetTransform before and after native
// processing. The two weights/flags at +16392/+16389 and +16400/+16396 gate
// the retail per-foot skateboard offset path.
class SkateboardOffsetObservationScope {
 public:
  SkateboardOffsetObservationScope(PPCContext& ctx, uint8_t* base);
  ~SkateboardOffsetObservationScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t skeleton_;
  bool focused_;
  uint32_t before_weight_0_;
  uint32_t before_weight_1_;
  uint8_t before_flag_0_;
  uint8_t before_flag_1_;
  std::array<uint32_t, 112> before_matrices_{};
};

// Captures the factory object registered by the community-named
// PhysicsWantsWipeOut condition initializer.
class PhysicsWantsWipeoutConditionFactoryObservationScope {
 public:
  PhysicsWantsWipeoutConditionFactoryObservationScope(PPCContext& ctx,
                                                       uint8_t* base);
  ~PhysicsWantsWipeoutConditionFactoryObservationScope();

  PhysicsWantsWipeoutConditionFactoryObservationScope(
      const PhysicsWantsWipeoutConditionFactoryObservationScope&) = delete;
  PhysicsWantsWipeoutConditionFactoryObservationScope& operator=(
      const PhysicsWantsWipeoutConditionFactoryObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

// Observes the exact evaluator installed by PhysicsWantsWipeOut's verified
// factory vtable.
class PhysicsWantsWipeoutConditionEvaluationScope {
 public:
  PhysicsWantsWipeoutConditionEvaluationScope(PPCContext& ctx, uint8_t* base);
  ~PhysicsWantsWipeoutConditionEvaluationScope();

  PhysicsWantsWipeoutConditionEvaluationScope(
      const PhysicsWantsWipeoutConditionEvaluationScope&) = delete;
  PhysicsWantsWipeoutConditionEvaluationScope& operator=(
      const PhysicsWantsWipeoutConditionEvaluationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t context_;
  uint32_t actor_;
  uint32_t skater_anim_interface_;
  bool focused_;
};

// Captures the condition objects and vtables returned by the map-named
// CanLandOnBoard and HasTiltToLargeForPreland factories. Their evaluator
// slots are promoted only after runtime/static verification.
class CanLandOnBoardConditionFactoryObservationScope {
 public:
  CanLandOnBoardConditionFactoryObservationScope(PPCContext& ctx,
                                                  uint8_t* base);
  ~CanLandOnBoardConditionFactoryObservationScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

class TiltTooLargeForPrelandConditionFactoryObservationScope {
 public:
  TiltTooLargeForPrelandConditionFactoryObservationScope(PPCContext& ctx,
                                                          uint8_t* base);
  ~TiltTooLargeForPrelandConditionFactoryObservationScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

class IsLandingOnBoardConditionFactoryObservationScope {
 public:
  IsLandingOnBoardConditionFactoryObservationScope(PPCContext& ctx,
                                                    uint8_t* base);
  ~IsLandingOnBoardConditionFactoryObservationScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

class IsLandingConditionFactoryObservationScope {
 public:
  IsLandingConditionFactoryObservationScope(PPCContext& ctx, uint8_t* base);
  ~IsLandingConditionFactoryObservationScope();

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

class CanLandOnBoardConditionEvaluationScope {
 public:
  CanLandOnBoardConditionEvaluationScope(PPCContext& ctx, uint8_t* base);
  ~CanLandOnBoardConditionEvaluationScope();

 private:
  PPCContext& ctx_;
  uint64_t frame_;
  uint32_t context_;
  uint32_t actor_;
};

class TiltTooLargeForPrelandConditionEvaluationScope {
 public:
  TiltTooLargeForPrelandConditionEvaluationScope(PPCContext& ctx,
                                                  uint8_t* base);
  ~TiltTooLargeForPrelandConditionEvaluationScope();

 private:
  PPCContext& ctx_;
  uint64_t frame_;
  uint32_t context_;
  uint32_t actor_;
  uint32_t tilt_interface_;
};

class IsLandingOnBoardConditionEvaluationScope {
 public:
  IsLandingOnBoardConditionEvaluationScope(PPCContext& ctx, uint8_t* base);
  ~IsLandingOnBoardConditionEvaluationScope();

 private:
  PPCContext& ctx_;
  uint64_t frame_;
  uint32_t context_;
  uint32_t actor_;
};

class IsLandingConditionEvaluationScope {
 public:
  IsLandingConditionEvaluationScope(PPCContext& ctx, uint8_t* base,
                                    bool on_board_mode);
  ~IsLandingConditionEvaluationScope();

 private:
  PPCContext& ctx_;
  uint64_t frame_;
  uint32_t context_;
  uint32_t actor_;
  uint32_t landing_interface_;
  bool on_board_mode_;
};

// Captures the runtime vtable installed by the community-named camera
// IsOffboard condition factory. The map name is a hypothesis until the
// returned condition and its virtual evaluator are verified at runtime.
class IsOffboardConditionFactoryObservationScope {
 public:
  IsOffboardConditionFactoryObservationScope(PPCContext& ctx, uint8_t* base);
  ~IsOffboardConditionFactoryObservationScope();

  IsOffboardConditionFactoryObservationScope(
      const IsOffboardConditionFactoryObservationScope&) = delete;
  IsOffboardConditionFactoryObservationScope& operator=(
      const IsOffboardConditionFactoryObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

class IsAirOffboardConditionFactoryObservationScope {
 public:
  IsAirOffboardConditionFactoryObservationScope(PPCContext& ctx,
                                                 uint8_t* base);
  ~IsAirOffboardConditionFactoryObservationScope();

  IsAirOffboardConditionFactoryObservationScope(
      const IsAirOffboardConditionFactoryObservationScope&) = delete;
  IsAirOffboardConditionFactoryObservationScope& operator=(
      const IsAirOffboardConditionFactoryObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
};

class IsOffboardConditionEvaluationScope {
 public:
  IsOffboardConditionEvaluationScope(PPCContext& ctx, uint8_t* base,
                                     bool air_condition);
  ~IsOffboardConditionEvaluationScope();

  IsOffboardConditionEvaluationScope(
      const IsOffboardConditionEvaluationScope&) = delete;
  IsOffboardConditionEvaluationScope& operator=(
      const IsOffboardConditionEvaluationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t frame_;
  uint32_t condition_;
  uint32_t context_;
  uint32_t owner_;
  uint32_t provider_;
  bool air_condition_;
  bool focused_;
};

// Observes the final retail text emitted by
// FE::TrickDisplayManager::RefreshTrickDisplay. This is intentionally
// read-only while the display-name boundary is being verified.
class TrickDisplayRefreshObservationScope {
 public:
  TrickDisplayRefreshObservationScope(PPCContext& ctx, uint8_t* base);
  ~TrickDisplayRefreshObservationScope();

  TrickDisplayRefreshObservationScope(
      const TrickDisplayRefreshObservationScope&) = delete;
  TrickDisplayRefreshObservationScope& operator=(
      const TrickDisplayRefreshObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t manager_;
  uint32_t caller_;
  bool armed_;
  bool focused_;
};

// Replaces the closed retail Scorable carrier's text after Skate 3 formats it
// but before the display manager publishes it to the HUD.
class AnimationCurrentObservationScope {
 public:
  AnimationCurrentObservationScope(PPCContext& ctx, uint8_t* base);
  ~AnimationCurrentObservationScope();

  AnimationCurrentObservationScope(
      const AnimationCurrentObservationScope&) = delete;
  AnimationCurrentObservationScope& operator=(
      const AnimationCurrentObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t conditioner_;
  uint32_t caller_;
};

class AnimationCompletedObservationScope {
 public:
  AnimationCompletedObservationScope(PPCContext& ctx, uint8_t* base);
  ~AnimationCompletedObservationScope();

  AnimationCompletedObservationScope(
      const AnimationCompletedObservationScope&) = delete;
  AnimationCompletedObservationScope& operator=(
      const AnimationCompletedObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint64_t frame_;
  uint32_t conditioner_;
  uint32_t caller_;
};

class SceneAnimationLoaderAddObservationScope {
 public:
  SceneAnimationLoaderAddObservationScope(PPCContext& ctx, uint8_t* base);
  ~SceneAnimationLoaderAddObservationScope();

  SceneAnimationLoaderAddObservationScope(
      const SceneAnimationLoaderAddObservationScope&) = delete;
  SceneAnimationLoaderAddObservationScope& operator=(
      const SceneAnimationLoaderAddObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint32_t manager_;
  uint32_t scene_;
  uint32_t name_;
  uint32_t caller_;
};

class AnimationLoaderDataLookupObservationScope {
 public:
  AnimationLoaderDataLookupObservationScope(PPCContext& ctx, uint8_t* base);
  ~AnimationLoaderDataLookupObservationScope();

  AnimationLoaderDataLookupObservationScope(
      const AnimationLoaderDataLookupObservationScope&) = delete;
  AnimationLoaderDataLookupObservationScope& operator=(
      const AnimationLoaderDataLookupObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t manager_;
  uint32_t name_;
  uint32_t caller_;
};

class SceneAnimationLoaderLoadObservationScope {
 public:
  SceneAnimationLoaderLoadObservationScope(PPCContext& ctx, uint8_t* base);
  ~SceneAnimationLoaderLoadObservationScope();

  SceneAnimationLoaderLoadObservationScope(
      const SceneAnimationLoaderLoadObservationScope&) = delete;
  SceneAnimationLoaderLoadObservationScope& operator=(
      const SceneAnimationLoaderLoadObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint32_t loader_;
  uint32_t caller_;
};

class SceneAnimationLoaderPollObservationScope {
 public:
  SceneAnimationLoaderPollObservationScope(PPCContext& ctx, uint8_t* base);
  ~SceneAnimationLoaderPollObservationScope();

  SceneAnimationLoaderPollObservationScope(
      const SceneAnimationLoaderPollObservationScope&) = delete;
  SceneAnimationLoaderPollObservationScope& operator=(
      const SceneAnimationLoaderPollObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint32_t loader_;
  uint32_t caller_;
  bool was_complete_;
};

class SceneAnimationAsyncLoadObservationScope {
 public:
  SceneAnimationAsyncLoadObservationScope(PPCContext& ctx, uint8_t* base);
  ~SceneAnimationAsyncLoadObservationScope();

  SceneAnimationAsyncLoadObservationScope(
      const SceneAnimationAsyncLoadObservationScope&) = delete;
  SceneAnimationAsyncLoadObservationScope& operator=(
      const SceneAnimationAsyncLoadObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t path_;
  uint32_t caller_;
};

class PlaybackDataConstructionObservationScope {
 public:
  PlaybackDataConstructionObservationScope(PPCContext& ctx, uint8_t* base);
  ~PlaybackDataConstructionObservationScope();

  PlaybackDataConstructionObservationScope(
      const PlaybackDataConstructionObservationScope&) = delete;
  PlaybackDataConstructionObservationScope& operator=(
      const PlaybackDataConstructionObservationScope&) = delete;

 private:
  uint8_t* base_;
  uint32_t playback_data_;
  uint32_t manager_;
  uint32_t name_;
  uint32_t kind_;
  uint32_t caller_;
};

class PlaybackDataLookupObservationScope {
 public:
  PlaybackDataLookupObservationScope(PPCContext& ctx, uint8_t* base);
  ~PlaybackDataLookupObservationScope();

  PlaybackDataLookupObservationScope(
      const PlaybackDataLookupObservationScope&) = delete;
  PlaybackDataLookupObservationScope& operator=(
      const PlaybackDataLookupObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t name_;
  uint32_t caller_;
};

class AndaleDatabaseLoadObservationScope {
 public:
  AndaleDatabaseLoadObservationScope(PPCContext& ctx, uint8_t* base);
  ~AndaleDatabaseLoadObservationScope();

  AndaleDatabaseLoadObservationScope(
      const AndaleDatabaseLoadObservationScope&) = delete;
  AndaleDatabaseLoadObservationScope& operator=(
      const AndaleDatabaseLoadObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t manager_;
  uint32_t slot_;
  uint32_t allocator_;
  uint32_t path_;
  uint32_t caller_;
};

class AndaleDatabaseContentObservationScope {
 public:
  AndaleDatabaseContentObservationScope(PPCContext& ctx, uint8_t* base);
  ~AndaleDatabaseContentObservationScope();

  AndaleDatabaseContentObservationScope(
      const AndaleDatabaseContentObservationScope&) = delete;
  AndaleDatabaseContentObservationScope& operator=(
      const AndaleDatabaseContentObservationScope&) = delete;

 private:
  PPCContext& ctx_;
  uint32_t manager_;
  uint32_t slot_;
  uint32_t content_;
  uint32_t policy_;
  uint32_t flags_;
  uint32_t caller_;
};

void ObserveScoreModuleUpdate(PPCContext& ctx, uint8_t* base);
void PublishCustomScorableIdAfterInputUpdate(uint8_t* base, uint32_t module,
                                             uint32_t external_input);
// Returns the PhysOut proven reachable from the player-0 action-graph actor.
// Zero means ownership has not yet been resolved.
uint32_t CurrentLocalPhysOut();

// Read-only player-owned board position for native sandbox diagnostics.
bool CurrentLocalBoardPosition(float out_position[3]);
void ObserveGestureMappingMatch(PPCContext& ctx, uint8_t* base);
void ObserveAirTrickAnalysis(PPCContext& ctx, uint8_t* base);
void ObserveAirStartTrick(PPCContext& ctx, uint8_t* base);
void ObserveAirEndTrick(PPCContext& ctx, uint8_t* base);
void ObserveAirUpdateTricks(PPCContext& ctx, uint8_t* base);
void ObserveScoreHolderRecordTrick(PPCContext& ctx, uint8_t* base);
void ObserveScoreHolderCancelTrick(PPCContext& ctx, uint8_t* base);
void ObserveXenonFileDeviceRead(PPCContext& ctx);
void ObserveAnimationEvalCommandBuffer(PPCContext& ctx, uint8_t* base);
void ObserveAnimationStreamTableEval(PPCContext& ctx, uint8_t* base);

class CustomAnimationStreamEvalScope {
 public:
  CustomAnimationStreamEvalScope(PPCContext& ctx, uint8_t* base);
  ~CustomAnimationStreamEvalScope();

  CustomAnimationStreamEvalScope(const CustomAnimationStreamEvalScope&) =
      delete;
  CustomAnimationStreamEvalScope& operator=(
      const CustomAnimationStreamEvalScope&) = delete;

 private:
  std::string previous_rule_;
  std::string previous_gesture_stream_;
  uint32_t previous_gesture_clip_{};
};

class VbrExtractObservationScope {
 public:
  VbrExtractObservationScope(PPCContext& ctx, uint8_t* base);
  ~VbrExtractObservationScope();

  VbrExtractObservationScope(const VbrExtractObservationScope&) = delete;
  VbrExtractObservationScope& operator=(const VbrExtractObservationScope&) =
      delete;

 private:
  uint8_t* base_{};
  uint32_t extracts_{};
  uint32_t count_{};
  uint32_t decoder_{};
  uint32_t observation_index_{};
  uint64_t frame_{};
  std::string rule_;
  std::string gesture_stream_;
  uint32_t gesture_clip_{};
};

void ObserveActiveCustomAnimationAssetLoadStage(const char* stage);
void PreloadConfiguredSkaterAnimationAssets(PPCContext& ctx, uint8_t* base,
                                            uint32_t playback_data);
void ApplySelectedAnimationOverride(PPCContext& ctx, uint8_t* base,
                                    uint32_t selected_descriptor_address);
// Samples the active skateboard-body world matrix selected by
// SkateboardController::FillPhysOut. The PhysOut argument is matched against
// the harness-observed local player before any spatial data is retained.
void ObserveLocalSkateboardSpatialState(PPCContext& ctx, uint8_t* base);
// Delays the guarded owned-world board correction until the retail
// SkateboardController::FillPhysOut body has completed.
class OwnedWorldCollisionBridgeScope {
 public:
  OwnedWorldCollisionBridgeScope(PPCContext& ctx, uint8_t* base);
  ~OwnedWorldCollisionBridgeScope();

  OwnedWorldCollisionBridgeScope(
      const OwnedWorldCollisionBridgeScope&) = delete;
  OwnedWorldCollisionBridgeScope& operator=(
      const OwnedWorldCollisionBridgeScope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t controller_;
  uint32_t phys_out_;
};
// Returns the latest player-owned board transform without retaining guest
// pointers in the caller. This is the compact presentation boundary used by
// the external Godot runtime capsule.
bool CurrentLiveSpatialSnapshot(LiveSpatialSnapshot& out);
// Samples the local player board-state provider resolved through the
// verified IsOffboard condition. Retail IsOffboard is byte 673 || byte 675;
// IsAirOffboard is byte 674.
void ObserveLocalBoardState(uint64_t frame, uint8_t* base, uint32_t entity,
                            bool on_ground);

void ResetAndArm();
void SetFocus(bool focused);
void AppendObservationFields(std::ostream& response);

}  // namespace skate3::trick_pipeline
