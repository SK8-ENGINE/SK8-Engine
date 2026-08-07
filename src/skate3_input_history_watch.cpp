#include "skate3_input_history_watch.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <mutex>

namespace skate3::input_history_watch {
namespace {

constexpr uint32_t kCodeBase = 0x82380000;
constexpr uint32_t kCodeSize = 0x00C1E16C;
constexpr size_t kFunctionSlots = (kCodeSize + 3) / 4;
constexpr size_t kTraceCapacity = 16384;
constexpr size_t kManagerCapacity = 8;
constexpr size_t kFrameCapacity = 64;
constexpr size_t kProcessedChordSlotCount = 16;
constexpr uint32_t kManagerBytes = 12020;
constexpr uint32_t kFrameBaseOffset = 4;
constexpr uint32_t kFrameBytes = 100;

std::array<std::atomic<uint8_t>, kFunctionSlots> g_channel_masks{};
std::array<std::atomic<uint32_t>, kTraceCapacity> g_reader_trace{};
std::atomic<uint32_t> g_trace_count{0};
std::array<std::atomic<uint32_t>, kManagerCapacity> g_managers{};
std::array<std::atomic<uint32_t>, kFrameCapacity> g_frames{};
std::atomic<uint64_t> g_frame_sequence{0};
std::array<std::atomic<uint32_t>, kProcessedChordSlotCount>
    g_processed_chord_outputs{};
std::array<std::atomic<bool>, kProcessedChordSlotCount>
    g_processed_chord_down{};
std::atomic<uint64_t> g_ls_rs_chord_pressed_frame{0};
std::atomic<uint64_t> g_manual_bail_chord_frame{0};
std::atomic<uint64_t> g_outer_circle_completed_frame{0};
std::atomic<uint64_t> g_outer_circle_active_frame{0};
std::atomic<uint64_t> g_outer_circle_last_published_frame{0};
std::atomic<uint32_t> g_outer_circle_duration_frames{0};
std::atomic<uint32_t> g_outer_circle_speed_bits{0};
std::atomic<uint32_t> g_outer_circle_completion_count{0};
std::atomic<uint32_t> g_outer_circle_reject_count{0};

enum class OuterCircleState : uint8_t {
  Idle,
  Tracking,
  AwaitRelease,
};

struct OuterCircleTracker {
  uint32_t output_address{};
  OuterCircleState state{OuterCircleState::Idle};
  uint64_t start_frame{};
  uint64_t last_frame{};
  float last_angle{};
  float clockwise_progress{};
  float reverse_progress{};
};

std::array<OuterCircleTracker, kProcessedChordSlotCount>
    g_outer_circle_trackers{};
std::mutex g_outer_circle_mutex;

struct ProcessedState {
  uint32_t output_address = 0;
  std::array<uint32_t, 24> value_bits{};
};

std::mutex g_processed_mutex;
std::vector<ProcessedState> g_processed_states;
std::vector<ProcessedFrame> g_processed_frames;

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

uint8_t ChannelMask(uint32_t manager, uint32_t address) noexcept {
  if (address < manager + kFrameBaseOffset ||
      address >= manager + kManagerBytes) {
    return 0;
  }
  const uint32_t frame_offset =
      (address - manager - kFrameBaseOffset) % kFrameBytes;
  // Classify the whole normalized controller frame while validating the
  // recovered layout.  This also exposes accessor functions that read a
  // larger block or a neighboring stick field instead of the four initially
  // assumed right-stick directional floats.
  if (frame_offset < 64) {
    return 1u << 0;  // buttons, triggers, and raw axes
  }
  if (frame_offset < 80) {
    return 1u << 1;  // first normalized stick-direction group
  }
  return 1u << 2;    // second normalized stick-direction group
}

uint8_t ChannelMaskForRange(uint32_t manager, uint32_t address,
                            uint32_t size) noexcept {
  uint8_t result = 0;
  for (uint32_t offset = 0; offset < size; offset += 4) {
    result |= ChannelMask(manager, address + offset);
  }
  return result;
}

uint8_t FrameMaskForRange(uint32_t frame, uint32_t address,
                          uint32_t size) noexcept {
  uint8_t result = 0;
  for (uint32_t offset = 0; offset < size; offset += 4) {
    const uint32_t sample = address + offset;
    if (sample < frame || sample >= frame + kFrameBytes) {
      continue;
    }
    const uint32_t frame_offset = sample - frame;
    if (frame_offset < 64) {
      result |= 1u << 0;
    } else if (frame_offset < 80) {
      result |= 1u << 1;
    } else {
      result |= 1u << 2;
    }
  }
  return result;
}

void RecordAccess(const char* function_name, uint8_t channel_mask) noexcept {
  if (channel_mask == 0) {
    return;
  }
  const uint32_t function_address = FunctionAddress(function_name);
  if (function_address < kCodeBase ||
      function_address >= kCodeBase + kCodeSize) {
    return;
  }
  const size_t slot = (function_address - kCodeBase) >> 2;
  const uint8_t previous =
      g_channel_masks[slot].fetch_or(channel_mask, std::memory_order_relaxed);
  if (previous == 0) {
    const uint32_t trace_index =
        g_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (trace_index < kTraceCapacity) {
      g_reader_trace[trace_index].store(function_address,
                                        std::memory_order_release);
    }
  }
}

}  // namespace

std::atomic<bool> g_active{false};

void RegisterManager(uint32_t guest_address) noexcept {
  if (guest_address == 0) {
    return;
  }
  for (auto& slot : g_managers) {
    const uint32_t existing = slot.load(std::memory_order_relaxed);
    if (existing == guest_address) {
      return;
    }
    if (existing == 0) {
      uint32_t expected = 0;
      if (slot.compare_exchange_strong(expected, guest_address,
                                       std::memory_order_relaxed)) {
        return;
      }
    }
  }
}

void RegisterFrame(uint32_t guest_address) noexcept {
  g_frame_sequence.fetch_add(1, std::memory_order_relaxed);
  if (guest_address == 0) {
    return;
  }
  for (auto& slot : g_frames) {
    const uint32_t existing = slot.load(std::memory_order_relaxed);
    if (existing == guest_address) {
      return;
    }
    if (existing == 0) {
      uint32_t expected = 0;
      if (slot.compare_exchange_strong(expected, guest_address,
                                       std::memory_order_relaxed)) {
        return;
      }
    }
  }
}

void ObserveProcessedChannels(uint8_t* base, uint32_t output_address,
                              uint32_t source_address,
                              uint32_t channel_count) noexcept {
  if (output_address == 0 || source_address == 0 || channel_count != 24) {
    return;
  }

  const bool ls_rs_down =
      REX_LOAD_U32(source_address + 6 * 4) != 0 &&
      REX_LOAD_U32(source_address + 7 * 4) != 0;
  // Processed channels 10 and 11 are LT and RT. Both triggers plus both
  // stick clicks are Skate 3's intentional-wipeout chord, so that superset
  // must remain exclusively retail-owned rather than also publishing the
  // custom LS+RS token.
  const bool manual_bail_chord =
      ls_rs_down && REX_LOAD_U32(source_address + 10 * 4) != 0 &&
      REX_LOAD_U32(source_address + 11 * 4) != 0;
  if (manual_bail_chord) {
    g_manual_bail_chord_frame.store(CurrentFrameSequence(),
                                    std::memory_order_release);
  }
  const bool custom_chord_down = ls_rs_down && !manual_bail_chord;
  for (size_t index = 0; index < g_processed_chord_outputs.size(); ++index) {
    uint32_t candidate =
        g_processed_chord_outputs[index].load(std::memory_order_relaxed);
    if (candidate == 0) {
      g_processed_chord_outputs[index].compare_exchange_strong(
          candidate, output_address, std::memory_order_relaxed);
      candidate =
          g_processed_chord_outputs[index].load(std::memory_order_relaxed);
    }
    if (candidate != output_address) {
      continue;
    }
    const bool was_down = g_processed_chord_down[index].exchange(
        custom_chord_down, std::memory_order_relaxed);
    if (custom_chord_down && !was_down) {
      g_ls_rs_chord_pressed_frame.store(CurrentFrameSequence(),
                                        std::memory_order_release);
    }
    break;
  }

  // Directional normalized channels are laid out as:
  //   16=L-right, 17=L-left, 18=L-up, 19=L-down,
  //   20=R-right, 21=R-left, 22=R-up, 23=R-down.
  // Build the right-stick vector from those retail-processed channels so the
  // custom gesture observes exactly the same dead-zone-normalized signal as
  // GestureTrickMapping.
  const float right =
      std::bit_cast<float>(REX_LOAD_U32(source_address + 20 * 4));
  const float left =
      std::bit_cast<float>(REX_LOAD_U32(source_address + 21 * 4));
  const float up =
      std::bit_cast<float>(REX_LOAD_U32(source_address + 22 * 4));
  const float down =
      std::bit_cast<float>(REX_LOAD_U32(source_address + 23 * 4));
  const float x = right - left;
  const float y = up - down;
  const float radius = std::sqrt(x * x + y * y);
  const float angle = std::atan2(y, x);
  const uint64_t frame = CurrentFrameSequence();
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kStartAngle = -kPi * 0.5f;
  constexpr float kStartTolerance = kPi * 0.22f;
  constexpr float kMinimumOuterRadius = 0.68f;
  constexpr float kStartRadius = 0.82f;
  constexpr float kReleaseRadius = 0.30f;
  constexpr float kRequiredProgress = kPi * 1.82f;
  constexpr float kMaximumReverseProgress = kPi * 0.20f;
  constexpr uint64_t kMaximumDurationFrames = 72;

  {
    std::lock_guard circle_lock(g_outer_circle_mutex);
    for (auto& tracker : g_outer_circle_trackers) {
      if (tracker.output_address == 0) {
        tracker.output_address = output_address;
      }
      if (tracker.output_address != output_address) {
        continue;
      }

      if (tracker.state == OuterCircleState::Idle) {
        const auto wrapped_delta = [=](float target) {
          float delta = angle - target;
          while (delta > kPi) {
            delta -= 2.0f * kPi;
          }
          while (delta < -kPi) {
            delta += 2.0f * kPi;
          }
          return std::abs(delta);
        };
        const float start_delta = wrapped_delta(kStartAngle);
        if (radius >= kStartRadius &&
            start_delta <= kStartTolerance) {
          tracker.state = OuterCircleState::Tracking;
          tracker.start_frame = frame;
          tracker.last_frame = frame;
          tracker.last_angle = angle;
          tracker.clockwise_progress = 0.0f;
          tracker.reverse_progress = 0.0f;
          g_outer_circle_active_frame.store(frame, std::memory_order_release);
        }
        break;
      }

      if (frame < tracker.start_frame ||
          frame - tracker.start_frame > kMaximumDurationFrames) {
        tracker.state = OuterCircleState::Idle;
        g_outer_circle_reject_count.fetch_add(1, std::memory_order_relaxed);
        break;
      }

      if (tracker.state == OuterCircleState::Tracking) {
        g_outer_circle_active_frame.store(frame, std::memory_order_release);
        if (radius < kMinimumOuterRadius) {
          tracker.state = OuterCircleState::Idle;
          g_outer_circle_reject_count.fetch_add(1, std::memory_order_relaxed);
          break;
        }

        float delta = angle - tracker.last_angle;
        while (delta > kPi) {
          delta -= 2.0f * kPi;
        }
        while (delta < -kPi) {
          delta += 2.0f * kPi;
        }
        if (delta <= 0.0f) {
          tracker.clockwise_progress += -delta;
        } else {
          tracker.reverse_progress += delta;
        }
        tracker.last_angle = angle;
        tracker.last_frame = frame;
        if (tracker.reverse_progress > kMaximumReverseProgress) {
          tracker.state = OuterCircleState::Idle;
          g_outer_circle_reject_count.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        if (tracker.clockwise_progress >= kRequiredProgress) {
          const uint32_t duration = static_cast<uint32_t>(
              std::max<uint64_t>(1, frame - tracker.start_frame));
          const uint64_t last_published =
              g_outer_circle_last_published_frame.load(
                  std::memory_order_acquire);
          if (last_published == 0 || frame < last_published ||
              frame - last_published > 12) {
            const float speed =
                std::clamp(24.0f / static_cast<float>(duration), 0.35f, 2.0f);
            g_outer_circle_duration_frames.store(duration,
                                                 std::memory_order_release);
            g_outer_circle_speed_bits.store(std::bit_cast<uint32_t>(speed),
                                            std::memory_order_release);
            g_outer_circle_completed_frame.store(frame,
                                                 std::memory_order_release);
            g_outer_circle_last_published_frame.store(
                frame, std::memory_order_release);
            g_outer_circle_active_frame.store(frame, std::memory_order_release);
            g_outer_circle_completion_count.fetch_add(
                1, std::memory_order_relaxed);
          }
          tracker.state = OuterCircleState::AwaitRelease;
        }
        break;
      }

      if (radius <= kReleaseRadius) {
        tracker.state = OuterCircleState::Idle;
      }
      break;
    }
  }

  if (!g_active.load(std::memory_order_relaxed)) {
    return;
  }

  ProcessedFrame event{};
  event.frame_sequence = CurrentFrameSequence();
  event.output_address = output_address;
  event.source_address = source_address;
  for (uint32_t channel = 0; channel < event.value_bits.size(); ++channel) {
    event.value_bits[channel] = REX_LOAD_U32(source_address + channel * 4);
  }

  std::lock_guard lock(g_processed_mutex);
  auto state = std::find_if(
      g_processed_states.begin(), g_processed_states.end(),
      [output_address](const ProcessedState& candidate) {
        return candidate.output_address == output_address;
      });
  if (state == g_processed_states.end()) {
    event.changed_mask = 0x00FFFFFFu;
    g_processed_states.push_back({output_address, event.value_bits});
  } else {
    for (uint32_t channel = 0; channel < event.value_bits.size(); ++channel) {
      if (event.value_bits[channel] != state->value_bits[channel]) {
        event.changed_mask |= 1u << channel;
      }
    }
    if (event.changed_mask == 0) {
      return;
    }
    state->value_bits = event.value_bits;
  }
  constexpr size_t kProcessedCapacity = 512;
  if (g_processed_frames.size() < kProcessedCapacity) {
    g_processed_frames.push_back(event);
  }
}

uint32_t ObserveLoad(const char* function_name, uint32_t guest_address,
                     uint32_t value) noexcept {
  uint8_t channel_mask = 0;
  for (const auto& slot : g_managers) {
    const uint32_t manager = slot.load(std::memory_order_relaxed);
    if (manager != 0) {
      channel_mask |= ChannelMask(manager, guest_address);
    }
  }
  for (const auto& slot : g_frames) {
    const uint32_t frame = slot.load(std::memory_order_relaxed);
    if (frame != 0) {
      channel_mask |= FrameMaskForRange(frame, guest_address, 4);
    }
  }
  RecordAccess(function_name, channel_mask);
  return value;
}

uint8_t* ObserveRawAddress(const char* function_name, uint32_t guest_address,
                           uint8_t* host_address) noexcept {
  uint8_t channel_mask = 0;
  for (const auto& slot : g_managers) {
    const uint32_t manager = slot.load(std::memory_order_relaxed);
    if (manager != 0) {
      channel_mask |= ChannelMaskForRange(manager, guest_address, 16);
    }
  }
  for (const auto& slot : g_frames) {
    const uint32_t frame = slot.load(std::memory_order_relaxed);
    if (frame != 0) {
      channel_mask |= FrameMaskForRange(frame, guest_address, 16);
    }
  }
  RecordAccess(function_name, channel_mask);
  return host_address;
}

uint32_t RegisteredManagerCount() noexcept {
  uint32_t count = 0;
  for (const auto& slot : g_managers) {
    if (slot.load(std::memory_order_relaxed) != 0) {
      ++count;
    }
  }
  return count;
}

uint32_t RegisteredFrameCount() noexcept {
  uint32_t count = 0;
  for (const auto& slot : g_frames) {
    if (slot.load(std::memory_order_relaxed) != 0) {
      ++count;
    }
  }
  return count;
}

uint64_t CurrentFrameSequence() noexcept {
  return g_frame_sequence.load(std::memory_order_relaxed);
}

bool ConsumeLsRsChordPress(uint64_t maximum_age_frames) noexcept {
  const uint64_t pressed_frame =
      g_ls_rs_chord_pressed_frame.load(std::memory_order_acquire);
  if (pressed_frame == 0) {
    return false;
  }
  const uint64_t current_frame = CurrentFrameSequence();
  // Processed output lanes run in an order that is not stable. Defer
  // publication until the next frame so a trigger-aware lane has had the
  // remainder of this frame to classify LT+RT+LS+RS as retail manual bail.
  if (current_frame <= pressed_frame) {
    return false;
  }
  if (current_frame - pressed_frame > maximum_age_frames) {
    uint64_t expected = pressed_frame;
    g_ls_rs_chord_pressed_frame.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    return false;
  }
  const uint64_t bail_frame =
      g_manual_bail_chord_frame.load(std::memory_order_acquire);
  if (bail_frame != 0 && current_frame >= bail_frame &&
      current_frame - bail_frame <= maximum_age_frames) {
    uint64_t expected = pressed_frame;
    g_ls_rs_chord_pressed_frame.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    return false;
  }
  uint64_t expected = pressed_frame;
  return g_ls_rs_chord_pressed_frame.compare_exchange_strong(
      expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool ConsumeOuterCircleGesture(uint64_t maximum_age_frames,
                               OuterCircleGesture& gesture) noexcept {
  const uint64_t completed_frame =
      g_outer_circle_completed_frame.load(std::memory_order_acquire);
  if (!completed_frame) {
    return false;
  }
  const uint64_t current_frame = CurrentFrameSequence();
  if (current_frame < completed_frame ||
      current_frame - completed_frame > maximum_age_frames) {
    uint64_t expected = completed_frame;
    g_outer_circle_completed_frame.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    return false;
  }
  uint64_t expected = completed_frame;
  if (!g_outer_circle_completed_frame.compare_exchange_strong(
          expected, 0, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  gesture.completed_frame = completed_frame;
  gesture.duration_frames =
      g_outer_circle_duration_frames.load(std::memory_order_acquire);
  gesture.speed_bits =
      g_outer_circle_speed_bits.load(std::memory_order_acquire);
  return true;
}

bool OuterCircleGestureActive(uint64_t maximum_age_frames) noexcept {
  (void)maximum_age_frames;
  std::lock_guard circle_lock(g_outer_circle_mutex);
  return std::any_of(
      g_outer_circle_trackers.begin(), g_outer_circle_trackers.end(),
      [](const OuterCircleTracker& tracker) {
        return tracker.state == OuterCircleState::Tracking ||
               tracker.state == OuterCircleState::AwaitRelease;
      });
}

uint32_t OuterCircleCompletionCount() noexcept {
  return g_outer_circle_completion_count.load(std::memory_order_acquire);
}

uint32_t OuterCircleRejectCount() noexcept {
  return g_outer_circle_reject_count.load(std::memory_order_acquire);
}

bool ManualBailChordRecentlyObserved(uint64_t maximum_age_frames) noexcept {
  const uint64_t bail_frame =
      g_manual_bail_chord_frame.load(std::memory_order_acquire);
  const uint64_t current_frame = CurrentFrameSequence();
  return bail_frame != 0 && current_frame >= bail_frame &&
         current_frame - bail_frame <= maximum_age_frames;
}

void ResetAndArm() {
  g_active.store(false, std::memory_order_release);
  g_ls_rs_chord_pressed_frame.store(0, std::memory_order_relaxed);
  g_manual_bail_chord_frame.store(0, std::memory_order_relaxed);
  g_outer_circle_completed_frame.store(0, std::memory_order_relaxed);
  g_outer_circle_active_frame.store(0, std::memory_order_relaxed);
  g_outer_circle_last_published_frame.store(0, std::memory_order_relaxed);
  g_outer_circle_duration_frames.store(0, std::memory_order_relaxed);
  g_outer_circle_speed_bits.store(0, std::memory_order_relaxed);
  g_outer_circle_completion_count.store(0, std::memory_order_relaxed);
  g_outer_circle_reject_count.store(0, std::memory_order_relaxed);
  {
    std::lock_guard circle_lock(g_outer_circle_mutex);
    for (auto& tracker : g_outer_circle_trackers) {
      tracker = {};
    }
  }
  {
    std::lock_guard lock(g_processed_mutex);
    g_processed_states.clear();
    g_processed_frames.clear();
  }
  const uint32_t old_count =
      std::min<uint32_t>(g_trace_count.load(std::memory_order_acquire),
                         static_cast<uint32_t>(kTraceCapacity));
  for (uint32_t index = 0; index < old_count; ++index) {
    const uint32_t address =
        g_reader_trace[index].exchange(0, std::memory_order_relaxed);
    if (address >= kCodeBase && address < kCodeBase + kCodeSize) {
      g_channel_masks[(address - kCodeBase) >> 2].store(
          0, std::memory_order_relaxed);
    }
  }
  g_trace_count.store(0, std::memory_order_relaxed);
  g_active.store(true, std::memory_order_release);
}

std::vector<Reader> SnapshotAndDisarm() {
  g_active.store(false, std::memory_order_release);
  std::vector<Reader> readers;
  const uint32_t count =
      std::min<uint32_t>(g_trace_count.load(std::memory_order_acquire),
                         static_cast<uint32_t>(kTraceCapacity));
  readers.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    const uint32_t address =
        g_reader_trace[index].load(std::memory_order_acquire);
    if (address < kCodeBase || address >= kCodeBase + kCodeSize) {
      continue;
    }
    const uint8_t mask =
        g_channel_masks[(address - kCodeBase) >> 2].load(
            std::memory_order_acquire);
    if (mask != 0) {
      readers.push_back({address, mask});
    }
  }
  return readers;
}

std::vector<ProcessedFrame> SnapshotProcessedFrames() {
  std::lock_guard lock(g_processed_mutex);
  return g_processed_frames;
}

}  // namespace skate3::input_history_watch
