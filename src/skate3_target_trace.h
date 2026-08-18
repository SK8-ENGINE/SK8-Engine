#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

struct PPCContext;

namespace skate3::target_trace {

struct Event {
  uint64_t sequence;
  uint64_t frame;
  uint64_t thread;
  uint32_t function;
  uint32_t caller;
  std::array<uint32_t, 8> arguments;
};

extern std::atomic<bool> g_active;

void Configure(const std::vector<uint32_t>& functions);
void ResetAndArm();
std::vector<Event> SnapshotAndDisarm();
void Record(const char* function_name, const PPCContext& ctx) noexcept;

inline void MaybeRecord(const char* function_name,
                        const PPCContext& ctx) noexcept {
  if (g_active.load(std::memory_order_relaxed)) {
    Record(function_name, ctx);
  }
}

}  // namespace skate3::target_trace
