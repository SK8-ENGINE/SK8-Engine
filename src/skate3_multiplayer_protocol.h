#pragma once

#include <cstddef>
#include <cstdint>

namespace skate3::multiplayer::protocol {

inline constexpr std::uint32_t kPacketMagic = 0x504D334Bu;  // "K3MP"
inline constexpr std::uint32_t kAnimationPacketMagic =
    0x414D334Bu;  // "K3MA"
inline constexpr std::uint32_t kAppearancePacketMagic =
    0x5041334Bu;  // "K3AP"
inline constexpr std::uint32_t kControlPacketMagic =
    0x434D334Bu;  // "K3MC"
inline constexpr std::uint16_t kProtocolVersion = 11;

// Complete vanilla CAC/ABIN hierarchy (indices 0..130). Keeping this at 128
// silently omitted hair/facial rows that are not referenced by the ordinary
// body pieces used to discover the runtime canonical palette.
inline constexpr std::uint16_t kMaximumAnimationBones = 131;
inline constexpr std::uint16_t kMaximumAnimationTracks = 32;
inline constexpr std::uint16_t kAnimationFragmentWords = 520;
inline constexpr std::uint16_t kMaximumAnimationFrameWords = 8192;
inline constexpr std::uint16_t kMaximumAnimationFragments =
    (kMaximumAnimationFrameWords + kAnimationFragmentWords - 1) /
    kAnimationFragmentWords;
inline constexpr std::uint32_t kAnimationKeyframeInterval = 20;
inline constexpr std::uint16_t kAppearanceChunkBytes = 1024;

// Recipe appearances are normally only a few KiB. Keep the larger
// compatibility cap while older peers can still send assembled vanilla
// mesh/texture bundles.
inline constexpr std::uint32_t kMaximumAppearanceBytes =
    16 * 1024 * 1024;

enum class AnimationTrackEncoding : std::uint16_t {
  kAffineRows = 0,
  kRigidQuaternion = 1,
  kAffineRowsWideTranslation = 2,
  kRigidQuaternionWideTranslation = 3,
};

enum class ControlMessageType : std::uint16_t {
  kCapabilities = 1,
  kAppearanceState = 2,
  kAppearanceRequest = 3,
};

enum class AppearanceDeliveryState : std::uint16_t {
  kUnknown = 0,
  kReceived = 1,
  kInstalled = 2,
  kFailed = 3,
};

inline constexpr std::uint32_t kCapabilityControlV1 = 1u << 0;
inline constexpr std::uint32_t kCapabilityAppearanceState = 1u << 1;
inline constexpr std::uint32_t kCapabilityAppearanceRequest = 1u << 2;
inline constexpr std::uint32_t kCapabilityProtocolV12 = 1u << 3;

#pragma pack(push, 1)
struct PosePacket {
  std::uint32_t magic = kPacketMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t byte_count = sizeof(PosePacket);
  std::uint32_t sender_role = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t sequence = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t sender_time_us = 0;
  float position[3] = {};
  float x_axis[3] = {};
  float z_axis[3] = {};
  std::uint32_t board_state_flags = 0xFFFFFFFFu;
};

struct AnimationFragmentPacket {
  std::uint32_t magic = kAnimationPacketMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t byte_count = 0;
  std::uint32_t sender_role = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t sequence = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t sender_time_us = 0;
  float root_position[3] = {};
  std::uint16_t root_bone = 0xFFFFu;
  std::uint16_t fragment_index = 0;
  std::uint16_t fragment_count = 0;
  std::uint16_t word_offset = 0;
  std::uint16_t total_words = 0;
  std::uint16_t word_count = 0;
  std::uint16_t words[kAnimationFragmentWords] = {};
};

struct AppearanceFragmentPacket {
  std::uint32_t magic = kAppearancePacketMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t byte_count = 0;
  std::uint32_t sender_role = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t appearance_id = 0;
  std::uint32_t total_bytes = 0;
  std::uint16_t chunk_index = 0;
  std::uint16_t chunk_count = 0;
  std::uint16_t chunk_bytes = 0;
  std::uint8_t bytes[kAppearanceChunkBytes] = {};
};

struct ControlPacket {
  std::uint32_t magic = kControlPacketMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t byte_count = sizeof(ControlPacket);
  std::uint32_t sender_role = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t target_role = 0;
  std::uint32_t map_hash = 0;
  ControlMessageType message_type =
      ControlMessageType::kCapabilities;
  AppearanceDeliveryState appearance_state =
      AppearanceDeliveryState::kUnknown;
  std::uint32_t capabilities = 0;
  std::uint64_t appearance_id = 0;
};
#pragma pack(pop)

[[nodiscard]] constexpr std::size_t AnimationFragmentByteCount(
    std::uint16_t word_count) {
  return offsetof(AnimationFragmentPacket, words) +
         sizeof(std::uint16_t) * word_count;
}

[[nodiscard]] constexpr std::size_t AppearanceFragmentByteCount(
    std::uint16_t chunk_bytes) {
  return offsetof(AppearanceFragmentPacket, bytes) + chunk_bytes;
}

[[nodiscard]] constexpr std::size_t AnimationFragmentCount(
    std::uint16_t total_words) {
  return (std::size_t(total_words) + kAnimationFragmentWords - 1) /
         kAnimationFragmentWords;
}

[[nodiscard]] constexpr std::size_t AnimationFragmentWordOffset(
    std::uint16_t fragment_index) {
  return std::size_t(fragment_index) * kAnimationFragmentWords;
}

[[nodiscard]] constexpr std::size_t AnimationFragmentWordCount(
    std::uint16_t total_words, std::uint16_t fragment_index) {
  const std::size_t offset =
      AnimationFragmentWordOffset(fragment_index);
  if (offset >= total_words) {
    return 0;
  }
  const std::size_t remaining = std::size_t(total_words) - offset;
  return remaining < kAnimationFragmentWords
             ? remaining
             : kAnimationFragmentWords;
}

// Every non-final fragment must fill its complete fixed-size range. Accepting
// a short fragment would mark the range received while leaving the missing
// words as zeroes in the assembled pose.
[[nodiscard]] constexpr bool AnimationFragmentShapeValid(
    const AnimationFragmentPacket& packet) {
  const std::size_t expected_fragments =
      AnimationFragmentCount(packet.total_words);
  return packet.total_words > 0 &&
         packet.total_words <= kMaximumAnimationFrameWords &&
         expected_fragments > 0 &&
         expected_fragments <= kMaximumAnimationFragments &&
         packet.fragment_count == expected_fragments &&
         packet.fragment_index < packet.fragment_count &&
         packet.word_offset ==
             AnimationFragmentWordOffset(packet.fragment_index) &&
         packet.word_count ==
             AnimationFragmentWordCount(
                 packet.total_words, packet.fragment_index);
}

[[nodiscard]] constexpr std::size_t AppearanceChunkCount(
    std::uint32_t total_bytes) {
  return (std::size_t(total_bytes) + kAppearanceChunkBytes - 1) /
         kAppearanceChunkBytes;
}

[[nodiscard]] constexpr std::size_t AppearanceChunkByteOffset(
    std::uint16_t chunk_index) {
  return std::size_t(chunk_index) * kAppearanceChunkBytes;
}

[[nodiscard]] constexpr std::size_t AppearanceChunkByteCount(
    std::uint32_t total_bytes, std::uint16_t chunk_index) {
  const std::size_t offset = AppearanceChunkByteOffset(chunk_index);
  if (offset >= total_bytes) {
    return 0;
  }
  const std::size_t remaining = std::size_t(total_bytes) - offset;
  return remaining < kAppearanceChunkBytes
             ? remaining
             : kAppearanceChunkBytes;
}

[[nodiscard]] constexpr bool AppearanceFragmentShapeValid(
    const AppearanceFragmentPacket& packet) {
  const std::size_t expected_chunks =
      AppearanceChunkCount(packet.total_bytes);
  return packet.total_bytes > 0 &&
         packet.total_bytes <= kMaximumAppearanceBytes &&
         expected_chunks > 0 &&
         packet.chunk_count == expected_chunks &&
         packet.chunk_index < packet.chunk_count &&
         packet.chunk_bytes ==
             AppearanceChunkByteCount(
                 packet.total_bytes, packet.chunk_index);
}

[[nodiscard]] constexpr bool ControlPacketShapeValid(
    const ControlPacket& packet) {
  if (packet.magic != kControlPacketMagic ||
      packet.version != kProtocolVersion ||
      packet.byte_count != sizeof(ControlPacket) ||
      packet.sender_role < 1 || packet.sender_role > 100 ||
      packet.target_role < 1 || packet.target_role > 100 ||
      packet.sender_role == packet.target_role ||
      (packet.capabilities & kCapabilityControlV1) == 0) {
    return false;
  }
  switch (packet.message_type) {
    case ControlMessageType::kCapabilities:
      return packet.appearance_state ==
                 AppearanceDeliveryState::kUnknown &&
             packet.appearance_id == 0;
    case ControlMessageType::kAppearanceState:
      return (packet.capabilities &
              kCapabilityAppearanceState) != 0 &&
             packet.appearance_id != 0 &&
             packet.appearance_state >=
                 AppearanceDeliveryState::kReceived &&
             packet.appearance_state <=
                 AppearanceDeliveryState::kFailed;
    case ControlMessageType::kAppearanceRequest:
      return (packet.capabilities &
              kCapabilityAppearanceRequest) != 0 &&
             packet.appearance_id != 0 &&
             packet.appearance_state ==
                 AppearanceDeliveryState::kUnknown;
  }
  return false;
}

[[nodiscard]] constexpr bool AppearanceTransferReceived(
    AppearanceDeliveryState state) {
  return state == AppearanceDeliveryState::kReceived ||
         state == AppearanceDeliveryState::kInstalled;
}

[[nodiscard]] constexpr bool AppearanceStateProgresses(
    AppearanceDeliveryState current,
    AppearanceDeliveryState candidate) {
  return current != candidate &&
         !(current == AppearanceDeliveryState::kInstalled &&
           candidate == AppearanceDeliveryState::kReceived);
}

// Sequence numbers use the standard half-range rule. Subtraction is
// intentionally unsigned so rollover from UINT32_MAX to zero remains a
// forward step; the signed interpretation is only used after that defined
// modulo arithmetic.
[[nodiscard]] constexpr bool SequenceNewer(std::uint32_t candidate,
                                           std::uint32_t reference) {
  return candidate != reference &&
         static_cast<std::int32_t>(candidate - reference) > 0;
}

[[nodiscard]] constexpr bool SequenceOlder(std::uint32_t candidate,
                                           std::uint32_t reference) {
  return SequenceNewer(reference, candidate);
}

[[nodiscard]] constexpr bool SequenceNewerOrEqual(
    std::uint32_t candidate, std::uint32_t reference) {
  return candidate == reference || SequenceNewer(candidate, reference);
}

static_assert(sizeof(PosePacket) == 72);
static_assert(offsetof(AnimationFragmentPacket, words) == 56);
static_assert(sizeof(AnimationFragmentPacket) == 1096);
static_assert(offsetof(AppearanceFragmentPacket, bytes) == 38);
static_assert(sizeof(AppearanceFragmentPacket) == 1062);
static_assert(sizeof(ControlPacket) == 40);

}  // namespace skate3::multiplayer::protocol
