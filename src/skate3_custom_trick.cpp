#include "skate3_custom_trick.h"
#include "skate3_input_history_watch.h"
#include "skate3_input_lab.h"
#include "skate3_trick_pipeline.h"
#include "skate3_trick_types.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_custom_trick_g, false, "Skate 3",
                    "Enable the G-key animation proof-of-concept");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_native_consumer, false, "Skate 3",
    "Experimental: consume the CODEX_CORKSCREW action-graph token and "
    "directly dispatch its additive custom animation");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_native_dispatch, false, "Skate 3",
    "Experimental: dispatch a queued custom token only through "
    "a PlayAnimation context whose ISkaterAnim belongs to the requesting "
    "Actor");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_scoring, false, "Skate 3",
    "Experimental: score the selected custom definition through an explicit "
    "retail Scorable compatibility carrier");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_kickflip_copy, false, "Skate 3",
    "Legacy alias: select the Blender-authored Impossible definition instead "
    "of the Corkscrew regression");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_impossible, false, "Skate 3",
    "Select the independent Blender-authored Impossible trick definition");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_720flip, false, "Skate 3",
    "Select the Blender-authored 720 Flip with its disjoint custom intent "
    "and native motion-graph state");

namespace skate3::custom_trick {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;

constexpr uint32_t kRetailScorableIdCount = 332;
constexpr CustomTrickDefinition kDefinitions[] = {
    {
        .id = kFirstCustomTrickId + 1,
        .name = "codex-corkscrew",
        .display_name = "Corkscrew",
        .input_token = "CODEX_CORKSCREW",
        .base_points = 350,
        .skater_animation = "CODEX_CORKSCREW",
        .skater_animation_air = "",
        // Skate 3's OnBoard clip carries the skater and board subtrees in one
        // synchronized 36-channel stream. This additive clip authors both.
        .board_animation = "CODEX_CORKSCREW",
        .carrier_animation = "",
        .carrier_animation_air = "",
    },
    {
        .id = kFirstCustomTrickId + 2,
        .name = "codex-impossible",
        .display_name = "Impossible",
        .input_token = "CODEX_IMPOSSIBLE",
        .base_points = 100,
        .skater_animation = "CODEX_360FLIP_BLENDER_G",
        .skater_animation_air = "CODEX_360FLIP_BLENDER_A",
        .board_animation = "CODEX_360FLIP_BLENDER_G",
        .carrier_animation = "",
        .carrier_animation_air = "",
    },
    {
        .id = kFirstCustomTrickId + 3,
        .name = "codex-720flip",
        .display_name = "720 Flip",
        .input_token = "CODEX_720FLIP",
        .base_points = 720,
        // Blender-authored, native-VBR ground and air phases selected by the
        // disjoint CODEX_720FLIP motion-graph state. No retail trick intent or
        // animation is used as a carrier.
        .skater_animation = "CODEX_720FLIP_HIGH_G",
        .skater_animation_air = "CODEX_720FLIP_HIGH_A",
        .board_animation = "CODEX_720FLIP_HIGH_G",
        .carrier_animation = "",
        .carrier_animation_air = "",
    },
};
static_assert(kDefinitions[0].id >= kFirstCustomTrickId);
static_assert(kDefinitions[0].id >= kRetailScorableIdCount);
static_assert(kDefinitions[1].id >= kFirstCustomTrickId);
static_assert(kDefinitions[1].id >= kRetailScorableIdCount);
static_assert(kDefinitions[2].id >= kFirstCustomTrickId);
static_assert(kDefinitions[2].id >= kRetailScorableIdCount);
constexpr uint32_t kPlayAnimationExecute = 0x82BB5188;
constexpr uint32_t kISkaterAnimIdentity = 0x82453410;
constexpr uint32_t kSkaterAnimAllocationSize = 15328;
constexpr uint32_t kActorSkaterAnimInterfaceOffset = 14960;

enum class ConsumerState : uint32_t {
  Idle,
  Queued,
  Dispatching,
  Dispatched,
  Evaluated,
  Landing,
  Completed,
  Cancelled,
  Recovering,
  Recovered,
  Failed,
};

enum class ScoreState : uint32_t {
  Idle,
  CarrierActive,
  Committed,
  Cancelled,
};

std::atomic<ConsumerState> g_consumer_state{ConsumerState::Idle};
std::atomic<uint32_t> g_animation_phase{0};
std::atomic<uint64_t> g_request_count{0};
std::atomic<uint64_t> g_dispatch_count{0};
std::atomic<uint64_t> g_dispatch_failure_count{0};
std::atomic<uint64_t> g_stale_request_requeue_count{0};
std::atomic<uint64_t> g_evaluation_count{0};
std::atomic<uint64_t> g_candidate_count{0};
std::atomic<uint64_t> g_rejected_candidate_count{0};
std::atomic<uint64_t> g_controller_match_count{0};
std::atomic<uint64_t> g_registry_bind_count{0};
std::atomic<uint64_t> g_carrier_bind_count{0};
std::atomic<uint64_t> g_carrier_restore_count{0};
std::atomic<uint32_t> g_carrier_restored_points{0};
std::atomic<uint64_t> g_score_commit_count{0};
std::atomic<uint64_t> g_score_cancel_count{0};
std::atomic<uint64_t> g_bank_observation_count{0};
std::atomic<uint64_t> g_bank_commit_count{0};
std::atomic<uint64_t> g_bank_cancel_count{0};
std::atomic<uint64_t> g_bank_observation_frame{0};
std::atomic<uint64_t> g_bank_commit_frame{0};
std::atomic<uint32_t> g_bank_published_reward_bits{0};
std::atomic<uint32_t> g_bank_cumulative_reward_bits{0};
std::atomic<uint64_t> g_lifecycle_cancel_count{0};
std::atomic<uint64_t> g_lifecycle_cancel_frame{0};
std::atomic<uint64_t> g_lifecycle_complete_count{0};
std::atomic<uint64_t> g_lifecycle_complete_frame{0};
std::atomic<uint64_t> g_landing_policy_check_count{0};
std::atomic<uint64_t> g_landing_policy_accept_count{0};
std::atomic<uint64_t> g_landing_policy_accept_frame{0};
std::atomic<uint32_t> g_landing_policy_actor{0};
std::atomic<uint64_t> g_landing_grounded_frame{0};
std::atomic<uint32_t> g_landing_grounded_entity{0};
std::atomic<uint64_t> g_recovery_start_count{0};
std::atomic<uint64_t> g_recovery_start_frame{0};
std::atomic<uint64_t> g_recovery_complete_count{0};
std::atomic<uint64_t> g_recovery_complete_frame{0};
std::atomic<uint64_t> g_collision_wipeout_request_count{0};
std::atomic<uint64_t> g_collision_wipeout_request_frame{0};
std::atomic<uint32_t> g_collision_wipeout_request_actor{0};
std::atomic<uint64_t> g_collision_wipeout_motion_match_count{0};
std::atomic<uint64_t> g_collision_wipeout_motion_match_frame{0};
std::atomic<uint64_t> g_collision_wipeout_cancel_count{0};
std::atomic<uint64_t> g_request_frame{0};
std::atomic<uint64_t> g_dispatch_frame{0};
std::atomic<uint64_t> g_evaluation_frame{0};
std::atomic<uint32_t> g_request_listener{0};
std::atomic<uint32_t> g_request_actor{0};
std::atomic<uint32_t> g_request_intents{0};
std::atomic<bool> g_replace_native_trick_layer{false};
std::atomic<bool> g_target_in_air{false};
std::atomic<uint32_t> g_target_actor{0};
std::atomic<uint32_t> g_target_entity{0};
std::atomic<uint32_t> g_target_skater_anim_interface{0};
std::atomic<uint32_t> g_target_animation_controller{0};
std::atomic<uint32_t> g_recent_player_animation_controller{0};
std::atomic<uint32_t> g_recent_player_skater_anim_interface{0};
std::atomic<uint32_t> g_recent_player_blend_time_bits{0};
std::atomic<bool> g_hotkey_pending{false};
std::atomic<uint32_t> g_dispatch_controller{0};
std::atomic<uint32_t> g_evaluation_object{0};
std::atomic<uint32_t> g_evaluation_clip{0};
std::atomic<uint32_t> g_carrier_scorable{0};
std::atomic<uint32_t> g_carrier_phys_out{0};
std::atomic<uint32_t> g_carrier_retail_id{0xFFFFFFFFu};
std::atomic<uint32_t> g_carrier_original_points{0};
std::atomic<uint32_t> g_carrier_original_name_pointer{0};
std::atomic<uint32_t> g_custom_display_name_guest_address{0};
std::atomic<uint32_t> g_custom_scorable_token_guest_address{0};
std::atomic<bool> g_direct_score_start_attempted{false};
std::atomic<uint32_t> g_score_holder{0};
std::atomic<uint64_t> g_penalty_observation_count{0};
std::atomic<uint64_t> g_penalty_adapter_count{0};
std::atomic<uint64_t> g_penalty_full_count{0};
std::atomic<uint64_t> g_penalty_half_count{0};
std::atomic<uint32_t> g_penalty_result_bits{0};
std::atomic<uint32_t> g_custom_repetition_count{0};
std::atomic<uint64_t> g_repetition_isolation_count{0};
std::atomic<uint32_t> g_retail_repetition_count_before{0};
std::atomic<uint32_t> g_retail_repetition_count_after{0};
std::atomic<uint32_t> g_observed_retail_repetition_count{0};
std::atomic<bool> g_custom_end_air_pending{false};
std::atomic<uint64_t> g_display_name_apply_count{0};
std::atomic<uint32_t> g_committed_points{0};
std::atomic<uint64_t> g_score_start_frame{0};
std::atomic<uint64_t> g_score_end_frame{0};
std::atomic<ScoreState> g_score_state{ScoreState::Idle};
std::atomic<const CustomTrickDefinition *> g_selected_definition{nullptr};
constexpr std::array<uint32_t, 17> kActorFieldOffsets = {
    1792, 1796, 1800, 1804, 1808, 1812, 1816, 1820, 1824,
    1828, 1832, 1836, 1840, 1844, 1848, 1852, 1856};
std::array<std::atomic<uint32_t>, kActorFieldOffsets.size()>
    g_actor_field_values{};
constexpr size_t kCandidateContextCount = 4;
constexpr size_t kCandidateContextWordCount = 32;
std::array<std::atomic<uint32_t>, kCandidateContextCount>
    g_candidate_contexts{};
std::array<std::array<std::atomic<uint32_t>, kCandidateContextWordCount>,
           kCandidateContextCount>
    g_candidate_context_words{};
std::mutex g_event_mutex;
std::vector<std::string> g_events;

const char *StateName(ConsumerState state) {
  switch (state) {
  case ConsumerState::Idle:
    return "idle";
  case ConsumerState::Queued:
    return "queued";
  case ConsumerState::Dispatching:
    return "dispatching";
  case ConsumerState::Dispatched:
    return "dispatched";
  case ConsumerState::Evaluated:
    return "evaluated";
  case ConsumerState::Landing:
    return "landing";
  case ConsumerState::Completed:
    return "completed";
  case ConsumerState::Cancelled:
    return "cancelled";
  case ConsumerState::Recovering:
    return "recovering";
  case ConsumerState::Recovered:
    return "recovered";
  case ConsumerState::Failed:
    return "failed";
  }
  return "unknown";
}

const char *ScoreStateName(ScoreState state) {
  switch (state) {
  case ScoreState::Idle:
    return "idle";
  case ScoreState::CarrierActive:
    return "carrier-active";
  case ScoreState::Committed:
    return "committed";
  case ScoreState::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

void PushEvent(std::string event) {
  std::lock_guard lock(g_event_mutex);
  constexpr size_t kMaxEvents = 32;
  if (g_events.size() == kMaxEvents) {
    g_events.erase(g_events.begin());
  }
  g_events.push_back(std::move(event));
}

void TryCompleteAcceptedLanding(uint64_t frame) {
  if (g_score_state.load(std::memory_order_acquire) != ScoreState::Committed) {
    return;
  }
  const uint64_t accepted_frame =
      g_landing_policy_accept_frame.load(std::memory_order_acquire);
  const uint64_t grounded_frame =
      g_landing_grounded_frame.load(std::memory_order_acquire);
  const uint64_t bank_frame =
      g_bank_commit_frame.load(std::memory_order_acquire);
  if (!accepted_frame || !grounded_frame || !bank_frame ||
      (accepted_frame > grounded_frame ? accepted_frame - grounded_frame
                                       : grounded_frame - accepted_frame) > 2) {
    return;
  }
  ConsumerState landing = ConsumerState::Landing;
  if (!g_consumer_state.compare_exchange_strong(
          landing, ConsumerState::Completed, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }
  g_lifecycle_complete_frame.store(frame, std::memory_order_release);
  g_lifecycle_complete_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream event;
  event << "CTG@" << frame << ":0x" << std::hex << std::uppercase
        << g_landing_grounded_entity.load(std::memory_order_acquire)
        << ":landed:policy-frame=" << std::dec << accepted_frame
        << ":score-frame=" << g_score_end_frame.load(std::memory_order_acquire)
        << ":bank-frame=" << bank_frame;
  PushEvent(event.str());
}

bool QueueRequest(uint64_t frame, uint32_t listener, uint32_t actor,
                  uint32_t intents, std::string_view source,
                  uint8_t *base = nullptr,
                  bool replace_native_trick_layer = false) {
  const CustomTrickDefinition *definition = &ActiveDefinition();
  ConsumerState expected = g_consumer_state.load(std::memory_order_acquire);
  constexpr uint64_t kStaleRequestFrames = 180;
  const uint64_t previous_request_frame =
      g_request_frame.load(std::memory_order_acquire);
  const bool stale_unscored_request =
      (expected == ConsumerState::Queued ||
       expected == ConsumerState::Dispatched ||
       expected == ConsumerState::Evaluated) &&
      g_score_state.load(std::memory_order_acquire) == ScoreState::Idle &&
      previous_request_frame && frame > previous_request_frame &&
      frame - previous_request_frame > kStaleRequestFrames;
  if (stale_unscored_request) {
    const ConsumerState stale_state = expected;
    if (g_consumer_state.compare_exchange_strong(
            expected, ConsumerState::Failed, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      g_stale_request_requeue_count.fetch_add(1, std::memory_order_relaxed);
      std::ostringstream stale_event;
      stale_event << "CTU@" << frame
                  << ":stale-unscored:" << StateName(stale_state)
                  << ":request-frame=" << previous_request_frame;
      PushEvent(stale_event.str());
      expected = ConsumerState::Failed;
    }
  }
  while (expected == ConsumerState::Idle ||
         expected == ConsumerState::Completed ||
         expected == ConsumerState::Recovered ||
         expected == ConsumerState::Failed) {
    if (g_consumer_state.compare_exchange_weak(expected, ConsumerState::Queued,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      g_score_state.store(ScoreState::Idle, std::memory_order_release);
      g_carrier_scorable.store(0, std::memory_order_release);
      g_carrier_phys_out.store(0, std::memory_order_release);
      g_carrier_retail_id.store(0xFFFFFFFFu, std::memory_order_release);
      g_carrier_original_points.store(0, std::memory_order_release);
      g_committed_points.store(0, std::memory_order_release);
      g_score_start_frame.store(0, std::memory_order_release);
      g_score_end_frame.store(0, std::memory_order_release);
      g_bank_observation_frame.store(0, std::memory_order_release);
      g_bank_commit_frame.store(0, std::memory_order_release);
      g_bank_published_reward_bits.store(0, std::memory_order_release);
      g_bank_cumulative_reward_bits.store(0, std::memory_order_release);
      g_landing_policy_accept_frame.store(0, std::memory_order_release);
      g_landing_policy_actor.store(0, std::memory_order_release);
      g_landing_grounded_frame.store(0, std::memory_order_release);
      g_landing_grounded_entity.store(0, std::memory_order_release);
      g_collision_wipeout_request_frame.store(0, std::memory_order_release);
      g_collision_wipeout_request_actor.store(0, std::memory_order_release);
      g_collision_wipeout_motion_match_frame.store(0,
                                                   std::memory_order_release);
      g_custom_end_air_pending.store(false, std::memory_order_release);
      g_animation_phase.store(0, std::memory_order_release);
      g_request_frame.store(frame, std::memory_order_release);
      g_request_listener.store(listener, std::memory_order_release);
      g_request_actor.store(actor, std::memory_order_release);
      g_request_intents.store(intents, std::memory_order_release);
      g_replace_native_trick_layer.store(replace_native_trick_layer,
                                         std::memory_order_release);
      g_selected_definition.store(definition, std::memory_order_release);
      g_registry_bind_count.fetch_add(1, std::memory_order_relaxed);
      g_target_actor.store(actor, std::memory_order_release);
      g_target_entity.store(trick_pipeline::CurrentLocalPhysOut(),
                            std::memory_order_release);
      const uint32_t skater_anim_interface =
          base && actor ? REX_LOAD_U32(actor + 1804) : 0;
      g_target_skater_anim_interface.store(skater_anim_interface,
                                           std::memory_order_release);
      g_target_animation_controller.store(0, std::memory_order_release);
      for (size_t index = 0; index < kActorFieldOffsets.size(); ++index) {
        g_actor_field_values[index].store(
            base && actor ? REX_LOAD_U32(actor + kActorFieldOffsets[index]) : 0,
            std::memory_order_release);
      }
      g_request_count.fetch_add(1, std::memory_order_relaxed);
      std::ostringstream event;
      event << "CTQ@" << frame << ':' << source << ':'
            << definition->input_token << ":0x" << std::hex << std::uppercase
            << listener << ":0x" << actor << ":0x" << intents;
      if (base && actor) {
        event << ":AG=0x"
              << g_actor_field_values[1].load(std::memory_order_acquire)
              << ":SA=0x"
              << g_actor_field_values[3].load(std::memory_order_acquire);
      }
      PushEvent(event.str());
      std::ostringstream registry_event;
      registry_event << "CTR@" << frame << ":0x" << std::hex << std::uppercase
                     << definition->id << ':' << definition->name << ':'
                     << std::dec << definition->base_points << ':'
                     << definition->skater_animation << ':'
                     << definition->board_animation;
      PushEvent(registry_event.str());
      return true;
    }
  }
  return false;
}

uint32_t ReadAnimationController(uint8_t *base, uint32_t behavior_context) {
  if (!behavior_context) {
    return 0;
  }

  const uint32_t state_context = REX_LOAD_U32(behavior_context + 4);
  if (!state_context) {
    return 0;
  }
  // PlayAnimation::Begin normalizes state_context+4 as its interface list,
  // then loads the animation-controller interface from state_context+0x1C.
  return REX_LOAD_U32(state_context + 0x1C);
}

uint32_t ResolveSkaterAnimInterface(PPCContext source_ctx, uint8_t *base,
                                    uint32_t state_context) {
  if (!state_context) {
    return 0;
  }
  // This is the exact IList::Find call made by PlayAnimation::Begin. The
  // The identity callback returns the retail "ISkaterAnim" literal.
  source_ctx.r3.u64 = state_context + 4;
  source_ctx.r4.u64 = kISkaterAnimIdentity;
  sub_82965630(source_ctx, base);
  return source_ctx.r3.u32;
}

void CaptureCandidateContext(uint8_t *base, uint32_t state_context) {
  if (!state_context) {
    return;
  }
  for (size_t slot = 0; slot < kCandidateContextCount; ++slot) {
    uint32_t observed =
        g_candidate_contexts[slot].load(std::memory_order_acquire);
    if (observed == state_context) {
      return;
    }
    if (observed != 0 || !g_candidate_contexts[slot].compare_exchange_strong(
                             observed, state_context, std::memory_order_acq_rel,
                             std::memory_order_acquire)) {
      continue;
    }
    for (size_t word = 0; word < kCandidateContextWordCount; ++word) {
      g_candidate_context_words[slot][word].store(
          REX_LOAD_U32(state_context + static_cast<uint32_t>(word * 4)),
          std::memory_order_release);
    }
    return;
  }
}

class CustomTrickInputDriver final : public rex::input::InputDriver {
public:
  CustomTrickInputDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  X_RESULT
  GetCapabilities(uint32_t user_index, uint32_t flags,
                  rex::input::X_INPUT_CAPABILITIES *out_caps) override {
    if (user_index != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index,
                    rex::input::X_INPUT_STATE *out_state) override {
    if (user_index != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!out_state) {
      return X_ERROR_SUCCESS;
    }

    std::memset(out_state, 0, sizeof(*out_state));
    PollHotkey();
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t user_index,
                    rex::input::X_INPUT_VIBRATION *vibration) override {
    return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        rex::input::X_INPUT_KEYSTROKE *out_keystroke) override {
    return user_index == 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

private:
  void PollHotkey() {
#if defined(_WIN32)
    const bool enabled = REXCVAR_GET(skate3_custom_trick_g);
    const bool down = enabled && (GetAsyncKeyState('G') & 0x8000) != 0;
    const bool gameplay =
        rex::kernel::guest_presence::GameplayContextValue() == 1;
    if (down && !g_was_down_ && gameplay) {
      g_hotkey_pending.store(true, std::memory_order_release);
      REXLOG_INFO(
          "Skate 3 custom trick: G pressed; queued game-thread dispatch");
    }
    g_was_down_ = down;
#endif
  }

  bool g_was_down_ = false;
};

} // namespace

const CustomTrickDefinition *FindDefinitionByToken(std::string_view token) {
  for (const auto &definition : kDefinitions) {
    if (definition.input_token == token) {
      return &definition;
    }
  }
  return nullptr;
}

const CustomTrickDefinition &ActiveDefinition() {
  if (REXCVAR_GET(skate3_custom_trick_720flip)) {
    return kDefinitions[2];
  }
  return (REXCVAR_GET(skate3_custom_trick_impossible) ||
          REXCVAR_GET(skate3_custom_trick_kickflip_copy))
             ? kDefinitions[1]
             : kDefinitions[0];
}

std::string_view ActiveInputToken() { return ActiveDefinition().input_token; }

bool IsActiveScorableName(std::string_view name) {
  return REXCVAR_GET(skate3_custom_trick_720flip) &&
         (name == "CODEX_720FLIP" || name == "codex_720flip" ||
          name == "codex-720flip");
}

bool IsActiveScorableId(uint32_t id) {
  return REXCVAR_GET(skate3_custom_trick_720flip) &&
         id == ActiveDefinition().id;
}

uint32_t ActiveScorableId() { return ActiveDefinition().id; }

bool ShouldPublishActiveScorable(uint64_t frame, uint32_t phys_out) {
  if (!REXCVAR_GET(skate3_custom_trick_720flip) ||
      !REXCVAR_GET(skate3_custom_trick_scoring) ||
      phys_out != g_target_entity.load(std::memory_order_acquire)) {
    return false;
  }
  const uint64_t request_frame =
      g_request_frame.load(std::memory_order_acquire);
  const ConsumerState state =
      g_consumer_state.load(std::memory_order_acquire);
  // The action-graph request precedes the normal ground-to-air collector
  // transition by roughly 30-40 simulation frames. Keep the custom descriptor
  // available across that transition, but never beyond this bounded request.
  return request_frame && frame >= request_frame &&
         frame - request_frame <= 64 &&
         (state == ConsumerState::Queued ||
          state == ConsumerState::Dispatching ||
          state == ConsumerState::Dispatched ||
          state == ConsumerState::Evaluated);
}

namespace {

uint32_t EnsureGuestString(uint8_t* base, std::atomic<uint32_t>& storage,
                           std::string_view value) {
  uint32_t address = storage.load(std::memory_order_acquire);
  if (address) {
    return address;
  }
  const uint32_t allocated = REX_KERNEL_MEMORY()->SystemHeapAlloc(128, 16);
  if (!allocated) {
    return 0;
  }
  uint32_t expected = 0;
  if (!storage.compare_exchange_strong(expected, allocated,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    REX_KERNEL_MEMORY()->SystemHeapFree(allocated);
    return expected;
  }
  for (size_t index = 0; index < value.size() && index < 127; ++index) {
    REX_STORE_U8(allocated + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(value[index]));
  }
  REX_STORE_U8(allocated + static_cast<uint32_t>(
                             std::min<size_t>(value.size(), 127)),
               0);
  return allocated;
}

} // namespace

uint32_t EnsureScorableTokenAddress(uint8_t* base) {
  if (!base) {
    return 0;
  }
  return EnsureGuestString(base, g_custom_scorable_token_guest_address,
                           "CODEX_720FLIP");
}

void FinalizeResolvedScorable(uint8_t* base, uint32_t scorable) {
  if (!base || !scorable || !REXCVAR_GET(skate3_custom_trick_720flip)) {
    return;
  }
  const auto& definition = ActiveDefinition();
  const uint32_t display_address =
      EnsureGuestString(base, g_custom_display_name_guest_address,
                        "#720 Flip ");
  if (display_address) {
    REX_STORE_U32(scorable + trick::ScorableLayout::kDefinitionWord0,
                  display_address);
  }
  REX_STORE_U32(scorable + trick::ScorableLayout::kBasePointValue,
                definition.base_points);
  REX_STORE_U32(scorable + trick::ScorableLayout::kId, definition.id);
  REX_STORE_U32(scorable + trick::ScorableLayout::kDefinitionWord8,
                definition.id);
  // RefreshTrickDisplay copies this definition selector into its manager and
  // resolves a fresh Scorable from it.  Keeping the inherited Kickflip
  // selector here makes an otherwise-custom Scorable publish "#Kickflip_".
  // Give the display path the custom selector; sub_82DA22A8's custom-ID scope
  // maps only the fixed retail vector lookup back to the Flip template and
  // then finalizes the temporary Scorable with this custom identity/name.
  REX_STORE_U32(scorable + trick::ScorableLayout::kDefinitionWord112,
                definition.id);
}

bool TryReturnScorableName(PPCContext& ctx, uint8_t* base) {
  if (!IsActiveScorableId(ctx.r3.u32)) {
    return false;
  }
  const uint32_t address = EnsureScorableTokenAddress(base);
  if (!address) {
    return false;
  }
  ctx.r3.u32 = address;
  return true;
}

extern "C" REX_FUNC(Skate3CustomTrick_PlayAnimationHook) {
  const uint32_t behavior_context = ctx.r4.u32;
  const uint32_t state_context =
      behavior_context ? REX_LOAD_U32(behavior_context + 4) : 0;
  const uint32_t animation_controller =
      ReadAnimationController(base, behavior_context);
  const uint32_t source_stack = ctx.r1.u32;

  // Preserve normal state-graph behavior first, then make the custom request
  // so it is the final animation command issued by this behavior tick.
  sub_82BB5188(ctx, base);

  // PlayAnimation assembles the selected 24-byte intent at stack frame
  // +0x80. Its frame is 0x190 bytes, and the bytes remain valid after return.
  // Observing this final value avoids false positives from inactive
  // stance/air/ground alternatives stored in the behavior object.
  skate3::input_lab::ObservePlayAnimation(ctx, base, source_stack - 0x110,
                                          behavior_context, state_context,
                                          animation_controller);

  ConsumerState expected = g_consumer_state.load(std::memory_order_acquire);
  const uint32_t candidate_actor =
      input_lab::ResolveAnimationContextActor(base, state_context);
  const uint32_t target_actor = g_target_actor.load(std::memory_order_acquire);
  const uint32_t target_entity =
      g_target_entity.load(std::memory_order_acquire);
  const uint32_t target_interface =
      g_target_skater_anim_interface.load(std::memory_order_acquire);
  const uint32_t candidate_interface =
      ResolveSkaterAnimInterface(ctx, base, state_context);
  const uint32_t skater_anim_base =
      target_interface >= kActorSkaterAnimInterfaceOffset
          ? target_interface - kActorSkaterAnimInterfaceOffset
          : 0;
  const bool ownership_match =
      skater_anim_base != 0 && candidate_interface >= skater_anim_base &&
      candidate_interface < skater_anim_base + kSkaterAnimAllocationSize;
  if (animation_controller && expected == ConsumerState::Queued) {
    CaptureCandidateContext(base, state_context);
    g_candidate_count.fetch_add(1, std::memory_order_relaxed);
    if (!ownership_match) {
      g_rejected_candidate_count.fetch_add(1, std::memory_order_relaxed);
      std::ostringstream event;
      event << "CTS@" << input_history_watch::CurrentFrameSequence() << ":0x"
            << std::hex << std::uppercase << animation_controller << ":0x"
            << state_context << ":0x" << candidate_actor << ":actor=0x"
            << target_actor << ":entity=0x" << target_entity
            << ":ISkaterAnim=0x" << candidate_interface;
      PushEvent(event.str());
      return;
    }
    g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream event;
    event << "CTM@" << input_history_watch::CurrentFrameSequence()
          << ":context=0x" << std::hex << std::uppercase << animation_controller
          << ":SkaterAnim=[0x" << skater_anim_base << ",0x"
          << (skater_anim_base + kSkaterAnimAllocationSize)
          << "):ISkaterAnim=0x" << candidate_interface;
    PushEvent(event.str());
    if (!REXCVAR_GET(skate3_custom_trick_native_dispatch)) {
      return;
    }
  }
}

void InstallHooks(rex::runtime::FunctionDispatcher *dispatcher) {
  if (!dispatcher) {
    REXLOG_WARN("Skate 3 custom trick: function dispatcher unavailable");
    return;
  }
  dispatcher->SetFunction(kPlayAnimationExecute,
                          &Skate3CustomTrick_PlayAnimationHook);
  REXLOG_INFO(
      "Skate 3 custom trick: PlayAnimation ownership observer installed at "
      "0x{:08X}",
      kPlayAnimationExecute);
}

void InstallInputDriver(rex::input::InputSystem *input_system) {
  if (!input_system) {
    return;
  }

  auto driver = std::make_unique<CustomTrickInputDriver>();
  if (driver->Setup() != X_STATUS_SUCCESS) {
    REXLOG_WARN("Skate 3 custom trick: input driver setup failed");
    return;
  }
  input_system->AddDriver(std::move(driver));
  REXLOG_INFO(
      "Skate 3 custom trick: G direct-animation input installed (no stick "
      "injection)");
}

void RequestFromActionGraphToken(uint64_t frame, uint8_t *base,
                                 uint32_t listener, uint32_t actor,
                                 uint32_t intents,
                                 bool replace_native_trick_layer) {
  if (!REXCVAR_GET(skate3_custom_trick_native_consumer)) {
    return;
  }
  QueueRequest(frame, listener, actor, intents,
               replace_native_trick_layer ? "ACTION_GRAPH_OUTER_CIRCLE"
                                          : "ACTION_GRAPH",
               base, replace_native_trick_layer);
}

void ObservePlayerAnimationDispatchContext(PPCContext &ctx, uint8_t *base,
                                           uint32_t actor) {
  if (!base || !actor || !ctx.r27.u32 || !ctx.r29.u32) {
    return;
  }
  const uint32_t expected_interface = REX_LOAD_U32(actor + 1804);
  if (expected_interface < kActorSkaterAnimInterfaceOffset) {
    return;
  }
  const uint32_t skater_anim_base =
      expected_interface - kActorSkaterAnimInterfaceOffset;
  if (ctx.r29.u32 < skater_anim_base ||
      ctx.r29.u32 >= skater_anim_base + kSkaterAnimAllocationSize) {
    return;
  }
  g_recent_player_animation_controller.store(ctx.r27.u32,
                                             std::memory_order_release);
  g_recent_player_skater_anim_interface.store(ctx.r29.u32,
                                              std::memory_order_release);
  g_recent_player_blend_time_bits.store(
      ctx.r31.u32 ? REX_LOAD_U32(ctx.r31.u32 + 144) : 0,
      std::memory_order_release);
}

bool TryDispatchPendingHotkey(PPCContext &ctx, uint8_t *base, uint32_t listener,
                              uint32_t actor, uint32_t intents) {
  if (!base || !actor || !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      !REXCVAR_GET(skate3_custom_trick_native_dispatch) ||
      !g_hotkey_pending.load(std::memory_order_acquire)) {
    return false;
  }
  const uint32_t controller =
      g_recent_player_animation_controller.load(std::memory_order_acquire);
  const uint32_t cached_interface =
      g_recent_player_skater_anim_interface.load(std::memory_order_acquire);
  if (!controller || !cached_interface ||
      cached_interface != REX_LOAD_U32(actor + 1804)) {
    return false;
  }
  if (!g_hotkey_pending.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  if (!QueueRequest(frame, listener, actor, intents, "G", base)) {
    return false;
  }
  ConsumerState expected = ConsumerState::Queued;
  if (!g_consumer_state.compare_exchange_strong(
          expected, ConsumerState::Dispatching, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  if (!definition) {
    g_consumer_state.store(ConsumerState::Failed, std::memory_order_release);
    return false;
  }

  const PPCContext saved = ctx;
  ctx.r1.u32 = (saved.r1.u32 - 0x400) & ~0xFu;
  const uint32_t name_address = ctx.r1.u32 + 0x80;
  const uint32_t descriptor_address = ctx.r1.u32 + 0x100;
  for (size_t index = 0; index < definition->skater_animation.size(); ++index) {
    REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(definition->skater_animation[index]));
  }
  REX_STORE_U8(name_address +
                   static_cast<uint32_t>(definition->skater_animation.size()),
               0);
  ctx.r3.u64 = descriptor_address;
  ctx.r4.u64 = name_address;
  sub_823C3B00(ctx, base);

  const uint32_t vtable = REX_LOAD_U32(controller);
  const uint32_t play_method = vtable ? REX_LOAD_U32(vtable + 16) : 0;
  if (!play_method) {
    ctx = saved;
    g_consumer_state.store(ConsumerState::Failed, std::memory_order_release);
    return false;
  }
  ctx.r3.u64 = controller;
  ctx.r4.u64 = descriptor_address;
  ctx.f1.f64 = static_cast<double>(std::bit_cast<float>(
      g_recent_player_blend_time_bits.load(std::memory_order_acquire)));
  ctx.f2.f64 = 0.0;
  ctx.lr = 0x825999F0;
  REX_CALL_INDIRECT_FUNC(play_method);
  ctx = saved;

  g_target_animation_controller.store(controller, std::memory_order_release);
  g_dispatch_controller.store(controller, std::memory_order_release);
  g_dispatch_frame.store(frame, std::memory_order_release);
  g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  g_consumer_state.store(ConsumerState::Dispatched, std::memory_order_release);
  std::ostringstream event;
  event << "CTJ@" << frame << ':' << definition->skater_animation << ":G:0x"
        << std::hex << std::uppercase << controller << ":ISkaterAnim=0x"
        << cached_interface;
  PushEvent(event.str());
  return true;
}

bool TryDispatchQueuedGroundLayer(PPCContext &ctx, uint8_t *base,
                                  uint32_t actor) {
  if (!base || !actor || !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      !REXCVAR_GET(skate3_custom_trick_native_dispatch) ||
      !g_replace_native_trick_layer.load(std::memory_order_acquire) ||
      g_animation_phase.load(std::memory_order_acquire) != 0 ||
      g_consumer_state.load(std::memory_order_acquire) !=
          ConsumerState::Queued) {
    return false;
  }
  const uint32_t controller =
      g_recent_player_animation_controller.load(std::memory_order_acquire);
  const uint32_t cached_interface =
      g_recent_player_skater_anim_interface.load(std::memory_order_acquire);
  if (!controller || !cached_interface ||
      cached_interface != REX_LOAD_U32(actor + 1804) ||
      actor != g_request_actor.load(std::memory_order_acquire)) {
    return false;
  }
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  if (!definition) {
    return false;
  }
  ConsumerState expected = ConsumerState::Queued;
  if (!g_consumer_state.compare_exchange_strong(
          expected, ConsumerState::Dispatching, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }

  const PPCContext saved = ctx;
  ctx.r1.u32 = (saved.r1.u32 - 0x400) & ~0xFu;
  const uint32_t name_address = ctx.r1.u32 + 0x80;
  const uint32_t descriptor_address = ctx.r1.u32 + 0x100;
  for (size_t index = 0; index < definition->skater_animation.size(); ++index) {
    REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(definition->skater_animation[index]));
  }
  REX_STORE_U8(name_address +
                   static_cast<uint32_t>(definition->skater_animation.size()),
               0);
  ctx.r3.u64 = descriptor_address;
  ctx.r4.u64 = name_address;
  sub_823C3B00(ctx, base);

  const uint32_t vtable = REX_LOAD_U32(controller);
  const uint32_t play_method = vtable ? REX_LOAD_U32(vtable + 16) : 0;
  if (!play_method) {
    ctx = saved;
    g_consumer_state.store(ConsumerState::Failed, std::memory_order_release);
    return false;
  }
  ctx.r3.u64 = controller;
  ctx.r4.u64 = descriptor_address;
  ctx.f1.f64 = static_cast<double>(std::bit_cast<float>(
      g_recent_player_blend_time_bits.load(std::memory_order_acquire)));
  ctx.f2.f64 = 0.0;
  ctx.lr = 0x825999F0;
  REX_CALL_INDIRECT_FUNC(play_method);
  ctx = saved;

  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  g_target_animation_controller.store(controller, std::memory_order_release);
  g_dispatch_controller.store(controller, std::memory_order_release);
  g_dispatch_frame.store(frame, std::memory_order_release);
  g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
  g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  g_animation_phase.store(1, std::memory_order_release);
  g_consumer_state.store(ConsumerState::Dispatched,
                         std::memory_order_release);
  std::ostringstream event;
  event << "CTI@" << frame << ':' << definition->skater_animation
        << ":GROUND:DIRECT:0x" << std::hex << std::uppercase << controller
        << ":ISkaterAnim=0x" << cached_interface;
  PushEvent(event.str());
  return true;
}

bool TryDispatchQueuedAirLayer(PPCContext &ctx, uint8_t *base,
                               uint32_t actor) {
  if (!base || !actor || !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      !REXCVAR_GET(skate3_custom_trick_native_dispatch) ||
      !g_replace_native_trick_layer.load(std::memory_order_acquire) ||
      !g_target_in_air.load(std::memory_order_acquire) ||
      g_animation_phase.load(std::memory_order_acquire) != 1 ||
      actor != g_request_actor.load(std::memory_order_acquire)) {
    return false;
  }
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  if (!definition || definition->skater_animation_air.empty()) {
    return false;
  }
  uint32_t expected_phase = 1;
  if (!g_animation_phase.compare_exchange_strong(
          expected_phase, 2, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  const uint32_t controller =
      g_recent_player_animation_controller.load(std::memory_order_acquire);
  const uint32_t cached_interface =
      g_recent_player_skater_anim_interface.load(std::memory_order_acquire);
  if (!controller || !cached_interface ||
      cached_interface != REX_LOAD_U32(actor + 1804)) {
    g_animation_phase.store(1, std::memory_order_release);
    return false;
  }

  const PPCContext saved = ctx;
  ctx.r1.u32 = (saved.r1.u32 - 0x400) & ~0xFu;
  const uint32_t name_address = ctx.r1.u32 + 0x80;
  const uint32_t descriptor_address = ctx.r1.u32 + 0x100;
  for (size_t index = 0; index < definition->skater_animation_air.size();
       ++index) {
    REX_STORE_U8(
        name_address + static_cast<uint32_t>(index),
        static_cast<uint8_t>(definition->skater_animation_air[index]));
  }
  REX_STORE_U8(
      name_address +
          static_cast<uint32_t>(definition->skater_animation_air.size()),
      0);
  ctx.r3.u64 = descriptor_address;
  ctx.r4.u64 = name_address;
  sub_823C3B00(ctx, base);

  const uint32_t vtable = REX_LOAD_U32(controller);
  const uint32_t play_method = vtable ? REX_LOAD_U32(vtable + 16) : 0;
  if (!play_method) {
    ctx = saved;
    g_animation_phase.store(1, std::memory_order_release);
    return false;
  }
  ctx.r3.u64 = controller;
  ctx.r4.u64 = descriptor_address;
  ctx.f1.f64 = static_cast<double>(std::bit_cast<float>(
      g_recent_player_blend_time_bits.load(std::memory_order_acquire)));
  ctx.f2.f64 = 0.0;
  ctx.lr = 0x825999F0;
  REX_CALL_INDIRECT_FUNC(play_method);
  ctx = saved;

  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  g_target_animation_controller.store(controller, std::memory_order_release);
  g_dispatch_controller.store(controller, std::memory_order_release);
  g_dispatch_frame.store(frame, std::memory_order_release);
  g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
  g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream event;
  event << "CTI@" << frame << ':' << definition->skater_animation_air
        << ":AIR:DIRECT:0x" << std::hex << std::uppercase << controller
        << ":ISkaterAnim=0x" << cached_interface;
  PushEvent(event.str());
  return true;
}

bool TryApplyQueuedAnimation(PPCContext &ctx, uint8_t *base,
                             uint32_t selected_descriptor) {
  if (!base || !selected_descriptor ||
      !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      !REXCVAR_GET(skate3_custom_trick_native_dispatch)) {
    return false;
  }

  const uint32_t target_interface =
      g_target_skater_anim_interface.load(std::memory_order_acquire);
  const uint32_t skater_anim_base =
      target_interface >= kActorSkaterAnimInterfaceOffset
          ? target_interface - kActorSkaterAnimInterfaceOffset
          : 0;
  const uint32_t candidate_interface = ctx.r29.u32;
  const uint32_t animation_controller = ctx.r27.u32;
  if (!skater_anim_base || !animation_controller ||
      candidate_interface < skater_anim_base ||
      candidate_interface >= skater_anim_base + kSkaterAnimAllocationSize) {
    return false;
  }

  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  if (!definition) {
    g_consumer_state.store(ConsumerState::Failed, std::memory_order_release);
    return false;
  }

  const auto construct_descriptor =
      [&](std::string_view animation, uint32_t descriptor_offset) {
        PPCContext constructor_ctx = ctx;
        constructor_ctx.r1.u32 = (ctx.r1.u32 - 0x600) & ~0xFu;
        const uint32_t name_address = constructor_ctx.r1.u32 + 0x80;
        const uint32_t descriptor_address =
            constructor_ctx.r1.u32 + descriptor_offset;
        for (size_t index = 0; index < animation.size(); ++index) {
          REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                       static_cast<uint8_t>(animation[index]));
        }
        REX_STORE_U8(name_address + static_cast<uint32_t>(animation.size()), 0);
        constructor_ctx.r3.u64 = descriptor_address;
        constructor_ctx.r4.u64 = name_address;
        sub_823C3B00(constructor_ctx, base);
        return descriptor_address;
      };
  const auto descriptor_matches = [&](std::string_view animation) {
    const uint32_t expected = construct_descriptor(animation, 0x100);
    for (uint32_t offset = 0; offset < 24; offset += 4) {
      if (REX_LOAD_U32(selected_descriptor + offset) !=
          REX_LOAD_U32(expected + offset)) {
        return false;
      }
    }
    return true;
  };
  const auto dispatch_layer = [&](std::string_view animation,
                                  std::string_view phase) {
    const uint32_t descriptor = construct_descriptor(animation, 0x180);
    const uint32_t vtable = REX_LOAD_U32(animation_controller);
    const uint32_t play_method = vtable ? REX_LOAD_U32(vtable + 16) : 0;
    if (!play_method) {
      return false;
    }
    const PPCContext saved = ctx;
    ctx.r3.u64 = animation_controller;
    ctx.r4.u64 = descriptor;
    ctx.lr = 0x825999F0;
    REX_CALL_INDIRECT_FUNC(play_method);
    ctx = saved;

    const uint64_t frame = input_history_watch::CurrentFrameSequence();
    g_target_animation_controller.store(animation_controller,
                                        std::memory_order_release);
    g_dispatch_controller.store(animation_controller,
                                std::memory_order_release);
    g_dispatch_frame.store(frame, std::memory_order_release);
    g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
    g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream event;
    event << "CTI@" << frame << ':' << animation << ':' << phase << ":0x"
          << std::hex << std::uppercase << animation_controller
          << ":ISkaterAnim=0x" << candidate_interface;
    PushEvent(event.str());
    return true;
  };
  const auto replace_selected_layer = [&](std::string_view animation,
                                          std::string_view phase) {
    PPCContext constructor_ctx = ctx;
    constructor_ctx.r1.u32 = (ctx.r1.u32 - 0x400) & ~0xFu;
    const uint32_t name_address = constructor_ctx.r1.u32 + 0x80;
    for (size_t index = 0; index < animation.size(); ++index) {
      REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                   static_cast<uint8_t>(animation[index]));
    }
    REX_STORE_U8(name_address + static_cast<uint32_t>(animation.size()), 0);
    constructor_ctx.r3.u64 = selected_descriptor;
    constructor_ctx.r4.u64 = name_address;
    sub_823C3B00(constructor_ctx, base);

    const uint64_t frame = input_history_watch::CurrentFrameSequence();
    g_target_animation_controller.store(animation_controller,
                                        std::memory_order_release);
    g_dispatch_controller.store(animation_controller,
                                std::memory_order_release);
    g_dispatch_frame.store(frame, std::memory_order_release);
    g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
    g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream event;
    event << "CTI@" << frame << ':' << animation << ':' << phase
          << ":REPLACE:0x" << std::hex << std::uppercase
          << animation_controller << ":ISkaterAnim=0x"
          << candidate_interface;
    PushEvent(event.str());
    return true;
  };

  // Retail Kickflip is split into additive ground and air layers over the
  // corresponding Ollie locomotion clips. A definition with explicit carrier
  // descriptors replaces only those retail trick layers, leaving the native
  // Ollie locomotion, pop, board physics, landing, and recovery path intact.
  if (!definition->skater_animation_air.empty()) {
    uint32_t expected_phase = g_animation_phase.load(std::memory_order_acquire);
    if (trick_pipeline::NativeCustomTrickGraphEnabled() &&
        expected_phase == 0 &&
        descriptor_matches(definition->skater_animation)) {
      ConsumerState expected_state = ConsumerState::Queued;
      if (!g_consumer_state.compare_exchange_strong(
              expected_state, ConsumerState::Dispatching,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
      }
      const uint64_t frame = input_history_watch::CurrentFrameSequence();
      g_target_animation_controller.store(animation_controller,
                                          std::memory_order_release);
      g_dispatch_controller.store(animation_controller,
                                  std::memory_order_release);
      g_dispatch_frame.store(frame, std::memory_order_release);
      g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
      g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
      g_animation_phase.store(1, std::memory_order_release);
      g_consumer_state.store(ConsumerState::Dispatched,
                             std::memory_order_release);
      std::ostringstream event;
      event << "CTI@" << frame << ':' << definition->skater_animation
            << ":GROUND:NATIVE_GRAPH:0x" << std::hex << std::uppercase
            << animation_controller << ":ISkaterAnim=0x"
            << candidate_interface;
      PushEvent(event.str());
      return true;
    }
    if (trick_pipeline::NativeCustomTrickGraphEnabled() &&
        expected_phase == 1 &&
        descriptor_matches(definition->skater_animation_air)) {
      if (!g_animation_phase.compare_exchange_strong(
              expected_phase, 2, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return false;
      }
      const uint64_t frame = input_history_watch::CurrentFrameSequence();
      g_dispatch_frame.store(frame, std::memory_order_release);
      g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
      g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
      std::ostringstream event;
      event << "CTI@" << frame << ':' << definition->skater_animation_air
            << ":AIR:NATIVE_GRAPH:0x" << std::hex << std::uppercase
            << animation_controller << ":ISkaterAnim=0x"
            << candidate_interface;
      PushEvent(event.str());
      return true;
    }
    if (!definition->carrier_animation.empty() && expected_phase == 0 &&
        descriptor_matches(definition->carrier_animation)) {
      ConsumerState expected_state = ConsumerState::Queued;
      if (!g_consumer_state.compare_exchange_strong(
              expected_state, ConsumerState::Dispatching,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
      }
      replace_selected_layer(definition->skater_animation, "GROUND");
      g_animation_phase.store(1, std::memory_order_release);
      g_consumer_state.store(ConsumerState::Dispatched,
                             std::memory_order_release);
      return true;
    }
    if (!definition->carrier_animation_air.empty() && expected_phase == 1 &&
        descriptor_matches(definition->carrier_animation_air)) {
      if (!g_animation_phase.compare_exchange_strong(
              expected_phase, 2, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return false;
      }
      replace_selected_layer(definition->skater_animation_air, "AIR");
      return true;
    }
    const bool replace_native_layer =
        g_replace_native_trick_layer.load(std::memory_order_acquire);
    if (replace_native_layer && expected_phase == 0 &&
        descriptor_matches("B_N_POPSHUVIT_G")) {
      ConsumerState expected_state = ConsumerState::Queued;
      if (!g_consumer_state.compare_exchange_strong(
              expected_state, ConsumerState::Dispatching,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
      }
      replace_selected_layer(definition->skater_animation, "GROUND");
      g_animation_phase.store(1, std::memory_order_release);
      g_consumer_state.store(ConsumerState::Dispatched,
                             std::memory_order_release);
      return true;
    }
    if (replace_native_layer && expected_phase == 1 &&
        descriptor_matches("B_N_POPSHUVIT_A")) {
      if (!g_animation_phase.compare_exchange_strong(
              expected_phase, 2, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return false;
      }
      replace_selected_layer(definition->skater_animation_air, "AIR");
      return true;
    }
    if (expected_phase == 0 && descriptor_matches("B_OLLIE_G")) {
      ConsumerState expected_state = ConsumerState::Queued;
      if (!g_consumer_state.compare_exchange_strong(
              expected_state, ConsumerState::Dispatching,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
      }
      if (!dispatch_layer(definition->skater_animation, "GROUND")) {
        g_consumer_state.store(ConsumerState::Failed,
                               std::memory_order_release);
        return false;
      }
      g_animation_phase.store(1, std::memory_order_release);
      g_consumer_state.store(ConsumerState::Dispatched,
                             std::memory_order_release);
      return true;
    }
    if (expected_phase == 1 && descriptor_matches("B_OLLIE_A")) {
      if (!g_animation_phase.compare_exchange_strong(
              expected_phase, 2, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return false;
      }
      if (!dispatch_layer(definition->skater_animation_air, "AIR")) {
        g_animation_phase.store(1, std::memory_order_release);
        return false;
      }
      return true;
    }
    return false;
  }

  ConsumerState expected = ConsumerState::Queued;
  if (!g_consumer_state.compare_exchange_strong(
          expected, ConsumerState::Dispatching, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }

  PPCContext constructor_ctx = ctx;
  constructor_ctx.r1.u32 = (ctx.r1.u32 - 0x200) & ~0xFu;
  const uint32_t name_address = constructor_ctx.r1.u32 + 0x80;
  const std::string_view animation = definition->skater_animation;
  for (size_t index = 0; index < animation.size(); ++index) {
    REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(animation[index]));
  }
  REX_STORE_U8(name_address + static_cast<uint32_t>(animation.size()), 0);
  constructor_ctx.r3.u64 = selected_descriptor;
  constructor_ctx.r4.u64 = name_address;
  sub_823C3B00(constructor_ctx, base);

  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  g_target_animation_controller.store(animation_controller,
                                      std::memory_order_release);
  g_dispatch_controller.store(animation_controller, std::memory_order_release);
  g_dispatch_frame.store(frame, std::memory_order_release);
  g_controller_match_count.fetch_add(1, std::memory_order_relaxed);
  g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  g_consumer_state.store(ConsumerState::Dispatched, std::memory_order_release);
  std::ostringstream event;
  event << "CTI@" << frame << ':' << animation << ":0x" << std::hex
        << std::uppercase << animation_controller << ":ISkaterAnim=0x"
        << candidate_interface;
  PushEvent(event.str());
  return true;
}

void ObserveCustomAnimationEvaluated(std::string_view rule_name, uint64_t frame,
                                     uint32_t object, uint32_t clip) {
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  if (!definition || rule_name != definition->name) {
    return;
  }
  ConsumerState expected = g_consumer_state.load(std::memory_order_acquire);
  while (expected == ConsumerState::Dispatching ||
         expected == ConsumerState::Dispatched) {
    if (g_consumer_state.compare_exchange_weak(
            expected, ConsumerState::Evaluated, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      g_evaluation_count.fetch_add(1, std::memory_order_relaxed);
      g_evaluation_frame.store(frame, std::memory_order_release);
      g_evaluation_object.store(object, std::memory_order_release);
      g_evaluation_clip.store(clip, std::memory_order_release);
      std::ostringstream event;
      event << "CTE@" << frame << ':' << rule_name << ":0x" << std::hex
            << std::uppercase << object << ":0x" << clip;
      PushEvent(event.str());
      return;
    }
  }
}

bool TryBindResolvedScorable(uint64_t frame, uint8_t* base,
                             uint32_t scorable,
                             std::string_view requested_name,
                             uint32_t phys_out) {
  if (!base || !scorable ||
      !IsActiveScorableName(requested_name) ||
      !IsActiveScorableId(
          REX_LOAD_U32(scorable + trick::ScorableLayout::kId)) ||
      !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      !REXCVAR_GET(skate3_custom_trick_scoring)) {
    return false;
  }
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  const uint64_t request_frame =
      g_request_frame.load(std::memory_order_acquire);
  const uint32_t target_entity =
      g_target_entity.load(std::memory_order_acquire);
  const ConsumerState consumer_state =
      g_consumer_state.load(std::memory_order_acquire);
  if (!definition || !request_frame || !target_entity ||
      phys_out != target_entity || frame < request_frame ||
      frame - request_frame > 64 ||
      (consumer_state != ConsumerState::Queued &&
       consumer_state != ConsumerState::Dispatching &&
       consumer_state != ConsumerState::Dispatched &&
       consumer_state != ConsumerState::Evaluated)) {
    return false;
  }

  ScoreState expected = ScoreState::Idle;
  if (!g_score_state.compare_exchange_strong(
          expected, ScoreState::CarrierActive, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return g_carrier_scorable.load(std::memory_order_acquire) == scorable;
  }

  const uint32_t custom_id =
      REX_LOAD_U32(scorable + trick::ScorableLayout::kId);
  const uint32_t points =
      REX_LOAD_U32(scorable + trick::ScorableLayout::kBasePointValue);
  g_carrier_scorable.store(scorable, std::memory_order_release);
  g_carrier_phys_out.store(phys_out, std::memory_order_release);
  g_carrier_retail_id.store(custom_id, std::memory_order_release);
  g_carrier_original_points.store(points, std::memory_order_release);
  g_carrier_original_name_pointer.store(0, std::memory_order_release);
  g_score_start_frame.store(frame, std::memory_order_release);
  g_carrier_bind_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream event;
  event << "CTB@" << frame << ":0x" << std::hex << std::uppercase << scorable
        << ":custom=0x" << custom_id << ":points=" << std::dec << points
        << ":phys=0x" << std::hex << std::uppercase << phys_out;
  PushEvent(event.str());
  return true;
}

void ObserveScoreHolderRecord(uint64_t frame, uint8_t *base, uint32_t holder,
                              uint32_t scorable) {
  if (!base || !scorable ||
      g_score_state.load(std::memory_order_acquire) !=
          ScoreState::CarrierActive ||
      g_carrier_scorable.load(std::memory_order_acquire) != scorable) {
    return;
  }
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  if (!definition) {
    return;
  }
  const uint32_t points = REX_LOAD_U32(scorable + 4);
  g_committed_points.store(points, std::memory_order_release);
  g_score_end_frame.store(frame, std::memory_order_release);
  g_score_commit_count.fetch_add(1, std::memory_order_relaxed);
  g_custom_end_air_pending.store(true, std::memory_order_release);
  g_score_state.store(ScoreState::Committed, std::memory_order_release);
  ConsumerState consumer_state =
      g_consumer_state.load(std::memory_order_acquire);
  while (consumer_state == ConsumerState::Dispatched ||
         consumer_state == ConsumerState::Evaluated) {
    if (g_consumer_state.compare_exchange_weak(
            consumer_state, ConsumerState::Landing, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      break;
    }
  }
  std::ostringstream event;
  event << "CTC@" << frame << ":0x" << std::hex << std::uppercase << holder
        << ":0x" << scorable << ":retail=0x"
        << g_carrier_retail_id.load(std::memory_order_acquire) << ":logical=0x"
        << definition->id << ":points=" << std::dec << points;
  PushEvent(event.str());
  const uint32_t original_name_pointer =
      g_carrier_original_name_pointer.load(std::memory_order_acquire);
  if (original_name_pointer) {
    REX_STORE_U32(scorable, original_name_pointer);
  }
}

void ObserveScoreHolderCancel(uint64_t frame, uint8_t *base, uint32_t holder,
                              uint32_t scorable) {
  if (!base || !scorable ||
      g_score_state.load(std::memory_order_acquire) !=
          ScoreState::CarrierActive ||
      g_carrier_scorable.load(std::memory_order_acquire) != scorable) {
    return;
  }
  const uint32_t original_points =
      g_carrier_original_points.load(std::memory_order_acquire);
  const uint32_t original_name_pointer =
      g_carrier_original_name_pointer.load(std::memory_order_acquire);
  REX_STORE_U32(scorable + 4, original_points);
  if (original_name_pointer) {
    REX_STORE_U32(scorable, original_name_pointer);
  }
  g_carrier_restored_points.store(original_points, std::memory_order_release);
  g_carrier_restore_count.fetch_add(1, std::memory_order_relaxed);
  g_custom_end_air_pending.store(false, std::memory_order_release);
  g_score_end_frame.store(frame, std::memory_order_release);
  g_score_cancel_count.fetch_add(1, std::memory_order_relaxed);
  g_score_state.store(ScoreState::Cancelled, std::memory_order_release);
  std::ostringstream event;
  event << "CTX@" << frame << ":0x" << std::hex << std::uppercase << holder
        << ":0x" << scorable << ":restored=" << std::dec << original_points;
  PushEvent(event.str());

  ConsumerState state = g_consumer_state.load(std::memory_order_acquire);
  while (state == ConsumerState::Dispatched ||
         state == ConsumerState::Evaluated || state == ConsumerState::Landing) {
    if (!g_consumer_state.compare_exchange_weak(state, ConsumerState::Cancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
      continue;
    }
    g_lifecycle_cancel_frame.store(frame, std::memory_order_release);
    g_lifecycle_cancel_count.fetch_add(1, std::memory_order_relaxed);
    const uint32_t entity = g_target_entity.load(std::memory_order_acquire);
    std::ostringstream cancel_event;
    cancel_event << "CTL@" << frame << ":0x" << std::hex << std::uppercase
                 << entity
                 << ":retail-scoreholder-cancel:from=" << StateName(state)
                 << ":carrier=0x" << scorable;
    PushEvent(cancel_event.str());
    g_consumer_state.store(ConsumerState::Recovering,
                           std::memory_order_release);
    g_recovery_start_frame.store(frame, std::memory_order_release);
    g_recovery_start_count.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream recovery_event;
    recovery_event << "CTW@" << frame << ":0x" << std::hex << std::uppercase
                   << entity << ":recovering:scoreholder-cancel";
    PushEvent(recovery_event.str());
    break;
  }
}

void ObserveScoreHolderAirSequenceBank(uint64_t frame, uint32_t holder,
                                       uint32_t published_reward_bits,
                                       uint32_t cumulative_reward_bits) {
  if (!holder || !REXCVAR_GET(skate3_custom_trick_scoring) ||
      holder != g_score_holder.load(std::memory_order_acquire) ||
      g_score_state.load(std::memory_order_acquire) != ScoreState::Committed ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return;
  }
  const ConsumerState state = g_consumer_state.load(std::memory_order_acquire);
  if (state != ConsumerState::Landing && state != ConsumerState::Completed) {
    return;
  }
  uint64_t unobserved = 0;
  if (!g_bank_observation_frame.compare_exchange_strong(
          unobserved, frame, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }
  g_bank_observation_count.fetch_add(1, std::memory_order_relaxed);
  g_bank_published_reward_bits.store(published_reward_bits,
                                     std::memory_order_release);
  g_bank_cumulative_reward_bits.store(cumulative_reward_bits,
                                      std::memory_order_release);
  const float published_reward = std::bit_cast<float>(published_reward_bits);
  const uint32_t entity = g_target_entity.load(std::memory_order_acquire);
  if (published_reward > 0.0f) {
    g_bank_commit_count.fetch_add(1, std::memory_order_relaxed);
    g_bank_commit_frame.store(frame, std::memory_order_release);
    std::ostringstream event;
    event << "CTK@" << frame << ":0x" << std::hex << std::uppercase << holder
          << ":published=0x" << published_reward_bits << ":cumulative=0x"
          << cumulative_reward_bits;
    PushEvent(event.str());
    TryCompleteAcceptedLanding(frame);
    return;
  }

  ConsumerState landing = ConsumerState::Landing;
  if (!g_consumer_state.compare_exchange_strong(
          landing, ConsumerState::Cancelled, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }
  g_bank_cancel_count.fetch_add(1, std::memory_order_relaxed);
  g_score_cancel_count.fetch_add(1, std::memory_order_relaxed);
  g_score_state.store(ScoreState::Cancelled, std::memory_order_release);
  g_score_end_frame.store(frame, std::memory_order_release);
  g_custom_end_air_pending.store(false, std::memory_order_release);
  g_lifecycle_cancel_frame.store(frame, std::memory_order_release);
  g_lifecycle_cancel_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream cancel_event;
  cancel_event << "CTL@" << frame << ":0x" << std::hex << std::uppercase
               << entity << ":air-sequence-bank-zero:from=landing:holder=0x"
               << holder;
  PushEvent(cancel_event.str());
  g_consumer_state.store(ConsumerState::Recovering, std::memory_order_release);
  g_recovery_start_frame.store(frame, std::memory_order_release);
  g_recovery_start_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream recovery_event;
  recovery_event << "CTW@" << frame << ":0x" << std::hex << std::uppercase
                 << entity << ":recovering:air-sequence-bank-zero";
  PushEvent(recovery_event.str());
}

void ObserveScoreModuleOwnership(uint64_t, uint32_t phys_out, uint32_t holder) {
  const uint32_t target_entity =
      g_target_entity.load(std::memory_order_acquire);
  if (target_entity && phys_out == target_entity && holder) {
    g_score_holder.store(holder, std::memory_order_release);
  }
}

void TryStartDirectScorable(PPCContext &ctx, uint8_t *base,
                            uint32_t phys_out, uint32_t air_collector,
                            uint32_t holder, uint32_t collector_state) {
  if (!base || !phys_out || !air_collector || !holder ||
      collector_state != 1 ||
      !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      !REXCVAR_GET(skate3_custom_trick_scoring) ||
      g_score_state.load(std::memory_order_acquire) != ScoreState::Idle ||
      phys_out != g_target_entity.load(std::memory_order_acquire) ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return;
  }
  const ConsumerState consumer_state =
      g_consumer_state.load(std::memory_order_acquire);
  if (consumer_state != ConsumerState::Dispatched &&
      consumer_state != ConsumerState::Evaluated) {
    return;
  }
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  const uint64_t request_frame =
      g_request_frame.load(std::memory_order_acquire);
  if (!request_frame || frame < request_frame || frame - request_frame > 96) {
    return;
  }
  bool expected = false;
  if (!g_direct_score_start_attempted.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }

  PPCContext name_ctx = ctx;
  name_ctx.r1.u32 = (ctx.r1.u32 - 0x200u) & ~0xFu;
  name_ctx.r3.u32 = 0x80u;
  name_ctx.lr = 0x82DA9EECu;
  sub_82DA4E98(name_ctx, base);

  PPCContext score_ctx = ctx;
  score_ctx.r1.u32 = (ctx.r1.u32 - 0x400u) & ~0xFu;
  score_ctx.r3.u32 = air_collector;
  score_ctx.r4.u32 = name_ctx.r3.u32;
  score_ctx.r5.u32 = 0x80u;
  score_ctx.r7.u32 = air_collector + 584u;
  score_ctx.r8.u32 = REX_LOAD_U32(air_collector + 764u);
  score_ctx.f1.f64 = 1.0;
  score_ctx.f2.f64 = 0.0;
  score_ctx.lr = 0x82DA9EFCu;
  sub_82DAC620(score_ctx, base);

  const bool bound =
      g_score_state.load(std::memory_order_acquire) ==
          ScoreState::CarrierActive &&
      g_carrier_scorable.load(std::memory_order_acquire) ==
          air_collector + 584u;
  if (!bound) {
    g_direct_score_start_attempted.store(false, std::memory_order_release);
  }
  std::ostringstream event;
  event << "CTS@" << frame << ":0x" << std::hex << std::uppercase
        << air_collector << ":holder=0x" << holder << ":bound="
        << std::dec << (bound ? 1 : 0);
  PushEvent(event.str());
}

void ObservePointPenalty(uint64_t frame, uint32_t holder, int32_t retail_id,
                         uint32_t result_bits, bool adapted) {
  if (!REXCVAR_GET(skate3_custom_trick_scoring) ||
      holder != g_score_holder.load(std::memory_order_acquire) ||
      retail_id != 0x80 ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return;
  }
  g_penalty_observation_count.fetch_add(1, std::memory_order_relaxed);
  if (adapted) {
    g_penalty_adapter_count.fetch_add(1, std::memory_order_relaxed);
    if (result_bits == 0x3F800000u) {
      g_penalty_full_count.fetch_add(1, std::memory_order_relaxed);
    } else if (result_bits == 0x3F000000u) {
      g_penalty_half_count.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_penalty_result_bits.store(result_bits, std::memory_order_release);
  std::ostringstream event;
  event << "CTP@" << frame << ":0x" << std::hex << std::uppercase << holder
        << ":retail=0x" << retail_id << ":result=0x" << result_bits
        << ":adapted=" << std::dec << (adapted ? 1 : 0);
  PushEvent(event.str());
}

bool BeginPointPenaltyAdapter(uint8_t *base, uint32_t holder, int32_t retail_id,
                              uint8_t &retail_count) {
  const ScoreState score_state = g_score_state.load(std::memory_order_acquire);
  if (!base || !REXCVAR_GET(skate3_custom_trick_scoring) ||
      holder != g_score_holder.load(std::memory_order_acquire) ||
      retail_id != 0x80 ||
      (score_state != ScoreState::Idle &&
       score_state != ScoreState::CarrierActive) ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return false;
  }
  const uint32_t address = holder + 4096u + static_cast<uint32_t>(retail_id);
  retail_count = REX_LOAD_U8(address);
  REX_STORE_U8(address, static_cast<uint8_t>(g_custom_repetition_count.load(
                            std::memory_order_acquire)));
  return true;
}

void EndPointPenaltyAdapter(uint8_t *base, uint32_t holder, int32_t retail_id,
                            uint8_t retail_count) {
  if (base && holder && retail_id == 0x80) {
    REX_STORE_U8(holder + 4096u + static_cast<uint32_t>(retail_id),
                 retail_count);
  }
}

bool BeginEndAirAdapter(uint8_t *base, uint32_t holder, int32_t retail_id,
                        uint8_t &retail_id_count,
                        uint8_t &retail_secondary_count, uint32_t &group_index,
                        uint8_t &retail_group_count) {
  if (!base || !REXCVAR_GET(skate3_custom_trick_scoring) ||
      holder != g_score_holder.load(std::memory_order_acquire) ||
      retail_id != 0x80 ||
      g_score_state.load(std::memory_order_acquire) != ScoreState::Committed ||
      !g_custom_end_air_pending.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  const uint32_t id = static_cast<uint32_t>(retail_id);
  retail_id_count = REX_LOAD_U8(holder + 4096u + id);
  retail_secondary_count = REX_LOAD_U8(holder + 4428u + id);
  group_index = REX_LOAD_U32(0x820862A8u + id * 24u + 16u);
  retail_group_count = REX_LOAD_U8(holder + 4760u + group_index);
  REX_STORE_U8(holder + 4096u + id,
               static_cast<uint8_t>(
                   g_custom_repetition_count.load(std::memory_order_acquire)));
  return true;
}

void EndEndAirAdapter(uint64_t frame, uint8_t *base, uint32_t holder,
                      int32_t retail_id, uint8_t retail_id_count,
                      uint8_t retail_secondary_count, uint32_t group_index,
                      uint8_t retail_group_count) {
  if (!base || !holder || retail_id != 0x80) {
    return;
  }
  const uint32_t id = static_cast<uint32_t>(retail_id);
  const uint8_t custom_count = REX_LOAD_U8(holder + 4096u + id);
  g_custom_repetition_count.store(custom_count, std::memory_order_release);
  REX_STORE_U8(holder + 4096u + id, retail_id_count);
  REX_STORE_U8(holder + 4428u + id, retail_secondary_count);
  REX_STORE_U8(holder + 4760u + group_index, retail_group_count);
  g_retail_repetition_count_before.store(retail_id_count,
                                         std::memory_order_release);
  g_retail_repetition_count_after.store(REX_LOAD_U8(holder + 4096u + id),
                                        std::memory_order_release);
  g_repetition_isolation_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream event;
  event << "CTD@" << frame << ":0x" << std::hex << std::uppercase << holder
        << ":retail=0x" << id << ":retail_count=" << std::dec
        << static_cast<uint32_t>(retail_id_count)
        << ":custom_count=" << static_cast<uint32_t>(custom_count)
        << ":group=0x" << std::hex << std::uppercase << group_index;
  PushEvent(event.str());
}

void ObserveEndAirRepetitionResult(uint8_t *base, uint32_t holder,
                                   int32_t retail_id) {
  if (base && holder == g_score_holder.load(std::memory_order_acquire) &&
      retail_id == 0x80) {
    g_observed_retail_repetition_count.store(
        REX_LOAD_U8(holder + 4096u + static_cast<uint32_t>(retail_id)),
        std::memory_order_release);
  }
}

std::string_view FindActiveDisplayName(uint8_t *base,
                                       uint32_t definition_index) {
  const uint32_t carrier = g_carrier_scorable.load(std::memory_order_acquire);
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  const ScoreState score_state =
      g_score_state.load(std::memory_order_acquire);
  if (!base || !carrier || !definition ||
      (score_state != ScoreState::CarrierActive &&
       score_state != ScoreState::Committed)) {
    return {};
  }
  const bool exact_carrier_definition =
      REX_LOAD_U32(carrier + 8) == definition_index;
  const bool committed_unbound_custom_display =
      definition_index == 0xFFFFFFFFu &&
      score_state == ScoreState::Committed;
  if (!exact_carrier_definition && !committed_unbound_custom_display) {
    return {};
  }
  return definition->display_name;
}

void ObserveDisplayNameApplied(uint64_t frame, uint32_t manager,
                               std::string_view retail_text,
                               std::string_view custom_text) {
  g_display_name_apply_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream event;
  event << "CTH@" << frame << ":0x" << std::hex << std::uppercase << manager
        << ':' << retail_text << "->" << custom_text;
  PushEvent(event.str());
}

void ObserveActorMotionState(uint64_t frame, uint8_t *base, uint32_t entity,
                             bool on_ground, bool in_air) {
  if (!base || !entity || !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      entity != g_target_entity.load(std::memory_order_acquire) ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return;
  }
  g_target_in_air.store(in_air, std::memory_order_release);

  if (on_ground &&
      g_score_state.load(std::memory_order_acquire) == ScoreState::Committed &&
      g_consumer_state.load(std::memory_order_acquire) ==
          ConsumerState::Landing) {
    g_landing_grounded_frame.store(frame, std::memory_order_release);
    g_landing_grounded_entity.store(entity, std::memory_order_release);
    TryCompleteAcceptedLanding(frame);
    return;
  }

  if (on_ground) {
    ConsumerState recovering = ConsumerState::Recovering;
    if (g_consumer_state.compare_exchange_strong(
            recovering, ConsumerState::Recovered, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      g_recovery_complete_frame.store(frame, std::memory_order_release);
      g_recovery_complete_count.fetch_add(1, std::memory_order_relaxed);
      std::ostringstream event;
      event << "CTY@" << frame << ":0x" << std::hex << std::uppercase << entity
            << ":recovered:cancel-frame=" << std::dec
            << g_lifecycle_cancel_frame.load(std::memory_order_acquire);
      PushEvent(event.str());
    }
    return;
  }

  if (on_ground || in_air) {
    return;
  }

  const bool manual_bail =
      input_history_watch::ManualBailChordRecentlyObserved(16);
  const uint64_t collision_request_frame =
      g_collision_wipeout_request_frame.load(std::memory_order_acquire);
  const bool collision_wipeout = collision_request_frame &&
                                 frame >= collision_request_frame &&
                                 frame - collision_request_frame <= 16;
  if (!manual_bail && !collision_wipeout) {
    return;
  }
  if (collision_wipeout) {
    uint64_t unmatched = 0;
    if (g_collision_wipeout_motion_match_frame.compare_exchange_strong(
            unmatched, frame, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      g_collision_wipeout_motion_match_count.fetch_add(
          1, std::memory_order_relaxed);
      std::ostringstream event;
      event << "CTM@" << frame << ":0x" << std::hex << std::uppercase << entity
            << ":collision-motion=unknown:request-frame=" << std::dec
            << collision_request_frame;
      PushEvent(event.str());
    }
  }

  ConsumerState state = g_consumer_state.load(std::memory_order_acquire);
  while (
      state == ConsumerState::Queued || state == ConsumerState::Dispatching ||
      state == ConsumerState::Dispatched || state == ConsumerState::Evaluated) {
    const ScoreState score_state =
        g_score_state.load(std::memory_order_acquire);
    if (score_state == ScoreState::Committed ||
        score_state == ScoreState::Cancelled) {
      return;
    }
    if (g_consumer_state.compare_exchange_weak(state, ConsumerState::Cancelled,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      const uint32_t carrier =
          g_carrier_scorable.load(std::memory_order_acquire);
      if (score_state == ScoreState::CarrierActive && carrier) {
        REX_STORE_U32(carrier + 4, g_carrier_original_points.load(
                                       std::memory_order_acquire));
      }
      g_custom_end_air_pending.store(false, std::memory_order_release);
      g_score_end_frame.store(frame, std::memory_order_release);
      g_score_state.store(ScoreState::Cancelled, std::memory_order_release);
      g_lifecycle_cancel_frame.store(frame, std::memory_order_release);
      g_lifecycle_cancel_count.fetch_add(1, std::memory_order_relaxed);
      if (collision_wipeout) {
        g_collision_wipeout_cancel_count.fetch_add(1,
                                                   std::memory_order_relaxed);
      }

      std::ostringstream event;
      event << "CTL@" << frame << ":0x" << std::hex << std::uppercase << entity
            << (collision_wipeout ? ":collision-wipeout:from="
                                  : ":manual-bail:from=")
            << StateName(state) << ":carrier=0x" << carrier;
      PushEvent(event.str());
      g_consumer_state.store(ConsumerState::Recovering,
                             std::memory_order_release);
      g_recovery_start_frame.store(frame, std::memory_order_release);
      g_recovery_start_count.fetch_add(1, std::memory_order_relaxed);
      std::ostringstream recovery_event;
      recovery_event << "CTW@" << frame << ":0x" << std::hex << std::uppercase
                     << entity << ":recovering:motion=unknown";
      PushEvent(recovery_event.str());
      return;
    }
  }
}

void ObservePhysicsWantsWipeout(uint64_t frame, uint32_t actor) {
  if (!actor || !REXCVAR_GET(skate3_custom_trick_native_consumer) ||
      actor != g_target_actor.load(std::memory_order_acquire) ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return;
  }
  uint64_t unarmed = 0;
  if (!g_collision_wipeout_request_frame.compare_exchange_strong(
          unarmed, frame, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }
  g_collision_wipeout_request_actor.store(actor, std::memory_order_release);
  g_collision_wipeout_request_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream event;
  event << "CTV@" << frame << ":0x" << std::hex << std::uppercase << actor
        << ":physics-wants-wipeout";
  PushEvent(event.str());
}

void ObserveLandingPolicy(uint64_t frame, uint32_t actor, bool on_board_mode,
                          bool accepted) {
  if (!on_board_mode || !actor ||
      actor != g_target_actor.load(std::memory_order_acquire) ||
      g_consumer_state.load(std::memory_order_acquire) !=
          ConsumerState::Landing ||
      !g_selected_definition.load(std::memory_order_acquire)) {
    return;
  }
  g_landing_policy_check_count.fetch_add(1, std::memory_order_relaxed);
  if (!accepted) {
    return;
  }
  g_landing_policy_accept_count.fetch_add(1, std::memory_order_relaxed);
  g_landing_policy_accept_frame.store(frame, std::memory_order_release);
  g_landing_policy_actor.store(actor, std::memory_order_release);
  std::ostringstream event;
  event << "CTZ@" << frame << ":0x" << std::hex << std::uppercase << actor
        << ":landing-policy=accepted";
  PushEvent(event.str());
  TryCompleteAcceptedLanding(frame);
}

void ResetAndArm() {
  g_consumer_state.store(ConsumerState::Idle, std::memory_order_release);
  g_animation_phase.store(0, std::memory_order_release);
  g_request_count.store(0, std::memory_order_relaxed);
  g_dispatch_count.store(0, std::memory_order_relaxed);
  g_dispatch_failure_count.store(0, std::memory_order_relaxed);
  g_stale_request_requeue_count.store(0, std::memory_order_relaxed);
  g_evaluation_count.store(0, std::memory_order_relaxed);
  g_candidate_count.store(0, std::memory_order_relaxed);
  g_rejected_candidate_count.store(0, std::memory_order_relaxed);
  g_controller_match_count.store(0, std::memory_order_relaxed);
  g_registry_bind_count.store(0, std::memory_order_relaxed);
  g_carrier_bind_count.store(0, std::memory_order_relaxed);
  g_carrier_restore_count.store(0, std::memory_order_relaxed);
  g_carrier_restored_points.store(0, std::memory_order_relaxed);
  g_score_commit_count.store(0, std::memory_order_relaxed);
  g_score_cancel_count.store(0, std::memory_order_relaxed);
  g_bank_observation_count.store(0, std::memory_order_relaxed);
  g_bank_commit_count.store(0, std::memory_order_relaxed);
  g_bank_cancel_count.store(0, std::memory_order_relaxed);
  g_bank_observation_frame.store(0, std::memory_order_relaxed);
  g_bank_commit_frame.store(0, std::memory_order_relaxed);
  g_bank_published_reward_bits.store(0, std::memory_order_relaxed);
  g_bank_cumulative_reward_bits.store(0, std::memory_order_relaxed);
  g_lifecycle_cancel_count.store(0, std::memory_order_relaxed);
  g_lifecycle_cancel_frame.store(0, std::memory_order_relaxed);
  g_lifecycle_complete_count.store(0, std::memory_order_relaxed);
  g_lifecycle_complete_frame.store(0, std::memory_order_relaxed);
  g_landing_policy_check_count.store(0, std::memory_order_relaxed);
  g_landing_policy_accept_count.store(0, std::memory_order_relaxed);
  g_landing_policy_accept_frame.store(0, std::memory_order_relaxed);
  g_landing_policy_actor.store(0, std::memory_order_relaxed);
  g_landing_grounded_frame.store(0, std::memory_order_relaxed);
  g_landing_grounded_entity.store(0, std::memory_order_relaxed);
  g_recovery_start_count.store(0, std::memory_order_relaxed);
  g_recovery_start_frame.store(0, std::memory_order_relaxed);
  g_recovery_complete_count.store(0, std::memory_order_relaxed);
  g_recovery_complete_frame.store(0, std::memory_order_relaxed);
  g_collision_wipeout_request_count.store(0, std::memory_order_relaxed);
  g_collision_wipeout_request_frame.store(0, std::memory_order_relaxed);
  g_collision_wipeout_request_actor.store(0, std::memory_order_relaxed);
  g_collision_wipeout_motion_match_count.store(0, std::memory_order_relaxed);
  g_collision_wipeout_motion_match_frame.store(0, std::memory_order_relaxed);
  g_collision_wipeout_cancel_count.store(0, std::memory_order_relaxed);
  g_request_frame.store(0, std::memory_order_relaxed);
  g_dispatch_frame.store(0, std::memory_order_relaxed);
  g_evaluation_frame.store(0, std::memory_order_relaxed);
  g_request_listener.store(0, std::memory_order_relaxed);
  g_request_actor.store(0, std::memory_order_relaxed);
  g_request_intents.store(0, std::memory_order_relaxed);
  g_replace_native_trick_layer.store(false, std::memory_order_relaxed);
  g_target_in_air.store(false, std::memory_order_relaxed);
  g_target_actor.store(0, std::memory_order_relaxed);
  g_target_entity.store(0, std::memory_order_relaxed);
  g_target_skater_anim_interface.store(0, std::memory_order_relaxed);
  g_target_animation_controller.store(0, std::memory_order_relaxed);
  g_dispatch_controller.store(0, std::memory_order_relaxed);
  g_evaluation_object.store(0, std::memory_order_relaxed);
  g_evaluation_clip.store(0, std::memory_order_relaxed);
  g_carrier_scorable.store(0, std::memory_order_relaxed);
  g_carrier_phys_out.store(0, std::memory_order_relaxed);
  g_carrier_retail_id.store(0xFFFFFFFFu, std::memory_order_relaxed);
  g_carrier_original_points.store(0, std::memory_order_relaxed);
  g_carrier_original_name_pointer.store(0, std::memory_order_relaxed);
  g_direct_score_start_attempted.store(false, std::memory_order_relaxed);
  g_score_holder.store(0, std::memory_order_relaxed);
  g_penalty_observation_count.store(0, std::memory_order_relaxed);
  g_penalty_adapter_count.store(0, std::memory_order_relaxed);
  g_penalty_full_count.store(0, std::memory_order_relaxed);
  g_penalty_half_count.store(0, std::memory_order_relaxed);
  g_penalty_result_bits.store(0, std::memory_order_relaxed);
  g_custom_repetition_count.store(0, std::memory_order_relaxed);
  g_repetition_isolation_count.store(0, std::memory_order_relaxed);
  g_retail_repetition_count_before.store(0, std::memory_order_relaxed);
  g_retail_repetition_count_after.store(0, std::memory_order_relaxed);
  g_observed_retail_repetition_count.store(0, std::memory_order_relaxed);
  g_custom_end_air_pending.store(false, std::memory_order_relaxed);
  g_display_name_apply_count.store(0, std::memory_order_relaxed);
  g_committed_points.store(0, std::memory_order_relaxed);
  g_score_start_frame.store(0, std::memory_order_relaxed);
  g_score_end_frame.store(0, std::memory_order_relaxed);
  g_score_state.store(ScoreState::Idle, std::memory_order_relaxed);
  g_selected_definition.store(nullptr, std::memory_order_relaxed);
  for (auto &value : g_actor_field_values) {
    value.store(0, std::memory_order_relaxed);
  }
  for (size_t slot = 0; slot < kCandidateContextCount; ++slot) {
    g_candidate_contexts[slot].store(0, std::memory_order_relaxed);
    for (auto &word : g_candidate_context_words[slot]) {
      word.store(0, std::memory_order_relaxed);
    }
  }
  std::lock_guard lock(g_event_mutex);
  g_events.clear();
}

void AppendObservationFields(std::ostream &response) {
  const CustomTrickDefinition *definition =
      g_selected_definition.load(std::memory_order_acquire);
  response
      << " custom_trick_consumer_enabled="
      << (REXCVAR_GET(skate3_custom_trick_native_consumer) ? 1 : 0)
      << " custom_trick_dispatch_enabled="
      << (REXCVAR_GET(skate3_custom_trick_native_dispatch) ? 1 : 0)
      << " custom_trick_scoring_enabled="
      << (REXCVAR_GET(skate3_custom_trick_scoring) ? 1 : 0)
      << " custom_trick_state="
      << StateName(g_consumer_state.load(std::memory_order_acquire))
      << " custom_trick_request_count="
      << g_request_count.load(std::memory_order_acquire)
      << " custom_trick_dispatch_count="
      << g_dispatch_count.load(std::memory_order_acquire)
      << " custom_trick_dispatch_failure_count="
      << g_dispatch_failure_count.load(std::memory_order_acquire)
      << " custom_trick_stale_request_requeue_count="
      << g_stale_request_requeue_count.load(std::memory_order_acquire)
      << " custom_trick_evaluation_count="
      << g_evaluation_count.load(std::memory_order_acquire)
      << " custom_trick_candidate_count="
      << g_candidate_count.load(std::memory_order_acquire)
      << " custom_trick_rejected_candidate_count="
      << g_rejected_candidate_count.load(std::memory_order_acquire)
      << " custom_trick_controller_match_count="
      << g_controller_match_count.load(std::memory_order_acquire)
      << " custom_trick_registry_count=" << std::size(kDefinitions)
      << " custom_trick_registry_bind_count="
      << g_registry_bind_count.load(std::memory_order_acquire)
      << " custom_trick_logical_id=" << (definition ? definition->id : 0)
      << " custom_trick_retail_proxy_id=-1"
      << " custom_trick_definition_name="
      << (definition ? definition->name : std::string_view{"none"})
      << " custom_trick_display_name="
      << (definition ? definition->display_name : std::string_view{"none"})
      << " custom_trick_base_points="
      << (definition ? definition->base_points : 0)
      << " custom_trick_board_animation="
      << (definition ? definition->board_animation : std::string_view{"none"})
      << " custom_trick_score_state="
      << ScoreStateName(g_score_state.load(std::memory_order_acquire))
      << " custom_trick_carrier_bind_count="
      << g_carrier_bind_count.load(std::memory_order_acquire)
      << " custom_trick_carrier_restore_count="
      << g_carrier_restore_count.load(std::memory_order_acquire)
      << " custom_trick_carrier_restored_points="
      << g_carrier_restored_points.load(std::memory_order_acquire)
      << " custom_trick_carrier_scorable="
      << g_carrier_scorable.load(std::memory_order_acquire)
      << " custom_trick_carrier_phys_out="
      << g_carrier_phys_out.load(std::memory_order_acquire)
      << " custom_trick_carrier_retail_id="
      << g_carrier_retail_id.load(std::memory_order_acquire)
      << " custom_trick_carrier_original_points="
      << g_carrier_original_points.load(std::memory_order_acquire)
      << " custom_trick_score_holder="
      << g_score_holder.load(std::memory_order_acquire)
      << " custom_trick_penalty_observation_count="
      << g_penalty_observation_count.load(std::memory_order_acquire)
      << " custom_trick_penalty_adapter_count="
      << g_penalty_adapter_count.load(std::memory_order_acquire)
      << " custom_trick_penalty_full_count="
      << g_penalty_full_count.load(std::memory_order_acquire)
      << " custom_trick_penalty_half_count="
      << g_penalty_half_count.load(std::memory_order_acquire)
      << " custom_trick_penalty_result_bits="
      << g_penalty_result_bits.load(std::memory_order_acquire)
      << " custom_trick_repetition_count="
      << g_custom_repetition_count.load(std::memory_order_acquire)
      << " custom_trick_repetition_isolation_count="
      << g_repetition_isolation_count.load(std::memory_order_acquire)
      << " custom_trick_retail_repetition_count_before="
      << g_retail_repetition_count_before.load(std::memory_order_acquire)
      << " custom_trick_retail_repetition_count_after="
      << g_retail_repetition_count_after.load(std::memory_order_acquire)
      << " custom_trick_observed_retail_repetition_count="
      << g_observed_retail_repetition_count.load(std::memory_order_acquire)
      << " custom_trick_display_name_apply_count="
      << g_display_name_apply_count.load(std::memory_order_acquire)
      << " custom_trick_active_display_name="
      << (definition ? definition->display_name : std::string_view{"none"})
      << " custom_trick_committed_points="
      << g_committed_points.load(std::memory_order_acquire)
      << " custom_trick_score_commit_count="
      << g_score_commit_count.load(std::memory_order_acquire)
      << " custom_trick_score_cancel_count="
      << g_score_cancel_count.load(std::memory_order_acquire)
      << " custom_trick_bank_observation_count="
      << g_bank_observation_count.load(std::memory_order_acquire)
      << " custom_trick_bank_commit_count="
      << g_bank_commit_count.load(std::memory_order_acquire)
      << " custom_trick_bank_cancel_count="
      << g_bank_cancel_count.load(std::memory_order_acquire)
      << " custom_trick_bank_observation_frame="
      << g_bank_observation_frame.load(std::memory_order_acquire)
      << " custom_trick_bank_commit_frame="
      << g_bank_commit_frame.load(std::memory_order_acquire)
      << " custom_trick_bank_published_reward_bits="
      << g_bank_published_reward_bits.load(std::memory_order_acquire)
      << " custom_trick_bank_cumulative_reward_bits="
      << g_bank_cumulative_reward_bits.load(std::memory_order_acquire)
      << " custom_trick_lifecycle_cancel_count="
      << g_lifecycle_cancel_count.load(std::memory_order_acquire)
      << " custom_trick_lifecycle_cancel_frame="
      << g_lifecycle_cancel_frame.load(std::memory_order_acquire)
      << " custom_trick_lifecycle_complete_count="
      << g_lifecycle_complete_count.load(std::memory_order_acquire)
      << " custom_trick_lifecycle_complete_frame="
      << g_lifecycle_complete_frame.load(std::memory_order_acquire)
      << " custom_trick_landing_policy_check_count="
      << g_landing_policy_check_count.load(std::memory_order_acquire)
      << " custom_trick_landing_policy_accept_count="
      << g_landing_policy_accept_count.load(std::memory_order_acquire)
      << " custom_trick_landing_policy_accept_frame="
      << g_landing_policy_accept_frame.load(std::memory_order_acquire)
      << " custom_trick_landing_policy_actor="
      << g_landing_policy_actor.load(std::memory_order_acquire)
      << " custom_trick_landing_grounded_frame="
      << g_landing_grounded_frame.load(std::memory_order_acquire)
      << " custom_trick_landing_grounded_entity="
      << g_landing_grounded_entity.load(std::memory_order_acquire)
      << " custom_trick_recovery_start_count="
      << g_recovery_start_count.load(std::memory_order_acquire)
      << " custom_trick_recovery_start_frame="
      << g_recovery_start_frame.load(std::memory_order_acquire)
      << " custom_trick_recovery_complete_count="
      << g_recovery_complete_count.load(std::memory_order_acquire)
      << " custom_trick_recovery_complete_frame="
      << g_recovery_complete_frame.load(std::memory_order_acquire)
      << " custom_trick_collision_wipeout_request_count="
      << g_collision_wipeout_request_count.load(std::memory_order_acquire)
      << " custom_trick_collision_wipeout_request_frame="
      << g_collision_wipeout_request_frame.load(std::memory_order_acquire)
      << " custom_trick_collision_wipeout_request_actor="
      << g_collision_wipeout_request_actor.load(std::memory_order_acquire)
      << " custom_trick_collision_wipeout_motion_match_count="
      << g_collision_wipeout_motion_match_count.load(std::memory_order_acquire)
      << " custom_trick_collision_wipeout_motion_match_frame="
      << g_collision_wipeout_motion_match_frame.load(std::memory_order_acquire)
      << " custom_trick_collision_wipeout_cancel_count="
      << g_collision_wipeout_cancel_count.load(std::memory_order_acquire)
      << " custom_trick_score_start_frame="
      << g_score_start_frame.load(std::memory_order_acquire)
      << " custom_trick_score_end_frame="
      << g_score_end_frame.load(std::memory_order_acquire)
      << " custom_trick_score_committed="
      << (g_score_state.load(std::memory_order_acquire) == ScoreState::Committed
              ? 1
              : 0)
      << " custom_trick_request_frame="
      << g_request_frame.load(std::memory_order_acquire)
      << " custom_trick_dispatch_frame="
      << g_dispatch_frame.load(std::memory_order_acquire)
      << " custom_trick_evaluation_frame="
      << g_evaluation_frame.load(std::memory_order_acquire)
      << " custom_trick_request_listener="
      << g_request_listener.load(std::memory_order_acquire)
      << " custom_trick_request_actor="
      << g_request_actor.load(std::memory_order_acquire)
      << " custom_trick_request_intents="
      << g_request_intents.load(std::memory_order_acquire)
      << " custom_trick_target_actor="
      << g_target_actor.load(std::memory_order_acquire)
      << " custom_trick_target_entity="
      << g_target_entity.load(std::memory_order_acquire)
      << " custom_trick_target_skater_anim_interface="
      << g_target_skater_anim_interface.load(std::memory_order_acquire)
      << " custom_trick_target_animation_controller="
      << g_target_animation_controller.load(std::memory_order_acquire)
      << " custom_trick_dispatch_controller="
      << g_dispatch_controller.load(std::memory_order_acquire)
      << " custom_trick_evaluation_object="
      << g_evaluation_object.load(std::memory_order_acquire)
      << " custom_trick_evaluation_clip="
      << g_evaluation_clip.load(std::memory_order_acquire)
      << " custom_trick_actor_fields=";
  for (size_t index = 0; index < kActorFieldOffsets.size(); ++index) {
    if (index != 0) {
      response << ':';
    }
    response << kActorFieldOffsets[index] << '@'
             << g_actor_field_values[index].load(std::memory_order_acquire);
  }
  response << " custom_trick_context_fields=";
  bool first_context = true;
  for (size_t slot = 0; slot < kCandidateContextCount; ++slot) {
    const uint32_t context =
        g_candidate_contexts[slot].load(std::memory_order_acquire);
    if (!context) {
      continue;
    }
    if (!first_context) {
      response << ';';
    }
    first_context = false;
    response << context << '@';
    for (size_t word = 0; word < kCandidateContextWordCount; ++word) {
      if (word != 0) {
        response << ':';
      }
      response << (word * 4) << '@'
               << g_candidate_context_words[slot][word].load(
                      std::memory_order_acquire);
    }
  }
  response << " custom_trick_events=";
  std::lock_guard lock(g_event_mutex);
  bool first = true;
  for (const auto &event : g_events) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << event;
  }
}

} // namespace skate3::custom_trick
