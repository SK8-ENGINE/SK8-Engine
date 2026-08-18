#pragma once

#include <cstdint>
#include <iosfwd>
#include <string_view>

struct PPCContext;

namespace rex::input {
class InputSystem;
}

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::custom_trick {

// Retail EScorableID is a closed 0..331 domain with many hard-coded bounds.
// Custom IDs live in a disjoint high-bit namespace and are never passed to a
// retail table consumer without an explicit adapter.
using CustomTrickId = uint32_t;
// EScorableID is carried as a signed 32-bit value throughout Skate 3 and
// reserves -1 as "none". Keep custom IDs positive while remaining far outside
// the fixed retail range [0, 331].
constexpr CustomTrickId kFirstCustomTrickId = 0x00010000u;

struct CustomTrickDefinition {
  CustomTrickId id;
  std::string_view name;
  std::string_view display_name;
  std::string_view input_token;
  uint32_t base_points;
  std::string_view skater_animation;
  std::string_view skater_animation_air;
  std::string_view board_animation;
  // Optional retail animation descriptors that the custom asset replaces
  // while preserving the native trick behavior and locomotion layers.
  std::string_view carrier_animation;
  std::string_view carrier_animation_air;
};

const CustomTrickDefinition *FindDefinitionByToken(std::string_view token);
// Selects the active experimental definition. The default remains the
// authored Corkscrew regression; feature flags select another registered
// native-VBR trick without mutating any registry entry.
const CustomTrickDefinition &ActiveDefinition();
std::string_view ActiveInputToken();
constexpr uint32_t kFlipScorableMetadataTemplateId = 0x60u;

// First-class custom Scorable boundary. The retail definition registry is a
// closed 0..331 table, so custom names resolve through a structurally
// compatible flip definition and are finalized with their disjoint identity
// before returning to the caller.
bool IsActiveScorableName(std::string_view name);
bool IsActiveScorableId(uint32_t id);
uint32_t ActiveScorableId();
bool ShouldPublishActiveScorable(uint64_t frame, uint32_t phys_out);
uint32_t EnsureScorableTokenAddress(uint8_t* base);
void FinalizeResolvedScorable(uint8_t* base, uint32_t scorable);
bool TryReturnScorableName(PPCContext& ctx, uint8_t* base);

// Hooks the game's PlayAnimation behavior so a queued custom animation can
// reuse the active skater's real animation controller.
void InstallHooks(rex::runtime::FunctionDispatcher *dispatcher);

// Adds a G-key watcher to the merged input stream. It never synthesizes
// gamepad input; G only queues the direct animation request handled by
// InstallHooks.
void InstallInputDriver(rex::input::InputSystem *input_system);

// Queues the native CODEX_CORKSCREW consumer after the action-graph input
// listener has published the new token. The consumer is independently
// default-off, so token production remains testable on its own.
void RequestFromActionGraphToken(uint64_t frame, uint8_t *base,
                                 uint32_t listener, uint32_t actor,
                                 uint32_t intents,
                                 bool replace_native_trick_layer = false);
void ObservePlayerAnimationDispatchContext(PPCContext &ctx, uint8_t *base,
                                           uint32_t actor);
bool TryDispatchPendingHotkey(PPCContext &ctx, uint8_t *base, uint32_t listener,
                              uint32_t actor, uint32_t intents);
bool TryDispatchQueuedGroundLayer(PPCContext &ctx, uint8_t *base,
                                  uint32_t actor);
bool TryDispatchQueuedAirLayer(PPCContext &ctx, uint8_t *base,
                               uint32_t actor);

// Dispatches the selected custom animation only when the queued request
// reaches a context owned by the requesting Actor's SkaterAnim. Definitions
// with a separate air phase layer their ground/air clips over the matching
// retail Ollie phases instead of replacing the locomotion base.
bool TryApplyQueuedAnimation(PPCContext &ctx, uint8_t *base,
                             uint32_t selected_descriptor);

// Links the direct request to the verified custom asset's stream evaluator.
// Evaluation is evidence that the custom clip reached native playback, not
// proof that the complete trick lifecycle is reconstructed.
void ObserveCustomAnimationEvaluated(std::string_view rule_name, uint64_t frame,
                                     uint32_t object, uint32_t clip);

// Binds the naturally resolved custom Scorable to the actor request. The
// Scorable itself carries the custom ID, name, and base points.
bool TryBindResolvedScorable(uint64_t frame, uint8_t* base,
                             uint32_t scorable,
                             std::string_view requested_name,
                             uint32_t phys_out);
void ObserveScoreHolderRecord(uint64_t frame, uint8_t *base, uint32_t holder,
                              uint32_t scorable);
void ObserveScoreHolderCancel(uint64_t frame, uint8_t *base, uint32_t holder,
                              uint32_t scorable);
// Joins the downstream air-sequence publish/reset result to the active custom
// lifecycle. A positive publish is the numeric combo-bank signal; a zero
// publish after carrier recording rejects the landing without rewriting
// retail cumulative score.
void ObserveScoreHolderAirSequenceBank(uint64_t frame, uint32_t holder,
                                       uint32_t published_reward_bits,
                                       uint32_t cumulative_reward_bits);
void ObserveScoreModuleOwnership(uint64_t frame, uint32_t phys_out,
                                 uint32_t holder);

void ObservePointPenalty(uint64_t frame, uint32_t holder, int32_t retail_id,
                         uint32_t result_bits, bool adapted);
bool BeginPointPenaltyAdapter(uint8_t *base, uint32_t holder, int32_t retail_id,
                              uint8_t &retail_count);
void EndPointPenaltyAdapter(uint8_t *base, uint32_t holder, int32_t retail_id,
                            uint8_t retail_count);
bool BeginEndAirAdapter(uint8_t *base, uint32_t holder, int32_t retail_id,
                        uint8_t &retail_id_count,
                        uint8_t &retail_secondary_count, uint32_t &group_index,
                        uint8_t &retail_group_count);
void EndEndAirAdapter(uint64_t frame, uint8_t *base, uint32_t holder,
                      int32_t retail_id, uint8_t retail_id_count,
                      uint8_t retail_secondary_count, uint32_t group_index,
                      uint8_t retail_group_count);
void ObserveEndAirRepetitionResult(uint8_t *base, uint32_t holder,
                                   int32_t retail_id);
std::string_view FindActiveDisplayName(uint8_t *base,
                                       uint32_t definition_index);
void ObserveDisplayNameApplied(uint64_t frame, uint32_t manager,
                               std::string_view retail_text,
                               std::string_view custom_text);

// Cancels an active logical custom trick when its target actor enters the
// offboard/unknown motion state immediately after retail's manual-bail chord.
// This covers the pre-carrier interval where ScoreHolder has no Scorable to
// announce as cancelled.
void ObserveActorMotionState(uint64_t frame, uint8_t *base, uint32_t entity,
                             bool on_ground, bool in_air);
// Arms collision cancellation only when the exact PhysicsWantsWipeOut
// evaluator returns true for the actor that owns the active custom request.
// A subsequent actor motion transition remains required before state changes.
void ObservePhysicsWantsWipeout(uint64_t frame, uint32_t actor);
// Records the exact actor-owned IsLanding state-graph decision. The on-board
// mode is the physics-quality acceptance signal; grounded state remains the
// final motion confirmation.
void ObserveLandingPolicy(uint64_t frame, uint32_t actor, bool on_board_mode,
                          bool accepted);

void ResetAndArm();
void AppendObservationFields(std::ostream &response);

} // namespace skate3::custom_trick
