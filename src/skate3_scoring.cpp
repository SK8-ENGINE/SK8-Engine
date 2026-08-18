#include "skate3_scoring.h"

#include "generated/skate3_init.h"
#include "skate3_input_history_watch.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <vector>

namespace skate3::scoring {
namespace {

// ScoringTrick is registered by sub_82F8A380. Its factory sub_82BCC030
// installs vtable 0x823214D0; behavior slot +0x34 is this action.
constexpr uint32_t kExecuteScoringTrick = 0x82BBFA88;
constexpr uint32_t kStateContextScoringTargetOffset = 0x708;
constexpr uint32_t kScoringTargetMethodOffset = 0xEC;
constexpr uint32_t kNodeParameterOffset = 0x1C;
constexpr size_t kParameterWordCount = 5;
constexpr size_t kEventCapacity = 128;
constexpr size_t kFocusedEventCapacity = 64;

struct Event {
  uint64_t frame = 0;
  uint32_t node = 0;
  uint32_t behavior_context = 0;
  uint32_t state_context = 0;
  uint32_t scoring_target = 0;
  uint32_t scoring_method = 0;
  std::array<uint32_t, kParameterWordCount> parameter_words{};
  bool focused = false;
};

std::atomic<bool> g_armed{false};
std::atomic<bool> g_focused{false};
std::atomic<uint64_t> g_event_count{0};
std::mutex g_event_mutex;
std::vector<Event> g_events;
std::vector<Event> g_focused_events;

void AppendEvent(std::ostream& response, const Event& event) {
  response << event.frame << ":0x" << std::hex << std::uppercase << event.node
           << ":0x" << event.behavior_context << ":0x" << event.state_context
           << ":0x" << event.scoring_target << ":0x" << event.scoring_method;
  for (const uint32_t word : event.parameter_words) {
    response << ":0x" << word;
  }
  response << std::dec;
}

void Observe(uint8_t* base, uint32_t node, uint32_t behavior_context) {
  if (!g_armed.load(std::memory_order_acquire) || !base || !node ||
      !behavior_context) {
    return;
  }

  Event event{};
  event.frame = input_history_watch::CurrentFrameSequence();
  event.node = node;
  event.behavior_context = behavior_context;
  event.state_context = REX_LOAD_U32(behavior_context + 4);
  if (event.state_context) {
    event.scoring_target =
        REX_LOAD_U32(event.state_context + kStateContextScoringTargetOffset);
  }
  if (event.scoring_target) {
    const uint32_t vtable = REX_LOAD_U32(event.scoring_target);
    if (vtable) {
      event.scoring_method =
          REX_LOAD_U32(vtable + kScoringTargetMethodOffset);
    }
  }
  for (size_t i = 0; i < event.parameter_words.size(); ++i) {
    event.parameter_words[i] =
        REX_LOAD_U32(node + kNodeParameterOffset +
                     static_cast<uint32_t>(i * sizeof(uint32_t)));
  }
  event.focused = g_focused.load(std::memory_order_acquire);
  g_event_count.fetch_add(1, std::memory_order_relaxed);

  std::lock_guard lock(g_event_mutex);
  if (g_events.size() < kEventCapacity) {
    g_events.push_back(event);
  }
  if (event.focused && g_focused_events.size() < kFocusedEventCapacity) {
    g_focused_events.push_back(event);
  }
}

}  // namespace

void ObserveExecuteTrick(PPCContext& ctx, uint8_t* base) {
  Observe(base, ctx.r3.u32, ctx.r4.u32);
}

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher) {
    REXLOG_WARN("Skate 3 scoring: function dispatcher unavailable");
    return;
  }
  REXLOG_INFO(
      "Skate 3 scoring: ScoringTrick generated-entry observer active at "
      "0x{:08X}",
      kExecuteScoringTrick);
}

void ResetAndArm() {
  g_armed.store(false, std::memory_order_release);
  g_focused.store(false, std::memory_order_release);
  {
    std::lock_guard lock(g_event_mutex);
    g_events.clear();
    g_focused_events.clear();
  }
  g_event_count.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_release);
}

void SetFocus(bool focused) {
  g_focused.store(focused, std::memory_order_release);
}

void AppendObservationFields(std::ostream& response) {
  std::lock_guard lock(g_event_mutex);
  response << " scoring_count="
           << g_event_count.load(std::memory_order_acquire)
           << " focused_scoring_count=" << g_focused_events.size()
           << " scoring_events=";
  bool first = true;
  for (const auto& event : g_events) {
    if (!first) {
      response << ',';
    }
    first = false;
    AppendEvent(response, event);
  }

  response << " focused_scoring_events=";
  first = true;
  for (const auto& event : g_focused_events) {
    if (!first) {
      response << ',';
    }
    first = false;
    AppendEvent(response, event);
  }
}

}  // namespace skate3::scoring
