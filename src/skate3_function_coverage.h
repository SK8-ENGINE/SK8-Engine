#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace skate3::function_coverage {

extern std::atomic<bool> g_active;

// Called by generated Skate 3 function and internal-label probes. The inactive
// path is a single relaxed atomic load; recording uses a lock-free bitset.
void RecordFunctionName(const char* function_name) noexcept;
void RecordAddress(uint32_t address) noexcept;

inline void MaybeRecord(const char* function_name) noexcept {
  if (g_active.load(std::memory_order_relaxed)) {
    RecordFunctionName(function_name);
  }
}

inline void MaybeRecordAddress(uint32_t address) noexcept {
  if (g_active.load(std::memory_order_relaxed)) {
    RecordAddress(address);
  }
}

void ResetAndArm();
void ResetAndArmCurrentThread();
std::vector<uint32_t> SnapshotAndDisarm();

}  // namespace skate3::function_coverage
