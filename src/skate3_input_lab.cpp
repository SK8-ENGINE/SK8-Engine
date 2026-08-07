#include "skate3_input_lab.h"

#include "generated/skate3_init.h"
#include "skate3_custom_trick.h"
#include "skate3_demo_path.h"
#include "skate3_function_coverage.h"
#include "skate3_input_history_watch.h"
#include "skate3_mechanics_sandbox.h"
#include "skate3_native_scene.h"
#include "skate3_scoring.h"
#include "skate3_target_trace.h"
#include "skate3_trick_pipeline.h"

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_input_lab, true, "Skate 3",
                    "Enable the local background controller automation pipe");

namespace skate3::input_lab {
namespace {

using rex::kernel::xam::SyntheticInputStep;

std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_replace{true};
std::atomic<bool> g_observation_armed{false};
std::atomic<bool> g_observation_focus_active{false};
std::atomic<bool> g_player_state_valid{false};
std::atomic<bool> g_player_on_ground{false};
std::atomic<bool> g_player_in_air{false};
std::atomic<uint32_t> g_player_entity{0};
std::atomic<uint64_t> g_player_state_frame{0};
std::atomic<uintptr_t> g_guest_base{0};
std::atomic<uint32_t> g_local_player_object{0};
std::atomic<uint32_t> g_local_player_parent{0};
std::atomic<uint32_t> g_processed_input_coordinator{0};

constexpr uint32_t kGroundStatePredicate = 0x8259C9B0;
constexpr uint32_t kCreateLocalPlayer = 0x82DCAF50;
constexpr uint32_t kLocalPlayerVtable = 0x82328A2C;
constexpr uint32_t kLocalPlayerSize = 464;

std::mutex g_observation_mutex;
std::deque<std::string> g_events;
uint64_t g_observation_sequence_baseline = 0;
uint64_t g_observation_poll_baseline = 0;
bool g_seen_ollie_ground_animation = false;
bool g_seen_ollie_air_animation = false;
std::set<std::string> g_seen_tricks;
std::set<std::string> g_seen_animations;
std::set<std::string> g_seen_trick_sources;
std::set<std::string> g_seen_trick_contexts;
std::set<std::string> g_seen_focused_tricks;
std::set<std::string> g_seen_focused_trick_sources;
std::set<std::pair<std::string, uint32_t>> g_seen_trick_actors;
struct ActorTransition {
  uint64_t frame = 0;
  bool on_ground = false;
  bool in_air = false;
};
struct ActorState {
  bool initialized = false;
  bool on_ground = false;
  bool in_air = false;
  uint32_t locomotion = 0;
  std::vector<ActorTransition> transitions;
};
std::mutex g_actor_state_mutex;
std::map<uint32_t, ActorState> g_actor_states;
std::mutex g_marked_coverage_mutex;
std::vector<uint32_t> g_marked_coverage;
bool g_marked_coverage_ready = false;
std::mutex g_marked_input_readers_mutex;
std::vector<input_history_watch::Reader> g_marked_input_readers;
bool g_marked_input_readers_ready = false;
std::mutex g_marked_processed_inputs_mutex;
std::vector<input_history_watch::ProcessedFrame> g_marked_processed_inputs;
bool g_marked_processed_inputs_ready = false;
std::mutex g_marked_target_trace_mutex;
std::vector<target_trace::Event> g_marked_target_trace;
bool g_marked_target_trace_ready = false;

std::array<uint32_t, 6> g_ollie_ground_intent{};
std::array<uint32_t, 6> g_ollie_air_intent{};
bool g_canonical_intents_ready = false;

struct KnownTrickAnimation {
  const char* trick;
  const char* animation_base;
};

// Concrete MotionGraph include parameters from Tricks.xml. The common trick
// templates request _G while taking off and _A after leaving the ground.
constexpr KnownTrickAnimation kKnownTrickAnimations[] = {
    {"Ollie", "B_OLLIE"},
    {"Nollie", "B_NOLLIE"},
    {"Kickflip", "B_KICKFLIP_IN"},
    {"Heelflip", "B_HEELFLIP_IN"},
    {"PopShuvit", "B_POPSHUVIT"},
    {"FSPopShuvit", "B_FSPOPSHUVIT"},
    {"VarialKickflip", "B_VARIALKICKFLIP"},
    {"VarialHeelflip", "B_VARIALHEELFLIP"},
    {"Hardflip", "B_HARDFLIP"},
    {"InwardHeelflip", "B_INWARDHEELFLIP"},
    {"360PopShuvit", "B_360POPSHUVIT"},
    {"FS360PopShuvit", "B_FS360POPSHUVIT"},
    {"360Flip", "B_360FLIP"},
    {"Laserflip", "B_LASERFLIP"},
    {"360Hardflip", "B_360HARDFLIP"},
    {"360InwardHeelflip", "B_360INWARDHEELFLIP"},
    {"N_Kickflip", "B_N_KICKFLIP_IN"},
    {"N_Heelflip", "B_N_HEELFLIP_IN"},
    {"N_PopShuvit", "B_N_POPSHUVIT"},
    {"N_FSPopShuvit", "B_N_FSPOPSHUVIT"},
    {"N_VarialKickflip", "B_N_VARIALKICKFLIP"},
    {"N_VarialHeelflip", "B_N_VARIALHEELFLIP"},
    {"N_Hardflip", "B_N_HARDFLIP"},
    {"N_InwardHeelflip", "B_N_INWARDHEELFLIP"},
    {"N_360PopShuvit", "B_N_360POPSHUVIT"},
    {"N_FS360PopShuvit", "B_N_FS360POPSHUVIT"},
    {"N_360Flip", "B_N_360FLIP"},
    {"N_Laserflip", "B_N_LASERFLIP"},
    {"N_360Hardflip", "B_N_360HARDFLIP"},
    {"N_360InwardHeelflip", "B_N_360INWARDHEELFLIP"},
    {"Boneless", "B_FS_BONELESS"},
    {"Boneless", "B_BS_BONELESS"},
    {"FastPlant", "B_FS_FASTPLANT"},
    {"FastPlant", "B_BS_FASTPLANT"},
    {"NoComply", "B_NOCOMPLY_L"},
};

struct CanonicalTrickIntent {
  std::string trick;
  std::string animation;
  std::array<uint32_t, 6> intent{};
};

std::vector<CanonicalTrickIntent> g_canonical_trick_intents;

void PushEventLocked(std::string event) {
  constexpr size_t kMaxEvents = 64;
  if (g_events.size() == kMaxEvents) {
    g_events.pop_front();
  }
  g_events.push_back(std::move(event));
}

void ResetObservation() {
  const auto telemetry = rex::kernel::xam::GetSyntheticInputTelemetry();
  {
    std::lock_guard actor_lock(g_actor_state_mutex);
    g_actor_states.clear();
  }
  {
    std::lock_guard coverage_lock(g_marked_coverage_mutex);
    g_marked_coverage.clear();
    g_marked_coverage_ready = false;
  }
  {
    std::lock_guard readers_lock(g_marked_input_readers_mutex);
    g_marked_input_readers.clear();
    g_marked_input_readers_ready = false;
  }
  {
    std::lock_guard processed_lock(g_marked_processed_inputs_mutex);
    g_marked_processed_inputs.clear();
    g_marked_processed_inputs_ready = false;
  }
  {
    std::lock_guard trace_lock(g_marked_target_trace_mutex);
    g_marked_target_trace.clear();
    g_marked_target_trace_ready = false;
  }
  skate3::function_coverage::ResetAndArm();
  input_history_watch::ResetAndArm();
  target_trace::ResetAndArm();
  skate3::scoring::ResetAndArm();
  skate3::trick_pipeline::ResetAndArm();
  skate3::custom_trick::ResetAndArm();
  std::lock_guard lock(g_observation_mutex);
  g_events.clear();
  g_observation_sequence_baseline = telemetry.sequence_id;
  g_observation_poll_baseline = telemetry.applied_poll_count;
  g_seen_ollie_ground_animation = false;
  g_seen_ollie_air_animation = false;
  g_seen_tricks.clear();
  g_seen_animations.clear();
  g_seen_trick_sources.clear();
  g_seen_trick_contexts.clear();
  g_seen_focused_tricks.clear();
  g_seen_focused_trick_sources.clear();
  g_seen_trick_actors.clear();
  PushEventLocked("observation_armed");
  g_observation_focus_active.store(false, std::memory_order_release);
  g_observation_armed.store(true, std::memory_order_release);
}

std::string ObservationStatus() {
  const auto telemetry = rex::kernel::xam::GetSyntheticInputTelemetry();
  std::lock_guard lock(g_observation_mutex);
  const bool sequence_started =
      telemetry.sequence_id > g_observation_sequence_baseline;
  const bool sequence_completed =
      telemetry.completed_sequence_id > g_observation_sequence_baseline;
  const bool ollie_animation =
      g_seen_ollie_ground_animation || g_seen_ollie_air_animation;
  const bool ollie_confirmed = sequence_completed && ollie_animation;
  const bool trick_confirmed = sequence_completed && !g_seen_tricks.empty();

  std::ostringstream response;
  bool first = true;
  response << "OK sequence_started=" << (sequence_started ? 1 : 0)
           << " sequence_completed=" << (sequence_completed ? 1 : 0)
           << " queue_active=" << (telemetry.queue_active ? 1 : 0)
           << " polls_consumed="
           << (telemetry.applied_poll_count - g_observation_poll_baseline)
           << " ollie_ground_animation="
           << (g_seen_ollie_ground_animation ? 1 : 0)
           << " ollie_air_animation=" << (g_seen_ollie_air_animation ? 1 : 0)
           << " ollie_confirmed=" << (ollie_confirmed ? 1 : 0)
           << " trick_confirmed=" << (trick_confirmed ? 1 : 0)
           << " tricks=";
  first = true;
  for (const auto& trick : g_seen_tricks) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << trick;
  }
  response << " animations=";
  first = true;
  for (const auto& animation : g_seen_animations) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << animation;
  }
  response << " trick_sources=";
  first = true;
  for (const auto& source : g_seen_trick_sources) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << source;
  }
  response << " trick_contexts=";
  first = true;
  for (const auto& context : g_seen_trick_contexts) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << context;
  }
  response << " focused_tricks=";
  first = true;
  for (const auto& trick : g_seen_focused_tricks) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << trick;
  }
  response << " focused_trick_sources=";
  first = true;
  for (const auto& source : g_seen_focused_trick_sources) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << source;
  }
  response << " trick_lifecycles=";
  first = true;
  {
    std::lock_guard actor_lock(g_actor_state_mutex);
    for (const auto& [trick, actor] : g_seen_trick_actors) {
      const auto state = g_actor_states.find(actor);
      if (state == g_actor_states.end() || state->second.transitions.empty()) {
        continue;
      }
      if (!first) {
        response << ',';
      }
      first = false;
      response << trick << "@0x" << std::hex << std::uppercase << actor
               << std::dec << ':';
      bool first_transition = true;
      for (const auto& transition : state->second.transitions) {
        if (!first_transition) {
          response << '>';
        }
        first_transition = false;
        const char state_name =
            transition.on_ground ? 'G' : (transition.in_air ? 'A' : 'U');
        response << state_name << '@' << transition.frame;
      }
    }
  }
  response << " actor_lifecycles=";
  first = true;
  {
    std::lock_guard actor_lock(g_actor_state_mutex);
    for (const auto& [actor, state] : g_actor_states) {
      if (state.transitions.empty()) {
        continue;
      }
      if (!first) {
        response << ',';
      }
      first = false;
      response << "0x" << std::hex << std::uppercase << actor << std::dec
               << ':';
      bool first_transition = true;
      for (const auto& transition : state.transitions) {
        if (!first_transition) {
          response << '>';
        }
        first_transition = false;
        const char state_name =
            transition.on_ground ? 'G' : (transition.in_air ? 'A' : 'U');
        response << state_name << '@' << transition.frame;
      }
    }
  }
  skate3::scoring::AppendObservationFields(response);
  skate3::trick_pipeline::AppendObservationFields(response);
  skate3::custom_trick::AppendObservationFields(response);
  skate3::mechanics_sandbox::AppendTelemetry(response);
  response
           << " events=";
  first = true;
  for (const auto& event : g_events) {
    if (!first) {
      response << '|';
    }
    first = false;
    response << event;
  }
  return response.str();
}

std::string LocalPlayerSnapshot() {
  const auto base_address = g_guest_base.load(std::memory_order_acquire);
  const uint32_t object =
      g_local_player_object.load(std::memory_order_acquire);
  if (!base_address || !object) {
    return "OK present=0";
  }

  auto* base = reinterpret_cast<uint8_t*>(base_address);
  std::ostringstream response;
  response << "OK present=1 object=0x" << std::hex << std::uppercase << object
           << " parent=0x"
           << g_local_player_parent.load(std::memory_order_acquire)
           << " words=";
  bool first = true;
  for (uint32_t offset = 0; offset < kLocalPlayerSize; offset += 4) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << "0x" << offset << ":0x" << REX_LOAD_U32(object + offset);
  }
  return response.str();
}

std::string FindCoordinatorRelation(uint8_t* base, uint32_t target) {
  const uint32_t coordinator =
      g_processed_input_coordinator.load(std::memory_order_acquire);
  if (!base || !coordinator || !target) {
    return "none";
  }

  constexpr uint32_t kRootBytes = 0x200;
  constexpr uint32_t kChildBytes = 0x800;
  constexpr uint32_t kGuestHeapStart = 0x40000000;
  constexpr uint32_t kGuestHeapEnd = 0x72000000;
  for (uint32_t root_offset = 0; root_offset < kRootBytes;
       root_offset += 4) {
    const uint32_t value = REX_LOAD_U32(coordinator + root_offset);
    if (value == target) {
      std::ostringstream path;
      path << "root+0x" << std::hex << std::uppercase << root_offset;
      return path.str();
    }
    if (value < kGuestHeapStart || value >= kGuestHeapEnd) {
      continue;
    }
    for (uint32_t child_offset = 0; child_offset < kChildBytes;
         child_offset += 4) {
      if (REX_LOAD_U32(value + child_offset) != target) {
        continue;
      }
      std::ostringstream path;
      path << "root+0x" << std::hex << std::uppercase << root_offset
           << "/+0x" << child_offset;
      return path.str();
    }
  }
  return "none";
}

struct StateContextActorRelation {
  uint32_t actor = 0;
  uint32_t offset = 0;
  bool component_entry = false;
  std::optional<uint32_t> component_field_offset;
  bool matched_locomotion = false;
};

std::optional<std::pair<uint32_t, bool>> FindActorIdentityLocked(
    uint32_t value) {
  const auto actor = g_actor_states.find(value);
  if (actor != g_actor_states.end()) {
    return std::pair<uint32_t, bool>{actor->first, false};
  }
  for (const auto& [entity, state] : g_actor_states) {
    if (state.locomotion == value) {
      return std::pair<uint32_t, bool>{entity, true};
    }
  }
  return std::nullopt;
}

std::optional<StateContextActorRelation> FindStateContextActor(
    uint8_t* base, uint32_t state_context) {
  if (!base || !state_context) {
    return std::nullopt;
  }

  // PlayAnimation itself reads through state_context + 0x710, so this bounded
  // direct-field scan stays inside a range already proven valid by the retail
  // function. Match only entities independently observed by the actor-scoped
  // ground/air predicate; do not recursively follow arbitrary guest pointers.
  constexpr uint32_t kLastValidatedOffset = 0x710;
  constexpr uint32_t kGuestHeapStart = 0x40000000;
  constexpr uint32_t kGuestHeapEnd = 0x72000000;
  constexpr uint32_t kMaxComponentTableBytes = 0x1000;
  std::lock_guard actor_lock(g_actor_state_mutex);
  for (uint32_t offset = 0; offset <= kLastValidatedOffset; offset += 4) {
    const uint32_t value = REX_LOAD_U32(state_context + offset);
    if (const auto identity = FindActorIdentityLocked(value)) {
      return StateContextActorRelation{
          identity->first, offset, false, std::nullopt, identity->second};
    }
  }

  // state_context + 4 is the sorted (interface-id, component-pointer) vector
  // searched by sub_82965630 in retail PlayAnimation. Check its component
  // values directly after validating both endpoints and a conservative span.
  const uint32_t components_begin = REX_LOAD_U32(state_context + 4);
  const uint32_t components_end = REX_LOAD_U32(state_context + 8);
  if (components_begin >= kGuestHeapStart &&
      components_end >= components_begin &&
      components_end <= kGuestHeapEnd &&
      components_end - components_begin <= kMaxComponentTableBytes &&
      (components_end - components_begin) % 8 == 0) {
    for (uint32_t entry = components_begin; entry < components_end;
         entry += 8) {
      const uint32_t value = REX_LOAD_U32(entry + 4);
      if (const auto identity = FindActorIdentityLocked(value)) {
        return StateContextActorRelation{
            identity->first, (entry - components_begin) + 4, true,
            std::nullopt, identity->second};
      }
      constexpr uint32_t kComponentOwnerScanBytes = 0x40;
      if (value < kGuestHeapStart ||
          value + kComponentOwnerScanBytes > kGuestHeapEnd) {
        continue;
      }
      for (uint32_t field_offset = 0;
           field_offset < kComponentOwnerScanBytes; field_offset += 4) {
        const uint32_t field = REX_LOAD_U32(value + field_offset);
        if (const auto identity = FindActorIdentityLocked(field)) {
          return StateContextActorRelation{
              identity->first, (entry - components_begin) + 4, true,
              field_offset, identity->second};
        }
      }
    }
  }
  return std::nullopt;
}

std::string CoverageSnapshot() {
  std::vector<uint32_t> addresses;
  {
    std::lock_guard lock(g_marked_coverage_mutex);
    if (g_marked_coverage_ready) {
      addresses = g_marked_coverage;
      g_marked_coverage_ready = false;
    }
  }
  if (addresses.empty()) {
    addresses = skate3::function_coverage::SnapshotAndDisarm();
  }
  std::ostringstream response;
  response << "OK count=" << addresses.size() << " addresses=";
  bool first = true;
  for (const uint32_t address : addresses) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << "0x" << std::hex << std::uppercase << address;
  }
  return response.str();
}

std::string InputReadersSnapshot() {
  std::vector<input_history_watch::Reader> readers;
  {
    std::lock_guard lock(g_marked_input_readers_mutex);
    if (g_marked_input_readers_ready) {
      readers = g_marked_input_readers;
      g_marked_input_readers_ready = false;
    }
  }
  if (readers.empty()) {
    readers = input_history_watch::SnapshotAndDisarm();
  }

  std::ostringstream response;
  response << "OK count=" << readers.size() << " readers=";
  bool first = true;
  for (const auto& reader : readers) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << "0x" << std::hex << std::uppercase
             << reader.function_address << ':' << std::dec
             << static_cast<unsigned>(reader.channel_mask);
  }
  return response.str();
}

std::string ProcessedInputsSnapshot() {
  std::vector<input_history_watch::ProcessedFrame> frames;
  {
    std::lock_guard lock(g_marked_processed_inputs_mutex);
    if (g_marked_processed_inputs_ready) {
      frames = g_marked_processed_inputs;
      g_marked_processed_inputs_ready = false;
    }
  }
  if (frames.empty()) {
    frames = input_history_watch::SnapshotProcessedFrames();
  }

  std::ostringstream response;
  response << "OK count=" << frames.size() << " frames=";
  bool first = true;
  for (const auto& frame : frames) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << frame.frame_sequence << ":0x" << std::hex << std::uppercase
             << frame.output_address << ":0x" << frame.source_address
             << ":0x" << frame.changed_mask;
    for (const uint32_t bits : frame.value_bits) {
      response << ":0x" << bits;
    }
    response << std::dec;
  }
  return response.str();
}

std::string TargetTraceSnapshot() {
  std::vector<target_trace::Event> events;
  {
    std::lock_guard lock(g_marked_target_trace_mutex);
    if (g_marked_target_trace_ready) {
      events = g_marked_target_trace;
      g_marked_target_trace_ready = false;
    }
  }
  if (events.empty()) {
    events = target_trace::SnapshotAndDisarm();
  }
  std::ostringstream response;
  response << "OK count=" << events.size() << " events=";
  bool first = true;
  for (const auto& event : events) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << event.sequence << ':' << event.frame << ':' << event.thread
             << ":0x" << std::hex << std::uppercase << event.function
             << ":0x" << event.caller;
    for (const uint32_t argument : event.arguments) {
      response << ":0x" << argument;
    }
    response << std::dec;
  }
  return response.str();
}

void HandleSyntheticInputMarker(uint32_t marker) {
  if (marker == 1) {
    g_observation_focus_active.store(true, std::memory_order_release);
    skate3::scoring::SetFocus(true);
    skate3::trick_pipeline::SetFocus(true);
    {
      std::lock_guard lock(g_marked_coverage_mutex);
      g_marked_coverage.clear();
      g_marked_coverage_ready = false;
    }
    skate3::function_coverage::ResetAndArmCurrentThread();
    {
      std::lock_guard lock(g_marked_input_readers_mutex);
      g_marked_input_readers.clear();
      g_marked_input_readers_ready = false;
    }
    input_history_watch::ResetAndArm();
    {
      std::lock_guard processed_lock(g_marked_processed_inputs_mutex);
      g_marked_processed_inputs.clear();
      g_marked_processed_inputs_ready = false;
    }
    {
      std::lock_guard trace_lock(g_marked_target_trace_mutex);
      g_marked_target_trace.clear();
      g_marked_target_trace_ready = false;
    }
    target_trace::ResetAndArm();
  } else if (marker == 2) {
    g_observation_focus_active.store(false, std::memory_order_release);
    skate3::scoring::SetFocus(false);
    skate3::trick_pipeline::SetFocus(false);
    auto addresses = skate3::function_coverage::SnapshotAndDisarm();
    std::lock_guard lock(g_marked_coverage_mutex);
    g_marked_coverage = std::move(addresses);
    g_marked_coverage_ready = true;
    auto readers = input_history_watch::SnapshotAndDisarm();
    std::lock_guard readers_lock(g_marked_input_readers_mutex);
    g_marked_input_readers = std::move(readers);
    g_marked_input_readers_ready = true;
    auto processed_inputs = input_history_watch::SnapshotProcessedFrames();
    std::lock_guard processed_lock(g_marked_processed_inputs_mutex);
    g_marked_processed_inputs = std::move(processed_inputs);
    g_marked_processed_inputs_ready = true;
    auto trace = target_trace::SnapshotAndDisarm();
    std::lock_guard trace_lock(g_marked_target_trace_mutex);
    g_marked_target_trace = std::move(trace);
    g_marked_target_trace_ready = true;
  }
}

std::array<uint32_t, 6> ConstructAnimationIntent(PPCContext& source_ctx,
                                                  uint8_t* base,
                                                  std::string_view name,
                                                  uint32_t scratch_offset) {
  PPCContext probe_ctx = source_ctx;
  probe_ctx.r1.u32 = (source_ctx.r1.u32 - 0x1000) & ~0xFu;
  const uint32_t intent_address =
      probe_ctx.r1.u32 + 0x100 + scratch_offset;
  const uint32_t name_address = probe_ctx.r1.u32 + 0x400 + scratch_offset;
  for (uint32_t offset = 0; offset < 24; offset += 4) {
    REX_STORE_U32(intent_address + offset, 0);
  }
  for (size_t i = 0; i < name.size(); ++i) {
    REX_STORE_U8(name_address + static_cast<uint32_t>(i),
                 static_cast<uint8_t>(name[i]));
  }
  REX_STORE_U8(name_address + static_cast<uint32_t>(name.size()), 0);

  probe_ctx.r3.u64 = intent_address;
  probe_ctx.r4.u64 = name_address;
  sub_823C3B00(probe_ctx, base);

  std::array<uint32_t, 6> result{};
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = REX_LOAD_U32(intent_address + static_cast<uint32_t>(i * 4));
  }
  return result;
}

void InitializeCanonicalTrickIntents(PPCContext& ctx, uint8_t* base) {
  if (!g_canonical_trick_intents.empty()) {
    return;
  }
  g_canonical_trick_intents.reserve(std::size(kKnownTrickAnimations) * 3);
  for (const auto& definition : kKnownTrickAnimations) {
    const std::string base_name = definition.animation_base;
    for (const std::string& animation :
         {base_name, base_name + "_G", base_name + "_A"}) {
      CanonicalTrickIntent entry{};
      entry.trick = definition.trick;
      entry.animation = animation;
      entry.intent = ConstructAnimationIntent(ctx, base, animation, 0x100);
      g_canonical_trick_intents.push_back(std::move(entry));
    }
  }
  PushEventLocked("canonical_trick_dictionary_ready");
}

bool IntentEquals(uint8_t* base, uint32_t selected_intent,
                  const std::array<uint32_t, 6>& expected) {
  for (size_t i = 0; i < expected.size(); ++i) {
    if (REX_LOAD_U32(selected_intent + static_cast<uint32_t>(i * 4)) !=
        expected[i]) {
      return false;
    }
  }
  return true;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

bool ParseInteger(std::string_view text, int64_t min_value, int64_t max_value,
                  int64_t& out) {
  text = Trim(text);
  if (text.empty()) {
    return false;
  }
  int base = 10;
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  if (text.starts_with("0x") || text.starts_with("0X")) {
    base = 16;
    text.remove_prefix(2);
  }
  if (text.empty()) {
    return false;
  }
  uint64_t magnitude = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), magnitude, base);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  const int64_t value =
      negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
  if (value < min_value || value > max_value) {
    return false;
  }
  out = value;
  return true;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  while (true) {
    const size_t offset = text.find(separator);
    parts.push_back(Trim(text.substr(0, offset)));
    if (offset == std::string_view::npos) {
      break;
    }
    text.remove_prefix(offset + 1);
  }
  return parts;
}

bool ParseMode(std::string_view text, bool& replace) {
  if (text == "REPLACE") {
    replace = true;
    return true;
  }
  if (text == "MERGE") {
    replace = false;
    return true;
  }
  return false;
}

bool ParseState(std::string_view text, bool with_polls, SyntheticInputStep& out,
                std::string& error) {
  const auto values = Split(text, ',');
  const size_t expected = with_polls ? 8 : 7;
  if (values.size() != expected &&
      !(with_polls && values.size() == expected + 1)) {
    error = "state needs buttons,lt,rt,lx,ly,rx,ry";
    if (with_polls) {
      error += ",polls";
    }
    return false;
  }

  std::array<int64_t, 9> parsed{};
  const std::array<int64_t, 9> mins = {0,      0,      0,      -32768, -32768,
                                       -32768, -32768, 1,      0};
  const std::array<int64_t, 9> maxes = {
      65535, 255, 255, 32767, 32767, 32767, 32767, UINT32_MAX, UINT32_MAX};
  for (size_t i = 0; i < values.size(); ++i) {
    if (!ParseInteger(values[i], mins[i], maxes[i], parsed[i])) {
      error = "invalid state value at field " + std::to_string(i + 1);
      return false;
    }
  }

  out.buttons = static_cast<uint16_t>(parsed[0]);
  out.left_trigger = static_cast<uint8_t>(parsed[1]);
  out.right_trigger = static_cast<uint8_t>(parsed[2]);
  out.thumb_lx = static_cast<int16_t>(parsed[3]);
  out.thumb_ly = static_cast<int16_t>(parsed[4]);
  out.thumb_rx = static_cast<int16_t>(parsed[5]);
  out.thumb_ry = static_cast<int16_t>(parsed[6]);
  out.poll_count = with_polls ? static_cast<uint32_t>(parsed[7]) : 0;
  out.marker =
      with_polls && values.size() == 9 ? static_cast<uint32_t>(parsed[8]) : 0;
  return true;
}

std::string HandleCommand(std::string_view command) {
  command = Trim(command);
  if (command == "PING") {
    return "OK PONG";
  }
  if (command == "STATUS") {
    const uint32_t gameplay_context =
        rex::kernel::guest_presence::GameplayContextValue();
    const uint32_t frame_count =
        input_history_watch::RegisteredFrameCount();
    const bool loading_or_frontend =
        native_scene::LoadingOrFrontendActive();
    const bool ready =
        gameplay_context == 1 && frame_count != 0 && !loading_or_frontend;
    std::ostringstream response;
    response << "OK enabled=" << (g_enabled.load() ? 1 : 0)
             << " mode=" << (g_replace.load() ? "replace" : "merge")
             << " pipe=Skate3InputLab input_managers="
             << input_history_watch::RegisteredManagerCount()
             << " input_frames=" << frame_count
             << " input_frame_sequence="
             << input_history_watch::CurrentFrameSequence()
             << " gameplay_context=" << gameplay_context
             << " loading_or_frontend=" << (loading_or_frontend ? 1 : 0)
             << " ready=" << (ready ? 1 : 0)
             << " boot_stage=" << demo_path::AutomationStage()
             << " frontend_state=" << demo_path::LastRequestedFrontEndState()
             << " language_seen=" << (demo_path::SeenLanguageUpdate() ? 1 : 0)
             << " player_state_valid="
             << (g_player_state_valid.load(std::memory_order_acquire) ? 1 : 0)
             << " on_ground="
             << (g_player_on_ground.load(std::memory_order_acquire) ? 1 : 0)
             << " in_air="
             << (g_player_in_air.load(std::memory_order_acquire) ? 1 : 0)
             << " player_entity="
             << g_player_entity.load(std::memory_order_acquire)
             << " player_state_frame="
             << g_player_state_frame.load(std::memory_order_acquire)
             << " local_player_object="
             << g_local_player_object.load(std::memory_order_acquire)
             << " local_player_parent="
             << g_local_player_parent.load(std::memory_order_acquire);
    mechanics_sandbox::AppendTelemetry(response);
    return response.str();
  }
  if (command == "RESET_OBSERVATION") {
    ResetObservation();
    return "OK observation reset";
  }
  if (command == "RESET_SANDBOX") {
    if (!mechanics_sandbox::RequestReset()) {
      return "ERR mechanics sandbox is not active";
    }
    SyntheticInputStep steps[2]{};
    steps[0].buttons = 0x0101;  // LB + D-pad Up: verified session marker reset.
    steps[0].poll_count = 8;
    steps[1].poll_count = 30;
    rex::kernel::xam::SetSyntheticInputMode(true);
    rex::kernel::xam::QueueSyntheticInputSequence(steps, 2);
    g_replace.store(true, std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);
    return "OK sandbox reset queued";
  }
  if (command.rfind("SANDBOX_DIAG ", 0) == 0) {
    const std::string_view mode = Trim(command.substr(13));
    std::string mode_copy(mode);
    if (!mechanics_sandbox::SetDiagnosticMode(mode_copy.c_str())) {
      return "ERR sandbox diagnostic mode must be background_only, all_dynamic, "
             "candidate_only, or candidate_world_on";
    }
    return std::string("OK sandbox diagnostic mode=") +
           mechanics_sandbox::DiagnosticModeName();
  }
  if (command == "OBSERVE") {
    return ObservationStatus();
  }
  if (command == "PLAYER") {
    return LocalPlayerSnapshot();
  }
  if (command == "COVERAGE") {
    return CoverageSnapshot();
  }
  if (command == "INPUT_READERS") {
    return InputReadersSnapshot();
  }
  if (command == "PROCESSED_INPUTS") {
    return ProcessedInputsSnapshot();
  }
  if (command == "TARGET_TRACE") {
    return TargetTraceSnapshot();
  }
  if (command == "DISABLE" || command == "CLEAR") {
    rex::kernel::xam::ClearSyntheticInput();
    g_enabled.store(false);
    return "OK disabled";
  }

  const size_t first_space = command.find(' ');
  const std::string_view verb = command.substr(0, first_space);
  if (first_space == std::string_view::npos) {
    return "ERR expected command arguments";
  }
  command.remove_prefix(first_space + 1);
  command = Trim(command);

  if (verb == "TRACE_CONFIG") {
    const auto encoded_addresses = Split(command, ',');
    if (encoded_addresses.size() > 32) {
      return "ERR targeted trace supports at most 32 functions";
    }
    std::vector<uint32_t> addresses;
    addresses.reserve(encoded_addresses.size());
    for (const auto encoded_address : encoded_addresses) {
      int64_t parsed = 0;
      if (!ParseInteger(encoded_address, 0x82380000, 0x82F9E16B,
                        parsed)) {
        return "ERR invalid targeted trace function address";
      }
      addresses.push_back(static_cast<uint32_t>(parsed));
    }
    target_trace::Configure(addresses);
    return "OK configured=" + std::to_string(addresses.size());
  }

  const size_t mode_end = command.find(' ');
  const std::string_view mode_text = command.substr(0, mode_end);
  bool replace = true;
  if (!ParseMode(mode_text, replace)) {
    return "ERR mode must be REPLACE or MERGE";
  }
  if (mode_end == std::string_view::npos) {
    return "ERR missing controller state";
  }
  command.remove_prefix(mode_end + 1);
  command = Trim(command);

  rex::kernel::xam::SetSyntheticInputMode(replace);
  g_replace.store(replace);

  if (verb == "SET") {
    SyntheticInputStep state{};
    std::string error;
    if (!ParseState(command, false, state, error)) {
      return "ERR " + error;
    }
    rex::kernel::xam::SetSyntheticInputState(state);
    g_enabled.store(true);
    return "OK state held";
  }

  if (verb == "QUEUE") {
    const auto encoded_steps = Split(command, ';');
    if (encoded_steps.empty() || encoded_steps.size() > 1024) {
      return "ERR queue needs between 1 and 1024 steps";
    }
    std::vector<SyntheticInputStep> steps;
    steps.reserve(encoded_steps.size());
    for (const auto encoded_step : encoded_steps) {
      SyntheticInputStep step{};
      std::string error;
      if (!ParseState(encoded_step, true, step, error)) {
        return "ERR " + error;
      }
      steps.push_back(step);
    }
    rex::kernel::xam::QueueSyntheticInputSequence(steps.data(), steps.size());
    g_enabled.store(true);
    return "OK queued=" + std::to_string(steps.size());
  }

  return "ERR unknown command";
}

#if defined(_WIN32)
constexpr wchar_t kPipeName[] = LR"(\\.\pipe\Skate3InputLab)";

void ServeClient(HANDLE pipe) {
  std::string request;
  std::array<char, 4096> buffer{};
  while (true) {
    DWORD bytes_read = 0;
    const BOOL succeeded =
        ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                 &bytes_read, nullptr);
    if (bytes_read != 0) {
      request.append(buffer.data(), bytes_read);
    }
    if (succeeded) {
      break;
    }
    if (GetLastError() != ERROR_MORE_DATA) {
      break;
    }
  }

  while (!request.empty() &&
         (request.back() == '\r' || request.back() == '\n' ||
          request.back() == '\0')) {
    request.pop_back();
  }
  std::string response = HandleCommand(request);
  response += "\n";
  DWORD bytes_written = 0;
  WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()),
            &bytes_written, nullptr);
  FlushFileBuffers(pipe);
}

void ServerLoop() {
  REXLOG_INFO(
      "Skate 3 input lab: listening on \\\\.\\pipe\\Skate3InputLab "
      "(window focus is not required)");
  while (true) {
    HANDLE pipe = CreateNamedPipeW(
        kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 1024 * 1024, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      REXLOG_ERROR("Skate 3 input lab: CreateNamedPipe failed ({})",
                   GetLastError());
      return;
    }
    const BOOL connected =
        ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (connected) {
      ServeClient(pipe);
      DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }
}
#endif

}  // namespace

extern "C" REX_FUNC(Skate3InputLab_GroundStatePredicateHook) {
  // This is the game's active state-graph predicate. Its original body reads
  // these same two bytes before doing any other work:
  //   entity = *(behavior_context + 4)
  //   locomotion = *(entity + 60)
  //   on_ground = locomotion[14635]
  //   in_air = locomotion[14636]
  // Observe first, then preserve the retail implementation and return value.
  const uint32_t behavior_context = ctx.r4.u32;
  if (behavior_context) {
    const uint32_t entity = REX_LOAD_U32(behavior_context + 4);
    if (entity) {
      const uint32_t locomotion = REX_LOAD_U32(entity + 60);
      if (locomotion) {
        const bool on_ground = REX_LOAD_U8(locomotion + 14635) != 0;
        const bool in_air = REX_LOAD_U8(locomotion + 14636) != 0;
        const uint64_t frame = input_history_watch::CurrentFrameSequence();
        g_player_entity.store(entity, std::memory_order_release);
        g_player_on_ground.store(on_ground, std::memory_order_release);
        g_player_in_air.store(in_air, std::memory_order_release);
        g_player_state_frame.store(frame, std::memory_order_release);
        g_player_state_valid.store(true, std::memory_order_release);
        const uint32_t local_phys_out = trick_pipeline::CurrentLocalPhysOut();
        const uint32_t local_action_actor =
            trick_pipeline::LocalActionGraphActor();
        if (local_phys_out != 0) {
          mechanics_sandbox::ObserveLocalPresentationEntity(frame,
                                                             local_phys_out);
        }
        const bool player_owned =
            local_action_actor != 0 && local_phys_out != 0 &&
            local_action_actor == mechanics_sandbox::LocalActor();
        mechanics_sandbox::ObserveLocalMotionState(
            frame, entity, on_ground, local_action_actor, local_phys_out,
            player_owned, in_air);
        custom_trick::ObserveActorMotionState(frame, base, entity, on_ground,
                                             in_air);
        trick_pipeline::ObserveLocalBoardState(frame, base, entity, on_ground);
        if (g_observation_armed.load(std::memory_order_acquire)) {
          std::lock_guard actor_lock(g_actor_state_mutex);
          auto& state = g_actor_states[entity];
          state.locomotion = locomotion;
          if (!state.initialized || state.on_ground != on_ground ||
              state.in_air != in_air) {
            state.initialized = true;
            state.on_ground = on_ground;
            state.in_air = in_air;
            constexpr size_t kMaxActorTransitions = 16;
            if (state.transitions.size() < kMaxActorTransitions) {
              state.transitions.push_back({frame, on_ground, in_air});
            }
          }
        }
      }
    }
  }
  sub_8259C9B0(ctx, base);
}

extern "C" REX_FUNC(Skate3InputLab_CreateLocalPlayerHook) {
  const uint32_t output = ctx.r3.u32;
  sub_82DCAF50(ctx, base);
  if (!output) {
    return;
  }

  const uint32_t object = REX_LOAD_U32(output);
  if (!object || REX_LOAD_U32(object) != kLocalPlayerVtable) {
    return;
  }
  g_guest_base.store(reinterpret_cast<uintptr_t>(base),
                     std::memory_order_release);
  g_local_player_parent.store(REX_LOAD_U32(object + 76),
                              std::memory_order_release);
  g_local_player_object.store(object, std::memory_order_release);
  REXLOG_INFO(
      "Skate 3 input lab: captured LocalPlayer object 0x{:08X}, parent "
      "0x{:08X}",
      object, g_local_player_parent.load(std::memory_order_acquire));
}

void ObserveLocalPlayerCreated(uint8_t* base, uint32_t object) {
  if (!base || !object || REX_LOAD_U32(object) != kLocalPlayerVtable) {
    return;
  }
  g_guest_base.store(reinterpret_cast<uintptr_t>(base),
                     std::memory_order_release);
  g_local_player_parent.store(REX_LOAD_U32(object + 76),
                              std::memory_order_release);
  g_local_player_object.store(object, std::memory_order_release);
  REXLOG_INFO(
      "Skate 3 input lab: observed LocalPlayer object 0x{:08X}, parent "
      "0x{:08X}",
      object, g_local_player_parent.load(std::memory_order_acquire));
}

void ObserveProcessedInputCoordinator(uint8_t* base, uint32_t coordinator,
                                      uint32_t player_index) {
  if (!base || !coordinator || player_index != 0) {
    return;
  }
  g_guest_base.store(reinterpret_cast<uintptr_t>(base),
                     std::memory_order_release);
  g_processed_input_coordinator.store(coordinator,
                                      std::memory_order_release);
}

uint32_t CurrentObservedPlayerEntity() {
  return g_player_entity.load(std::memory_order_acquire);
}

uint32_t ResolveAnimationContextActor(uint8_t* base,
                                      uint32_t state_context) {
  const auto relation = FindStateContextActor(base, state_context);
  return relation ? relation->actor : 0;
}

bool AnimationContextReferences(uint8_t* base, uint32_t state_context,
                                uint32_t target) {
  if (!base || !state_context || !target) {
    return false;
  }
  constexpr uint32_t kLastValidatedOffset = 0x710;
  constexpr uint32_t kGuestHeapStart = 0x40000000;
  constexpr uint32_t kGuestHeapEnd = 0x72000000;
  constexpr uint32_t kMaxComponentTableBytes = 0x1000;
  constexpr uint32_t kComponentOwnerScanBytes = 0x80;
  for (uint32_t offset = 0; offset <= kLastValidatedOffset; offset += 4) {
    if (REX_LOAD_U32(state_context + offset) == target) {
      return true;
    }
  }
  const uint32_t components_begin = REX_LOAD_U32(state_context + 4);
  const uint32_t components_end = REX_LOAD_U32(state_context + 8);
  if (components_begin < kGuestHeapStart ||
      components_end < components_begin ||
      components_end > kGuestHeapEnd ||
      components_end - components_begin > kMaxComponentTableBytes ||
      (components_end - components_begin) % 8 != 0) {
    return false;
  }
  for (uint32_t entry = components_begin; entry < components_end;
       entry += 8) {
    const uint32_t component = REX_LOAD_U32(entry + 4);
    if (component == target) {
      return true;
    }
    if (component < kGuestHeapStart ||
        component + kComponentOwnerScanBytes > kGuestHeapEnd) {
      continue;
    }
    for (uint32_t offset = 0; offset < kComponentOwnerScanBytes;
         offset += 4) {
      if (REX_LOAD_U32(component + offset) == target) {
        return true;
      }
    }
  }
  return false;
}

bool GuestObjectReferences(uint8_t* base, uint32_t root, uint32_t target) {
  if (!base || !root || !target) {
    return false;
  }
  constexpr uint32_t kGuestHeapStart = 0x40000000;
  constexpr uint32_t kGuestHeapEnd = 0x72000000;
  constexpr uint32_t kRootBytes = 0x800;
  constexpr uint32_t kChildBytes = 0x800;
  for (uint32_t root_offset = 0; root_offset < kRootBytes;
       root_offset += 4) {
    const uint32_t value = REX_LOAD_U32(root + root_offset);
    if (value == target) {
      return true;
    }
    if (value < kGuestHeapStart ||
        value + kChildBytes > kGuestHeapEnd) {
      continue;
    }
    for (uint32_t child_offset = 0; child_offset < kChildBytes;
         child_offset += 4) {
      if (REX_LOAD_U32(value + child_offset) == target) {
        return true;
      }
    }
  }
  return false;
}

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher) {
    REXLOG_WARN("Skate 3 input lab: function dispatcher unavailable");
    return;
  }
  dispatcher->SetFunction(kGroundStatePredicate,
                          &Skate3InputLab_GroundStatePredicateHook);
  dispatcher->SetFunction(kCreateLocalPlayer,
                          &Skate3InputLab_CreateLocalPlayerHook);
  REXLOG_INFO(
      "Skate 3 input lab: actor ground-state observer installed at 0x{:08X}; "
      "LocalPlayer capture installed at 0x{:08X}",
      kGroundStatePredicate, kCreateLocalPlayer);
}

void Install() {
  if (!REXCVAR_GET(skate3_input_lab) || g_started.exchange(true)) {
    return;
  }
  rex::kernel::xam::SetSyntheticInputMarkerCallback(
      &HandleSyntheticInputMarker);
#if defined(_WIN32)
  std::thread(ServerLoop).detach();
#else
  REXLOG_WARN("Skate 3 input lab: command pipe is currently Windows-only");
#endif
}

void ObservePlayAnimation(PPCContext& ctx, uint8_t* base,
                          uint32_t selected_intent,
                          uint32_t behavior_context,
                          uint32_t state_context,
                          uint32_t animation_controller) {
  if (!g_observation_armed.load(std::memory_order_acquire) ||
      !selected_intent || !base || animation_controller == 0) {
    return;
  }

  std::lock_guard lock(g_observation_mutex);
  if (g_observation_focus_active.load(std::memory_order_acquire)) {
    REXLOG_WARN(
        "cac-gesture: PlayAnimation controller=0x{:08X} behavior=0x{:08X} "
        "state=0x{:08X} intent=0x{:08X},0x{:08X},0x{:08X},0x{:08X},"
        "0x{:08X},0x{:08X}",
        animation_controller, behavior_context, state_context,
        REX_LOAD_U32(selected_intent + 0), REX_LOAD_U32(selected_intent + 4),
        REX_LOAD_U32(selected_intent + 8), REX_LOAD_U32(selected_intent + 12),
        REX_LOAD_U32(selected_intent + 16),
        REX_LOAD_U32(selected_intent + 20));
  }
  if (!g_canonical_intents_ready) {
    g_ollie_ground_intent =
        ConstructAnimationIntent(ctx, base, "B_OLLIE_G", 0);
    g_ollie_air_intent =
        ConstructAnimationIntent(ctx, base, "B_OLLIE_A", 0x80);
    g_canonical_intents_ready = true;
    PushEventLocked("canonical_ollie_intents_ready");
  }
  InitializeCanonicalTrickIntents(ctx, base);

  if (!g_seen_ollie_ground_animation &&
      IntentEquals(base, selected_intent, g_ollie_ground_intent)) {
    g_seen_ollie_ground_animation = true;
    PushEventLocked("animation:B_OLLIE_G");
  }
  if (!g_seen_ollie_air_animation &&
      IntentEquals(base, selected_intent, g_ollie_air_intent)) {
    g_seen_ollie_air_animation = true;
    PushEventLocked("animation:B_OLLIE_A");
  }

  for (const auto& candidate : g_canonical_trick_intents) {
    if (!IntentEquals(base, selected_intent, candidate.intent)) {
      continue;
    }
    const auto actor_relation = FindStateContextActor(base, state_context);
    std::ostringstream source_tag;
    source_tag << candidate.trick << "@0x" << std::hex << std::uppercase
               << animation_controller;
    if (g_seen_trick_sources.insert(source_tag.str()).second) {
      PushEventLocked("source:" + source_tag.str());
    }
    if (g_observation_focus_active.load(std::memory_order_acquire)) {
      g_seen_focused_tricks.insert(candidate.trick);
      g_seen_focused_trick_sources.insert(source_tag.str());
    }
    std::ostringstream context_tag;
    context_tag << candidate.trick << "@0x" << std::hex << std::uppercase
                << animation_controller << ":0x" << behavior_context << ":0x"
                << state_context << ":S="
                << FindCoordinatorRelation(base, state_context) << ":C="
                << FindCoordinatorRelation(base, animation_controller)
                << ":A=";
    if (actor_relation) {
      context_tag << "0x" << actor_relation->actor << '/'
                  << (actor_relation->component_entry ? "components+0x"
                                                      : "+0x")
                  << actor_relation->offset;
      if (actor_relation->component_field_offset) {
        context_tag << "/+0x" << *actor_relation->component_field_offset;
      }
      if (actor_relation->matched_locomotion) {
        context_tag << "/locomotion";
      }
    } else {
      context_tag << "none";
    }
    if (g_seen_trick_contexts.insert(context_tag.str()).second) {
      PushEventLocked("context:" + context_tag.str());
    }
    if (actor_relation) {
      g_seen_trick_actors.insert({candidate.trick, actor_relation->actor});
    }
    if (g_seen_animations.contains(candidate.animation)) {
      continue;
    }
    g_seen_animations.insert(candidate.animation);
    g_seen_tricks.insert(candidate.trick);
    PushEventLocked("trick:" + candidate.trick);
    PushEventLocked("animation:" + candidate.animation);
  }
}

}  // namespace skate3::input_lab
