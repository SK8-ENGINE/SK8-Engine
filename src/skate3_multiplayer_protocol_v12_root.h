#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <bit>
#include <cstdint>
#include <span>

namespace skate3::multiplayer::protocol_v12 {

inline constexpr std::uint16_t kRootSnapshotStreamId = 1;
inline constexpr std::uint16_t kRootSnapshotPayloadBytes = 40;

struct RootSnapshot {
  float position[3] = {};
  float x_axis[3] = {};
  float z_axis[3] = {};
  std::uint32_t board_state_flags = 0xFFFFFFFFu;
};

[[nodiscard]] constexpr bool RootSnapshotEnvelopeShapeValid(
    const Envelope& envelope) {
  return EnvelopeShapeValid(envelope) &&
         envelope.kind == MessageKind::kRootSnapshot &&
         envelope.flags == kFlagExpires &&
         envelope.stream_id == kRootSnapshotStreamId &&
         envelope.payload_bytes == kRootSnapshotPayloadBytes;
}

[[nodiscard]] inline bool EncodeRootSnapshot(
    const RootSnapshot& snapshot,
    std::span<std::uint8_t> destination) {
  if (destination.size() < kRootSnapshotPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kRootSnapshotPayloadBytes));
  for (const float value : snapshot.position) {
    if (!writer.U32(std::bit_cast<std::uint32_t>(value))) {
      return false;
    }
  }
  for (const float value : snapshot.x_axis) {
    if (!writer.U32(std::bit_cast<std::uint32_t>(value))) {
      return false;
    }
  }
  for (const float value : snapshot.z_axis) {
    if (!writer.U32(std::bit_cast<std::uint32_t>(value))) {
      return false;
    }
  }
  return writer.U32(snapshot.board_state_flags) &&
         writer.offset() == kRootSnapshotPayloadBytes;
}

[[nodiscard]] inline bool DecodeRootSnapshot(
    std::span<const std::uint8_t> payload,
    RootSnapshot& output) {
  if (payload.size() != kRootSnapshotPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  RootSnapshot decoded;
  for (float& value : decoded.position) {
    std::uint32_t bits = 0;
    if (!reader.U32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
  }
  for (float& value : decoded.x_axis) {
    std::uint32_t bits = 0;
    if (!reader.U32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
  }
  for (float& value : decoded.z_axis) {
    std::uint32_t bits = 0;
    if (!reader.U32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
  }
  if (!reader.U32(decoded.board_state_flags) ||
      reader.offset() != kRootSnapshotPayloadBytes) {
    return false;
  }
  output = decoded;
  return true;
}

static_assert(kEnvelopeBytes + kRootSnapshotPayloadBytes == 80);

}  // namespace skate3::multiplayer::protocol_v12
