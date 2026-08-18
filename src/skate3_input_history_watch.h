#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

namespace skate3::input_history_watch {

struct Reader {
  uint32_t function_address;
  uint8_t channel_mask;
};

struct ProcessedFrame {
  uint64_t frame_sequence;
  uint32_t output_address;
  uint32_t source_address;
  uint32_t changed_mask;
  std::array<uint32_t, 24> value_bits;
};

struct OuterCircleGesture {
  uint64_t completed_frame{};
  uint32_t duration_frames{};
  uint32_t speed_bits{};
};

extern std::atomic<bool> g_active;

// The native input manager stores 120 controller frames of 100 bytes each.
// RegisterManager is called from its update function, before XamInputGetState.
void RegisterManager(uint32_t guest_address) noexcept;
void RegisterFrame(uint32_t guest_address) noexcept;

// Called at the entry of the game's normalized-channel state updater
// (0x82966F30). The function consumes 24 normalized float channels and turns
// them into persistent values plus pressed/released/held/repeat state. This
// records only value changes per output object, keeping focused traces compact.
void ObserveProcessedChannels(uint8_t* base, uint32_t output_address,
                              uint32_t source_address,
                              uint32_t channel_count) noexcept;

// Called by the debug REX_LOAD_U32 wrapper. The inactive path remains one
// predictable relaxed atomic load.
uint32_t ObserveLoad(const char* function_name, uint32_t guest_address,
                     uint32_t value) noexcept;
uint8_t* ObserveRawAddress(const char* function_name, uint32_t guest_address,
                           uint8_t* host_address) noexcept;

inline uint32_t MaybeObserveLoad(const char* function_name,
                                 uint32_t guest_address,
                                 uint32_t value) noexcept {
  if (g_active.load(std::memory_order_relaxed)) {
    return ObserveLoad(function_name, guest_address, value);
  }
  return value;
}

inline uint8_t* MaybeObserveRawAddress(const char* function_name,
                                       uint32_t guest_address,
                                       uint8_t* host_address) noexcept {
  if (g_active.load(std::memory_order_relaxed)) {
    return ObserveRawAddress(function_name, guest_address, host_address);
  }
  return host_address;
}

uint32_t RegisteredManagerCount() noexcept;
uint32_t RegisteredFrameCount() noexcept;
uint64_t CurrentFrameSequence() noexcept;
bool ConsumeLsRsChordPress(uint64_t maximum_age_frames) noexcept;
// Recognizes a right-stick-only clockwise circle that starts at the bottom,
// stays on the outer rim, returns to the bottom, then releases to center.
bool ConsumeOuterCircleGesture(uint64_t maximum_age_frames,
                               OuterCircleGesture& gesture) noexcept;
bool OuterCircleGestureActive(uint64_t maximum_age_frames) noexcept;
uint32_t OuterCircleCompletionCount() noexcept;
uint32_t OuterCircleRejectCount() noexcept;
bool ManualBailChordRecentlyObserved(uint64_t maximum_age_frames) noexcept;
void ResetAndArm();
std::vector<Reader> SnapshotAndDisarm();
std::vector<ProcessedFrame> SnapshotProcessedFrames();

}  // namespace skate3::input_history_watch
