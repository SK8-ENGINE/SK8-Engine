#include "skate3_function_coverage.h"

#include <array>
#include <bit>
#include <cstddef>
#include <functional>
#include <thread>

namespace skate3::function_coverage {
namespace {

constexpr uint32_t kCodeBase = 0x82380000;
constexpr uint32_t kCodeSize = 0x00C1E16C;
constexpr size_t kAddressSlots = (kCodeSize + 3) / 4;
constexpr size_t kCoverageWords = (kAddressSlots + 63) / 64;
constexpr size_t kTraceCapacity = 65536;

std::array<std::atomic<uint64_t>, kCoverageWords> g_coverage{};
std::array<std::atomic<uint32_t>, kTraceCapacity> g_first_hit_trace{};
std::atomic<uint32_t> g_trace_count{0};
std::atomic<size_t> g_target_thread{0};

size_t CurrentThreadToken() noexcept {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

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

}  // namespace

std::atomic<bool> g_active{false};

void RecordFunctionName(const char* function_name) noexcept {
  if (!function_name) {
    return;
  }

  // DEFINE_REX_FUNC emits the implementation as "__imp__sub_82......" and
  // aliases the shorter sub_ name to it, so __func__ reports the former.
  const char* encoded_address = nullptr;
  for (size_t i = 0; function_name[i] != '\0' && i < 32; ++i) {
    if (function_name[i] == 's' && function_name[i + 1] == 'u' &&
        function_name[i + 2] == 'b' && function_name[i + 3] == '_') {
      encoded_address = function_name + i + 4;
      break;
    }
  }
  if (!encoded_address) {
    return;
  }

  uint32_t address = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(encoded_address[i]);
    if (digit < 0) {
      return;
    }
    address = (address << 4) | static_cast<uint32_t>(digit);
  }
  RecordAddress(address);
}

void RecordAddress(uint32_t address) noexcept {
  const size_t target_thread = g_target_thread.load(std::memory_order_relaxed);
  if (target_thread != 0 && CurrentThreadToken() != target_thread) {
    return;
  }
  if (address < kCodeBase || address >= kCodeBase + kCodeSize) {
    return;
  }

  const size_t slot = (address - kCodeBase) >> 2;
  const uint64_t mask = uint64_t{1} << (slot & 63);
  const uint64_t previous =
      g_coverage[slot >> 6].fetch_or(mask, std::memory_order_relaxed);
  if ((previous & mask) == 0) {
    const uint32_t trace_index =
        g_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (trace_index < kTraceCapacity) {
      g_first_hit_trace[trace_index].store(address, std::memory_order_release);
    }
  }
}

void ResetAndArm() {
  g_active.store(false, std::memory_order_release);
  g_target_thread.store(0, std::memory_order_relaxed);
  g_trace_count.store(0, std::memory_order_relaxed);
  for (auto& address : g_first_hit_trace) {
    address.store(0, std::memory_order_relaxed);
  }
  for (auto& word : g_coverage) {
    word.store(0, std::memory_order_relaxed);
  }
  g_active.store(true, std::memory_order_release);
}

void ResetAndArmCurrentThread() {
  g_active.store(false, std::memory_order_release);
  g_target_thread.store(CurrentThreadToken(), std::memory_order_relaxed);
  g_trace_count.store(0, std::memory_order_relaxed);
  for (auto& address : g_first_hit_trace) {
    address.store(0, std::memory_order_relaxed);
  }
  for (auto& word : g_coverage) {
    word.store(0, std::memory_order_relaxed);
  }
  g_active.store(true, std::memory_order_release);
}

std::vector<uint32_t> SnapshotAndDisarm() {
  g_active.store(false, std::memory_order_release);
  std::vector<uint32_t> addresses;
  const uint32_t trace_count =
      std::min<uint32_t>(g_trace_count.load(std::memory_order_acquire),
                         static_cast<uint32_t>(kTraceCapacity));
  addresses.reserve(trace_count);
  for (uint32_t index = 0; index < trace_count; ++index) {
    const uint32_t address =
        g_first_hit_trace[index].load(std::memory_order_acquire);
    if (address != 0) {
      addresses.push_back(address);
    }
  }
  return addresses;
}

}  // namespace skate3::function_coverage
