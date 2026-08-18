#include "skate3_target_trace.h"

#include "skate3_input_history_watch.h"

#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <functional>
#include <thread>

namespace skate3::target_trace {
namespace {

constexpr size_t kTargetCapacity = 32;
constexpr size_t kEventCapacity = 4096;

std::array<std::atomic<uint32_t>, kTargetCapacity> g_targets{};
std::array<Event, kEventCapacity> g_events{};
std::atomic<uint64_t> g_event_count{0};

int HexDigit(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

uint32_t FunctionAddress(const char* function_name) noexcept {
  if (!function_name) {
    return 0;
  }
  const char* encoded_address = nullptr;
  for (size_t i = 0; function_name[i] != '\0' && i < 32; ++i) {
    if (function_name[i] == 's' && function_name[i + 1] == 'u' &&
        function_name[i + 2] == 'b' && function_name[i + 3] == '_') {
      encoded_address = function_name + i + 4;
      break;
    }
  }
  if (!encoded_address) {
    return 0;
  }
  uint32_t address = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(encoded_address[i]);
    if (digit < 0) {
      return 0;
    }
    address = (address << 4) | static_cast<uint32_t>(digit);
  }
  return address;
}

bool IsTarget(uint32_t address) noexcept {
  for (const auto& target : g_targets) {
    const uint32_t value = target.load(std::memory_order_relaxed);
    if (value == 0) {
      return false;
    }
    if (value == address) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::atomic<bool> g_active{false};

void Configure(const std::vector<uint32_t>& functions) {
  g_active.store(false, std::memory_order_release);
  for (auto& target : g_targets) {
    target.store(0, std::memory_order_relaxed);
  }
  const size_t count = std::min(functions.size(), g_targets.size());
  for (size_t i = 0; i < count; ++i) {
    g_targets[i].store(functions[i], std::memory_order_relaxed);
  }
  g_event_count.store(0, std::memory_order_relaxed);
}

void ResetAndArm() {
  g_active.store(false, std::memory_order_release);
  g_event_count.store(0, std::memory_order_relaxed);
  if (g_targets[0].load(std::memory_order_relaxed) != 0) {
    g_active.store(true, std::memory_order_release);
  }
}

void Record(const char* function_name, const PPCContext& ctx) noexcept {
  const uint32_t function = FunctionAddress(function_name);
  if (!IsTarget(function)) {
    return;
  }
  const uint64_t sequence =
      g_event_count.fetch_add(1, std::memory_order_relaxed);
  if (sequence >= g_events.size()) {
    return;
  }
  Event event{};
  event.sequence = sequence;
  event.frame = input_history_watch::CurrentFrameSequence();
  event.thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
  event.function = function;
  event.caller = ctx.lr;
  event.arguments = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
      ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
  };
  g_events[sequence] = event;
}

std::vector<Event> SnapshotAndDisarm() {
  g_active.store(false, std::memory_order_release);
  const uint64_t count =
      std::min<uint64_t>(g_event_count.load(std::memory_order_acquire),
                         g_events.size());
  return std::vector<Event>(g_events.begin(), g_events.begin() + count);
}

}  // namespace skate3::target_trace
