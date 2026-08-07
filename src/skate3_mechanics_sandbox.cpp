#include "skate3_mechanics_sandbox.h"

#include "native/skate3_native_entity.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_trick_pipeline.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>
#include <ostream>
#include <cstring>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox, false, "Skate 3",
    "After stable direct-boot gameplay, retain the generated mechanics kernel "
    "and show only the verified local skater presentation in the native sandbox "
    "shell. The original flat collision remains authoritative.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_visual_map, true, "Skate 3",
    "Draw the native Blender graybox inside the mechanics sandbox. This is "
    "presentation-only and does not replace game collision. Enable explicitly "
    "after the retained-skater presentation is verified.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_collision_observer, false, "Skate 3",
    "Run the native test-map hitbox query as read-only telemetry. It never "
    "feeds or overrides the generated physics kernel.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, skate3_native_render);
REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_selection_outline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_msaa);

namespace skate3::mechanics_sandbox {
namespace {

enum class State : uint8_t {
  Disabled = 0,
  WaitingForGameplay,
  Active,
  ResetPending,
  ResetSucceeded,
  ResetFailed,
};

std::atomic<State> g_state{State::Disabled};
std::atomic<uint32_t> g_local_actor{0};
std::atomic<uint32_t> g_local_presentation_entity{0};
std::atomic<uint32_t> g_presentation_candidate{0};
std::atomic<uint64_t> g_last_gameplay_frame{0};
std::atomic<uint64_t> g_reset_frame{0};
std::atomic<uint32_t> g_reset_requests{0};
std::atomic<uint32_t> g_reset_completions{0};
std::atomic<uint32_t> g_reset_failures{0};
std::atomic<uint32_t> g_visible_items{0};
std::atomic<uint32_t> g_dropped_nonlocal{0};
std::atomic<uint32_t> g_dropped_unresolved{0};
std::atomic<uint8_t> g_diagnostic_mode{
    static_cast<uint8_t>(DiagnosticMode::CandidateOnly)};
std::atomic<uint32_t> g_verified_ground_entity{0};
std::atomic<uint32_t> g_reset_expected_ground_entity{0};
std::atomic<uint32_t> g_reset_expected_action_actor{0};
std::atomic<uint32_t> g_reset_expected_phys_out{0};
std::atomic<uint8_t> g_render_stage{
    static_cast<uint8_t>(RenderStage::None)};
std::atomic<uint64_t> g_render_stage_frames{0};
std::atomic<uint32_t> g_rendered_draws{0};
std::atomic<uint32_t> g_rendered_frames{0};
std::atomic<uint32_t> g_candidate_draws{0};
std::atomic<uint32_t> g_candidate_draw_frames{0};
std::atomic<uint32_t> g_map_draws{0};
std::atomic<uint32_t> g_map_contact_count{0};
std::atomic<uint32_t> g_map_last_contact_id{0};
std::atomic<uint32_t> g_map_last_penetration_bits{0};
std::atomic<uint32_t> g_map_origin_x_bits{0};
std::atomic<uint32_t> g_map_origin_y_bits{0};
std::atomic<uint32_t> g_map_origin_z_bits{0};
std::atomic<bool> g_map_origin_valid{false};
std::mutex g_identity_log_mutex;
std::unordered_set<uint32_t> g_logged_presentation_entities;

struct SavedRenderSettings {
  bool captured = false;
  bool scene = true;
  bool scene_2d = true;
  bool splines = true;
  bool outline = true;
  bool world_items = true;
  bool dynamic_items = true;
  bool hdr = true;
  int32_t msaa = 1;
};

std::mutex g_settings_mutex;
SavedRenderSettings g_saved_settings;

const char* StateNameFor(State state) {
  switch (state) {
    case State::WaitingForGameplay: return "waiting_for_gameplay";
    case State::Active: return "active";
    case State::ResetPending: return "reset_pending";
    case State::ResetSucceeded: return "reset_succeeded";
    case State::ResetFailed: return "reset_failed";
    default: return "disabled";
  }
}

const char* DiagnosticModeNameFor(DiagnosticMode mode) {
  switch (mode) {
    case DiagnosticMode::BackgroundOnly: return "background_only";
    case DiagnosticMode::AllDynamic: return "all_dynamic";
    case DiagnosticMode::CandidateWorldOn: return "candidate_world_on";
    default: return "candidate_only";
  }
}

const char* RenderStageNameFor(RenderStage stage) {
  switch (stage) {
    case RenderStage::Entered: return "entered";
    case RenderStage::YieldedForMenus: return "yielded_menus";
    case RenderStage::YieldedForPhoto: return "yielded_photo";
    case RenderStage::YieldedForMovie: return "yielded_movie";
    case RenderStage::SceneReady: return "scene_ready";
    case RenderStage::PipelineReady: return "pipeline_ready";
    case RenderStage::BackgroundCleared: return "background_cleared";
    case RenderStage::MainPassComplete: return "main_pass_complete";
    case RenderStage::HdrPostComplete: return "hdr_post_complete";
    case RenderStage::Presented: return "presented";
    default: return "none";
  }
}

DiagnosticMode DiagnosticModeValue() {
  return static_cast<DiagnosticMode>(
      g_diagnostic_mode.load(std::memory_order_acquire));
}

void ApplySandboxPresentation() {
  std::lock_guard lock(g_settings_mutex);
  if (!g_saved_settings.captured) {
    g_saved_settings.captured = true;
    g_saved_settings.scene = REXCVAR_GET(skate3_native_render_scene);
    g_saved_settings.scene_2d = REXCVAR_GET(skate3_native_render_scene_2d);
    g_saved_settings.splines = REXCVAR_GET(skate3_native_render_scene_splines);
    g_saved_settings.outline =
        REXCVAR_GET(skate3_native_render_scene_selection_outline);
    g_saved_settings.world_items =
        REXCVAR_GET(skate3_native_render_scene_world_items);
    g_saved_settings.dynamic_items =
        REXCVAR_GET(skate3_native_render_scene_dynamic_items);
    g_saved_settings.hdr = REXCVAR_GET(skate3_native_render_scene_hdr);
    g_saved_settings.msaa = REXCVAR_GET(skate3_native_render_scene_msaa);
  }

  // These are presentation-only gates. Dynamic captures stay on because the
  // local player/board still need their normal native scene path; the scene
  // builder applies the ownership filter below.
  REXCVAR_SET(skate3_native_render_scene, true);
  REXCVAR_SET(skate3_native_render_scene_2d, false);
  REXCVAR_SET(skate3_native_render_scene_splines, false);
  REXCVAR_SET(skate3_native_render_scene_selection_outline, false);
  // Keep world-item capture alive even when the sandbox hides those items at
  // the final draw boundary. The native renderer still needs submitted scene
  // state for a valid camera/frame target; disabling this capture produced
  // the black sandbox frame in the render matrix.
  REXCVAR_SET(skate3_native_render_scene_world_items, true);
  REXCVAR_SET(skate3_native_render_scene_dynamic_items, true);
  // The background-only probe deliberately bypasses the HDR tonemap and
  // MSAA resolve. If the colored clear appears on the classic path, the
  // failure is in the HDR/post chain rather than scene submission.
  if (DiagnosticModeValue() == DiagnosticMode::BackgroundOnly) {
    REXCVAR_SET(skate3_native_render_scene_hdr, false);
    REXCVAR_SET(skate3_native_render_scene_msaa, 1);
  } else {
    REXCVAR_SET(skate3_native_render_scene_hdr, g_saved_settings.hdr);
    REXCVAR_SET(skate3_native_render_scene_msaa, g_saved_settings.msaa);
  }
}

void RestorePresentation() {
  std::lock_guard lock(g_settings_mutex);
  if (!g_saved_settings.captured) {
    return;
  }
  REXCVAR_SET(skate3_native_render_scene, g_saved_settings.scene);
  REXCVAR_SET(skate3_native_render_scene_2d, g_saved_settings.scene_2d);
  REXCVAR_SET(skate3_native_render_scene_splines, g_saved_settings.splines);
  REXCVAR_SET(skate3_native_render_scene_selection_outline,
              g_saved_settings.outline);
  REXCVAR_SET(skate3_native_render_scene_world_items,
              g_saved_settings.world_items);
  REXCVAR_SET(skate3_native_render_scene_dynamic_items,
              g_saved_settings.dynamic_items);
  REXCVAR_SET(skate3_native_render_scene_hdr, g_saved_settings.hdr);
  REXCVAR_SET(skate3_native_render_scene_msaa, g_saved_settings.msaa);
  g_saved_settings = {};
}

void MaybeDeactivate() {
  if (REXCVAR_GET(skate3_mechanics_sandbox)) {
    return;
  }
  const State previous = g_state.exchange(State::Disabled);
  if (previous != State::Disabled) {
    RestorePresentation();
    g_map_origin_valid.store(false, std::memory_order_release);
    g_map_contact_count.store(0, std::memory_order_relaxed);
    REXLOG_INFO("mechanics-sandbox: disabled; retail presentation restored");
  }
}

void TryActivate(uint64_t frame) {
  if (!REXCVAR_GET(skate3_mechanics_sandbox) ||
      g_local_actor.load(std::memory_order_acquire) == 0 ||
      g_local_presentation_entity.load(std::memory_order_acquire) == 0) {
    return;
  }
  const bool gameplay =
      rex::kernel::guest_presence::GameplayContextValue() == 1 &&
      REXCVAR_GET(skate3_native_render);
  State state = g_state.load(std::memory_order_acquire);
  if (gameplay && state != State::Active && state != State::ResetPending) {
    g_last_gameplay_frame.store(frame, std::memory_order_release);
    ApplySandboxPresentation();
    float position[3] = {};
    if (trick_pipeline::CurrentLocalBoardPosition(position)) {
      g_map_origin_x_bits.store(std::bit_cast<uint32_t>(position[0]),
                                std::memory_order_release);
      g_map_origin_y_bits.store(std::bit_cast<uint32_t>(position[1]),
                                std::memory_order_release);
      g_map_origin_z_bits.store(std::bit_cast<uint32_t>(position[2]),
                                std::memory_order_release);
      g_map_origin_valid.store(true, std::memory_order_release);
    }
    g_state.store(State::Active, std::memory_order_release);
    REXLOG_INFO(
        "mechanics-sandbox: active after stable freeroam; local actor=0x{:08X} "
        "physout=0x{:08X} presentation=local-only background=flat-clear "
        "collision=retail-bootstrap",
        g_local_actor.load(std::memory_order_acquire),
        g_local_presentation_entity.load(std::memory_order_acquire));
  }
}

}  // namespace

void ObserveLocalActionGraphActor(uint64_t frame, uint32_t actor) {
  MaybeDeactivate();
  if (!REXCVAR_GET(skate3_mechanics_sandbox)) {
    return;
  }
  if (actor == 0 || frame == 0) {
    if (g_state.load(std::memory_order_acquire) == State::Disabled) {
      g_state.store(State::WaitingForGameplay, std::memory_order_release);
    }
    return;
  }
  g_local_actor.store(actor, std::memory_order_release);
  g_last_gameplay_frame.store(frame, std::memory_order_release);
  TryActivate(frame);
}

void ObserveLocalPresentationEntity(uint64_t frame, uint32_t entity) {
  MaybeDeactivate();
  if (!REXCVAR_GET(skate3_mechanics_sandbox)) {
    return;
  }
  if (entity == 0 || frame == 0) {
    if (g_state.load(std::memory_order_acquire) == State::Disabled) {
      g_state.store(State::WaitingForGameplay, std::memory_order_release);
    }
    return;
  }
  g_local_presentation_entity.store(entity, std::memory_order_release);
  g_last_gameplay_frame.store(frame, std::memory_order_release);
  TryActivate(frame);
}

void ObserveLocalMotionState(uint64_t frame, uint32_t actor, bool on_ground,
                             uint32_t action_graph_actor, uint32_t phys_out,
                             bool player_owned, bool in_air) {
  if (actor == 0 || !Active()) {
    return;
  }
  if (NativeCollisionObserverEnabled() && player_owned) {
    float position[3] = {};
    float origin[3] = {};
    if (trick_pipeline::CurrentLocalBoardPosition(position) &&
        SandboxMapOrigin(origin)) {
      const float local_position[3] = {position[0] - origin[0],
                                       position[1] - origin[1],
                                       position[2] - origin[2]};
      map::Contact contact;
      if (map::QueryContact(local_position, 0.18f, contact)) {
        RecordMapContact(true, contact.id, contact.normal,
                         contact.penetration);
      }
    }
  }
  const State state = g_state.load(std::memory_order_acquire);
  // GroundStatePredicateHook is shared by many actors. A nonzero actor and a
  // grounded bit are therefore insufficient evidence for reset completion.
  // Before a reset, remember only the ground entity observed concurrently
  // with the verified player-0 ActionGraph -> PhysOut ownership lane. While a
  // reset is pending, do not replace that identity with ambient actors.
  if (state != State::ResetPending && player_owned &&
      action_graph_actor == g_local_actor.load(std::memory_order_acquire) &&
      phys_out == g_local_presentation_entity.load(std::memory_order_acquire)) {
    g_verified_ground_entity.store(actor, std::memory_order_release);
  }
  if (state == State::ResetPending && on_ground && !in_air &&
      frame > g_reset_frame.load(std::memory_order_acquire) + 8 &&
      player_owned &&
      actor == g_reset_expected_ground_entity.load(std::memory_order_acquire) &&
      action_graph_actor ==
          g_reset_expected_action_actor.load(std::memory_order_acquire) &&
      phys_out == g_reset_expected_phys_out.load(std::memory_order_acquire)) {
    g_reset_completions.fetch_add(1, std::memory_order_relaxed);
    g_state.store(State::ResetSucceeded, std::memory_order_release);
    REXLOG_INFO(
        "mechanics-sandbox: session-marker reset observed on verified player-0 "
        "ground entity=0x{:08X} action=0x{:08X} physout=0x{:08X}",
        actor, action_graph_actor, phys_out);
  }
}

bool Requested() { return REXCVAR_GET(skate3_mechanics_sandbox); }

bool Active() {
  const State state = g_state.load(std::memory_order_acquire);
  return state == State::Active || state == State::ResetPending ||
         state == State::ResetSucceeded;
}

DiagnosticMode CurrentDiagnosticMode() { return DiagnosticModeValue(); }

const char* DiagnosticModeName() {
  return DiagnosticModeNameFor(DiagnosticModeValue());
}

bool SetDiagnosticMode(const char* mode) {
  if (mode == nullptr) {
    return false;
  }
  DiagnosticMode next;
  if (std::strcmp(mode, "candidate_only") == 0 ||
      std::strcmp(mode, "candidate") == 0) {
    next = DiagnosticMode::CandidateOnly;
  } else if (std::strcmp(mode, "background_only") == 0 ||
             std::strcmp(mode, "background") == 0) {
    next = DiagnosticMode::BackgroundOnly;
  } else if (std::strcmp(mode, "all_dynamic") == 0 ||
             std::strcmp(mode, "all") == 0) {
    next = DiagnosticMode::AllDynamic;
  } else if (std::strcmp(mode, "candidate_world_on") == 0 ||
             std::strcmp(mode, "world_on") == 0) {
    next = DiagnosticMode::CandidateWorldOn;
  } else {
    return false;
  }
  g_diagnostic_mode.store(static_cast<uint8_t>(next), std::memory_order_release);
  if (Active()) {
    ApplySandboxPresentation();
  }
  REXLOG_INFO("mechanics-sandbox: diagnostic mode={}",
              DiagnosticModeNameFor(next));
  return true;
}

const char* StateName() {
  return StateNameFor(g_state.load(std::memory_order_acquire));
}

uint32_t LocalActor() { return g_local_actor.load(std::memory_order_acquire); }

uint32_t LocalPresentationEntity() {
  return g_local_presentation_entity.load(std::memory_order_acquire);
}

bool VisualMapEnabled() {
  return Active() && REXCVAR_GET(skate3_mechanics_sandbox_visual_map);
}

bool NativeCollisionObserverEnabled() {
  return Active() &&
         REXCVAR_GET(skate3_mechanics_sandbox_collision_observer);
}

bool ObserveSandboxCamera(const float camera[3]) {
  if (!Active() || camera == nullptr ||
      g_map_origin_valid.load(std::memory_order_acquire)) {
    return false;
  }
  // Camera fallback is only for a visual frame before the board spatial seam
  // publishes. It is never accepted as ownership proof for collision data.
  g_map_origin_x_bits.store(std::bit_cast<uint32_t>(camera[0]),
                            std::memory_order_release);
  g_map_origin_y_bits.store(std::bit_cast<uint32_t>(camera[1]),
                            std::memory_order_release);
  g_map_origin_z_bits.store(std::bit_cast<uint32_t>(camera[2]),
                            std::memory_order_release);
  g_map_origin_valid.store(true, std::memory_order_release);
  return true;
}

bool SandboxMapOrigin(float out_origin[3]) {
  if (out_origin == nullptr ||
      !g_map_origin_valid.load(std::memory_order_acquire)) {
    return false;
  }
  out_origin[0] = std::bit_cast<float>(
      g_map_origin_x_bits.load(std::memory_order_acquire));
  out_origin[1] = std::bit_cast<float>(
      g_map_origin_y_bits.load(std::memory_order_acquire));
  out_origin[2] = std::bit_cast<float>(
      g_map_origin_z_bits.load(std::memory_order_acquire));
  return std::isfinite(out_origin[0]) && std::isfinite(out_origin[1]) &&
         std::isfinite(out_origin[2]);
}

void RecordMapContact(bool hit, uint32_t id, const float normal[3],
                      float penetration) {
  if (!hit) {
    return;
  }
  g_map_contact_count.fetch_add(1, std::memory_order_relaxed);
  g_map_last_contact_id.store(id, std::memory_order_relaxed);
  g_map_last_penetration_bits.store(std::bit_cast<uint32_t>(penetration),
                                    std::memory_order_relaxed);
  (void)normal;
}

PresentationDecision ClassifyPresentationEntity(uint32_t entity,
                                                 uint8_t entity_class) {
  if (!Active()) {
    return PresentationDecision::Keep;
  }
  const DiagnosticMode mode = DiagnosticModeValue();
  if (mode == DiagnosticMode::BackgroundOnly) {
    return PresentationDecision::DropNonLocal;
  }
  if (mode == DiagnosticMode::AllDynamic) {
    return PresentationDecision::Keep;
  }
  if (entity == 0) {
    return PresentationDecision::DropUnresolved;
  }
  // The verified player-0 PhysOut is not itself a PresentationEntity. Until
  // the direct PhysOut->SkaterPresEntity link is instrumented, keep the first
  // SkaterPresEntity published after player-0 activation. This is a bounded,
  // presentation-only bridge: all later skater/world entities are dropped.
  constexpr uint8_t kSkaterPresEntityClass = 2;
  uint32_t candidate = g_presentation_candidate.load(std::memory_order_acquire);
  if (candidate == 0 && entity_class == kSkaterPresEntityClass) {
    if (g_presentation_candidate.compare_exchange_strong(
            candidate, entity, std::memory_order_acq_rel)) {
      REXLOG_INFO(
          "mechanics-sandbox: provisional local presentation candidate="
          "0x{:08X} source=first-skater-after-player0-activation",
          entity);
      candidate = entity;
    }
  }
  return entity == candidate ? PresentationDecision::Keep
                             : PresentationDecision::DropNonLocal;
}

void BeginPresentationFrame() {
  if (!Active()) {
    return;
  }
  g_visible_items.store(0, std::memory_order_relaxed);
  g_dropped_nonlocal.store(0, std::memory_order_relaxed);
  g_dropped_unresolved.store(0, std::memory_order_relaxed);
  g_candidate_draws.store(0, std::memory_order_relaxed);
}

void RecordPresentation(PresentationDecision decision) {
  switch (decision) {
    case PresentationDecision::Keep:
      g_visible_items.fetch_add(1, std::memory_order_relaxed);
      break;
    case PresentationDecision::DropNonLocal:
      g_dropped_nonlocal.fetch_add(1, std::memory_order_relaxed);
      break;
    case PresentationDecision::DropUnresolved:
      g_dropped_unresolved.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

void RecordPresentationIdentity(uint32_t entity, uint32_t instance,
                                uint8_t entity_class) {
  if (!Active() || entity == 0) {
    return;
  }
  std::lock_guard lock(g_identity_log_mutex);
  if (g_logged_presentation_entities.size() >= 96 ||
      !g_logged_presentation_entities.insert(entity).second) {
    return;
  }
  REXLOG_INFO("mechanics-sandbox: presentation identity entity=0x{:08X} "
              "instance=0x{:08X} class={}{}",
              entity, instance, entity_class,
              entity == LocalPresentationEntity() ? " local-physout" : "");
}

void RecordRenderStage(RenderStage stage) {
  if (!Active()) {
    return;
  }
  g_render_stage.store(static_cast<uint8_t>(stage), std::memory_order_release);
  if (stage == RenderStage::Presented) {
    g_render_stage_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordRenderedItems(uint32_t draw_count) {
  if (!Active()) {
    return;
  }
  g_rendered_draws.store(draw_count, std::memory_order_relaxed);
  g_rendered_frames.fetch_add(1, std::memory_order_relaxed);
}

void RecordMapDraw(bool submitted) {
  if (submitted) {
    g_map_draws.fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordRenderedPresentation(uint32_t entity, uint32_t draw_count) {
  if (!Active() || entity == 0 || draw_count == 0) {
    return;
  }
  if (entity == g_presentation_candidate.load(std::memory_order_acquire)) {
    g_candidate_draws.fetch_add(draw_count, std::memory_order_relaxed);
    g_candidate_draw_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

bool RequestReset() {
  if (!Active()) {
    return false;
  }
  const uint32_t ground = g_verified_ground_entity.load(std::memory_order_acquire);
  const uint32_t action = g_local_actor.load(std::memory_order_acquire);
  const uint32_t phys_out =
      g_local_presentation_entity.load(std::memory_order_acquire);
  if (ground == 0 || action == 0 || phys_out == 0) {
    g_reset_failures.fetch_add(1, std::memory_order_relaxed);
    g_state.store(State::ResetFailed, std::memory_order_release);
    REXLOG_WARN(
        "mechanics-sandbox: reset rejected; no verified player-0 ground "
        "identity (ground=0x{:08X} action=0x{:08X} physout=0x{:08X})",
        ground, action, phys_out);
    return false;
  }
  g_presentation_candidate.store(0, std::memory_order_release);
  g_reset_expected_ground_entity.store(ground, std::memory_order_release);
  g_reset_expected_action_actor.store(action, std::memory_order_release);
  g_reset_expected_phys_out.store(phys_out, std::memory_order_release);
  g_reset_frame.store(g_last_gameplay_frame.load(std::memory_order_acquire),
                      std::memory_order_release);
  g_reset_requests.fetch_add(1, std::memory_order_relaxed);
  g_state.store(State::ResetPending, std::memory_order_release);
  return true;
}

void AppendTelemetry(std::ostream& out) {
  out << " sandbox_requested=" << (Requested() ? 1 : 0)
      << " sandbox_state=" << StateName()
      << " sandbox_active=" << (Active() ? 1 : 0)
      << " sandbox_local_actor=" << LocalActor()
      << " sandbox_local_presentation_entity=" << LocalPresentationEntity()
      << " sandbox_presentation_candidate="
      << g_presentation_candidate.load(std::memory_order_acquire)
      << " sandbox_visible_items="
      << g_visible_items.load(std::memory_order_relaxed)
      << " sandbox_dropped_nonlocal="
      << g_dropped_nonlocal.load(std::memory_order_relaxed)
      << " sandbox_dropped_unresolved="
      << g_dropped_unresolved.load(std::memory_order_relaxed)
      << " sandbox_diag_mode=" << DiagnosticModeName()
      << " sandbox_render_stage="
      << RenderStageNameFor(static_cast<RenderStage>(
             g_render_stage.load(std::memory_order_acquire)))
      << " sandbox_rendered_draws="
      << g_rendered_draws.load(std::memory_order_relaxed)
      << " sandbox_rendered_frames="
      << g_rendered_frames.load(std::memory_order_relaxed)
      << " sandbox_candidate_draws="
      << g_candidate_draws.load(std::memory_order_relaxed)
      << " sandbox_candidate_draw_frames="
      << g_candidate_draw_frames.load(std::memory_order_relaxed)
      << " sandbox_map_draws="
      << g_map_draws.load(std::memory_order_relaxed)
      << " sandbox_presented_frames="
      << g_render_stage_frames.load(std::memory_order_relaxed)
      << " sandbox_verified_ground_entity="
      << g_verified_ground_entity.load(std::memory_order_acquire)
      << " sandbox_reset_expected_ground_entity="
      << g_reset_expected_ground_entity.load(std::memory_order_acquire)
      << " sandbox_reset_expected_action_actor="
      << g_reset_expected_action_actor.load(std::memory_order_acquire)
      << " sandbox_reset_expected_physout="
      << g_reset_expected_phys_out.load(std::memory_order_acquire)
      << " sandbox_background=flat_clear"
      << " sandbox_visual_map=" << (VisualMapEnabled() ? 1 : 0)
      << " sandbox_map_origin_valid="
      << (g_map_origin_valid.load(std::memory_order_acquire) ? 1 : 0)
      << " sandbox_map_contact_count="
      << g_map_contact_count.load(std::memory_order_relaxed)
      << " sandbox_map_last_contact_id="
      << g_map_last_contact_id.load(std::memory_order_relaxed)
      << " sandbox_collision="
      << (NativeCollisionObserverEnabled() ? "native_test_map_observer"
                                            : "retail_flat_bootstrap")
      << " sandbox_camera=retail_chase"
      << " sandbox_reset_requests="
      << g_reset_requests.load(std::memory_order_relaxed)
      << " sandbox_reset_completions="
      << g_reset_completions.load(std::memory_order_relaxed)
      << " sandbox_reset_failures="
      << g_reset_failures.load(std::memory_order_relaxed);
}

}  // namespace skate3::mechanics_sandbox
