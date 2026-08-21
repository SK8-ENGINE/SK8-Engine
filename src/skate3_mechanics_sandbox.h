#pragma once

#include <cstdint>
#include <iosfwd>

struct PPCContext;

namespace skate3::mechanics_sandbox {

enum class PresentationDecision : uint8_t {
  Keep = 0,
  DropNonLocal = 1,
  DropUnresolved = 2,
};

// Presentation-only diagnostic modes. CandidateOnly is the normal sandbox
// policy; the other modes are reversible probes for isolating a black frame.
enum class DiagnosticMode : uint8_t {
  CandidateOnly = 0,
  BackgroundOnly,
  AllDynamic,
  CandidateWorldOn,
};

enum class RenderStage : uint8_t {
  None = 0,
  Entered,
  YieldedForMenus,
  YieldedForPhoto,
  YieldedForMovie,
  SceneReady,
  PipelineReady,
  BackgroundCleared,
  MainPassComplete,
  HdrPostComplete,
  Presented,
};

// Called by the verified player-0 ActionGraph input lane. This is the only
// activation path: normal boot must reach gameplay before the presentation
// shell changes anything.
void ObserveLocalActionGraphActor(uint64_t frame, uint32_t actor);

// Called with the PhysOut proven by the player-0 action-graph ownership
// chain. This is the presentation identity used by the native scene filter;
// it is deliberately not inferred from the most-recent ground predicate.
void ObserveLocalPresentationEntity(uint64_t frame, uint32_t entity);

// Called by actor-scoped ground-state telemetry. It only completes a pending
// reset after the exact local actor returns to a grounded, non-air state.
void ObserveLocalMotionState(uint64_t frame, uint32_t actor, bool on_ground,
                             uint32_t action_graph_actor,
                             uint32_t phys_out, bool player_owned,
                             bool in_air);

bool Requested();
bool Active();
const char* StateName();
uint32_t LocalActor();
uint32_t LocalPresentationEntity();
// Provisional render-side PresentationEntity selected from the first plain
// local skater after player-0 activation. Unlike LocalPresentationEntity
// (the verified PhysOut), this value is directly comparable with
// native_entity::CtxInfo::entity.
uint32_t LocalPresentationCandidate();

bool VisualMapEnabled();
bool NativeCollisionObserverEnabled();
bool OwnedWorldCollisionEnabled();
bool ShouldPublishOwnedWorldGround(uint64_t frame, uint32_t phys_out);
bool ObserveSandboxCamera(const float camera[3]);
bool SandboxMapOrigin(float out_origin[3]);
// Presentation uses the calibrated board-contact plane so the visible floor
// matches the position bridge rather than the earlier pre-settle origin.
bool SandboxMapRenderOrigin(float out_origin[3]);
void RecordMapContact(bool hit, uint32_t id, const float normal[3],
                      float penetration);

// Runs after the verified SkateboardController::FillPhysOut call. The bridge
// is default-off and corrects only the exact player-owned board vertically
// against owned-world floor/ramp surfaces. Retail still owns forces,
// orientation, and lateral contact.
void ApplyOwnedWorldCollisionAfterPhysOut(PPCContext& ctx, uint8_t* base,
                                          uint32_t controller,
                                          uint32_t phys_out);

// Native-scene presentation policy. The scene renderer remains the only
// consumer; no generated guest update or mechanics path is disabled here.
PresentationDecision ClassifyPresentationEntity(uint32_t entity,
                                                 uint8_t entity_class);
DiagnosticMode CurrentDiagnosticMode();
const char* DiagnosticModeName();
bool SetDiagnosticMode(const char* mode);
void BeginPresentationFrame();
void RecordPresentation(PresentationDecision decision);
void RecordPresentationIdentity(uint32_t entity, uint32_t instance,
                                uint8_t entity_class);
void RecordRenderedPresentation(uint32_t entity, uint32_t draw_count);

void RecordRenderStage(RenderStage stage);
void RecordRenderedItems(uint32_t draw_count);
void RecordMapDraw(bool submitted);
void RecordMapChunks(uint32_t total, uint32_t candidates,
                     uint32_t visible, uint32_t resident,
                     uint32_t draw_calls);
void RecordSkyDraw(uint32_t draw_calls);

// Harness reset uses the verified session-marker chord. This only records the
// reset lifecycle; it never writes a guest transform or velocity.
bool RequestReset();

// Appends compact machine-readable fields to STATUS/OBSERVE responses.
void AppendTelemetry(std::ostream& out);

}  // namespace skate3::mechanics_sandbox
