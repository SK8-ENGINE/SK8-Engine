#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace skate3::multiplayer::protocol_v12 {

inline constexpr std::uint16_t kPoseControlPayloadBytes = 20;
inline constexpr std::uint16_t kPoseGroupHeaderBytes = 24;
inline constexpr std::uint16_t kMaximumPoseFragmentBytes =
    kMaximumPayloadBytes - kPoseGroupHeaderBytes;
inline constexpr std::uint32_t kMaximumPoseGroupBytes = 64u * 1024u;
inline constexpr std::uint16_t kMaximumPoseGroupElements = 512;

enum class PoseControlType : std::uint8_t {
  kDecodedBaseline = 1,
  kRequestBaseline = 2,
};

struct PoseControl {
  PoseControlType type = PoseControlType::kDecodedBaseline;
  std::uint8_t reserved_0 = 0;
  std::uint16_t target_role = 0;
  std::uint16_t target_stream_id = 0;
  std::uint16_t reserved_1 = 0;
  std::uint32_t target_session = 0;
  std::uint32_t baseline_id = 0;
  std::uint32_t group_mask = 0;
};

enum class PoseGroupEncoding : std::uint8_t {
  // Migration bridge for the current tested word stream. More compact
  // encodings can be negotiated later without changing fragmentation.
  kV11WordStream = 1,
  kBitPackedV1 = 2,
  kSemanticDeltaV1 = 3,
  kBlockDeltaV1 = 4,
  kPredictiveDeltaV1 = 5,
  kSnappyV1 = 6,
};

struct PoseGroupHeader {
  std::uint32_t pose_id = 0;
  // Zero for a baseline; the receiver-confirmed baseline ID for a delta.
  std::uint32_t baseline_id = 0;
  std::uint32_t total_bytes = 0;
  std::uint32_t fragment_offset = 0;
  std::uint16_t element_count = 0;
  std::uint16_t fragment_bytes = 0;
  std::uint8_t fragment_index = 0;
  std::uint8_t fragment_count = 0;
  std::uint8_t group_id = 0;
  PoseGroupEncoding encoding = PoseGroupEncoding::kV11WordStream;
};

[[nodiscard]] constexpr bool PoseControlTypeValid(PoseControlType type) {
  return type >= PoseControlType::kDecodedBaseline &&
         type <= PoseControlType::kRequestBaseline;
}

[[nodiscard]] constexpr bool PoseControlShapeValid(const PoseControl &control) {
  if (!PoseControlTypeValid(control.type) || control.reserved_0 != 0 ||
      control.reserved_1 != 0 || control.target_role < 1 ||
      control.target_role > 100 || control.target_session == 0 ||
      control.group_mask == 0) {
    return false;
  }
  return control.type == PoseControlType::kRequestBaseline ||
         control.baseline_id != 0;
}

[[nodiscard]] constexpr bool
PoseControlEnvelopeShapeValid(const Envelope &envelope,
                              const PoseControl &control) {
  return EnvelopeShapeValid(envelope) &&
         envelope.kind == MessageKind::kPoseControl &&
         envelope.flags == kFlagReliable &&
         envelope.payload_bytes == kPoseControlPayloadBytes &&
         envelope.sender_role != control.target_role &&
         PoseControlShapeValid(control);
}

[[nodiscard]] constexpr bool
PoseGroupEncodingValid(PoseGroupEncoding encoding) {
  return encoding >= PoseGroupEncoding::kV11WordStream &&
         encoding <= PoseGroupEncoding::kSnappyV1;
}

[[nodiscard]] constexpr std::size_t
PoseGroupFragmentCount(std::uint32_t total_bytes) {
  return (std::size_t(total_bytes) + kMaximumPoseFragmentBytes - 1) /
         kMaximumPoseFragmentBytes;
}

[[nodiscard]] constexpr std::size_t
PoseGroupFragmentOffset(std::uint8_t fragment_index) {
  return std::size_t(fragment_index) * kMaximumPoseFragmentBytes;
}

[[nodiscard]] constexpr std::size_t
PoseGroupFragmentByteCount(std::uint32_t total_bytes,
                           std::uint8_t fragment_index) {
  const std::size_t offset = PoseGroupFragmentOffset(fragment_index);
  if (offset >= total_bytes) {
    return 0;
  }
  const std::size_t remaining = std::size_t(total_bytes) - offset;
  return remaining < kMaximumPoseFragmentBytes ? remaining
                                               : kMaximumPoseFragmentBytes;
}

[[nodiscard]] constexpr bool
PoseGroupHeaderShapeValid(const PoseGroupHeader &header, MessageKind kind) {
  if (kind != MessageKind::kPoseBaseline && kind != MessageKind::kPoseDelta) {
    return false;
  }
  const std::size_t expected_fragments =
      PoseGroupFragmentCount(header.total_bytes);
  if (header.pose_id == 0 || header.total_bytes == 0 ||
      header.total_bytes > kMaximumPoseGroupBytes ||
      header.element_count == 0 ||
      header.element_count > kMaximumPoseGroupElements ||
      header.group_id >= 32 || !PoseGroupEncodingValid(header.encoding) ||
      expected_fragments == 0 || expected_fragments > UINT8_MAX ||
      header.fragment_count != expected_fragments ||
      header.fragment_index >= header.fragment_count ||
      header.fragment_offset !=
          PoseGroupFragmentOffset(header.fragment_index) ||
      header.fragment_bytes != PoseGroupFragmentByteCount(
                                   header.total_bytes, header.fragment_index)) {
    return false;
  }
  if (kind == MessageKind::kPoseBaseline) {
    return header.baseline_id == 0 &&
           header.encoding != PoseGroupEncoding::kSemanticDeltaV1 &&
           header.encoding != PoseGroupEncoding::kBlockDeltaV1 &&
           header.encoding != PoseGroupEncoding::kPredictiveDeltaV1;
  }
  return header.baseline_id != 0 &&
         SequenceNewer(header.pose_id, header.baseline_id);
}

[[nodiscard]] constexpr bool
PoseGroupEnvelopeShapeValid(const Envelope &envelope,
                            const PoseGroupHeader &header) {
  if (!EnvelopeShapeValid(envelope) ||
      !PoseGroupHeaderShapeValid(header, envelope.kind) ||
      envelope.payload_bytes != kPoseGroupHeaderBytes + header.fragment_bytes) {
    return false;
  }
  if (envelope.kind == MessageKind::kPoseBaseline) {
    return envelope.flags == (kFlagKeyframe | kFlagExpires);
  }
  return envelope.flags == kFlagExpires;
}

[[nodiscard]] inline bool
EncodePoseControl(const PoseControl &control,
                  std::span<std::uint8_t> destination) {
  if (!PoseControlShapeValid(control) ||
      destination.size() < kPoseControlPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kPoseControlPayloadBytes));
  const bool encoded =
      writer.U8(static_cast<std::uint8_t>(control.type)) &&
      writer.U8(control.reserved_0) && writer.U16(control.target_role) &&
      writer.U16(control.target_stream_id) && writer.U16(control.reserved_1) &&
      writer.U32(control.target_session) && writer.U32(control.baseline_id) &&
      writer.U32(control.group_mask);
  return encoded && writer.offset() == kPoseControlPayloadBytes;
}

[[nodiscard]] inline bool
DecodePoseControl(std::span<const std::uint8_t> payload, PoseControl &output) {
  if (payload.size() != kPoseControlPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  PoseControl decoded;
  std::uint8_t type = 0;
  if (!reader.U8(type) || !reader.U8(decoded.reserved_0) ||
      !reader.U16(decoded.target_role) ||
      !reader.U16(decoded.target_stream_id) ||
      !reader.U16(decoded.reserved_1) || !reader.U32(decoded.target_session) ||
      !reader.U32(decoded.baseline_id) || !reader.U32(decoded.group_mask) ||
      reader.offset() != kPoseControlPayloadBytes) {
    return false;
  }
  decoded.type = static_cast<PoseControlType>(type);
  if (!PoseControlShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

[[nodiscard]] inline bool
EncodePoseGroupHeader(const PoseGroupHeader &header, MessageKind kind,
                      std::span<std::uint8_t> destination) {
  if (!PoseGroupHeaderShapeValid(header, kind) ||
      destination.size() < kPoseGroupHeaderBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(destination.first(kPoseGroupHeaderBytes));
  const bool encoded =
      writer.U32(header.pose_id) && writer.U32(header.baseline_id) &&
      writer.U32(header.total_bytes) && writer.U32(header.fragment_offset) &&
      writer.U16(header.element_count) && writer.U16(header.fragment_bytes) &&
      writer.U8(header.fragment_index) && writer.U8(header.fragment_count) &&
      writer.U8(header.group_id) &&
      writer.U8(static_cast<std::uint8_t>(header.encoding));
  return encoded && writer.offset() == kPoseGroupHeaderBytes;
}

[[nodiscard]] inline bool
DecodePoseGroupHeader(std::span<const std::uint8_t> payload, MessageKind kind,
                      PoseGroupHeader &output) {
  if (payload.size() < kPoseGroupHeaderBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload.first(kPoseGroupHeaderBytes));
  PoseGroupHeader decoded;
  std::uint8_t encoding = 0;
  if (!reader.U32(decoded.pose_id) || !reader.U32(decoded.baseline_id) ||
      !reader.U32(decoded.total_bytes) ||
      !reader.U32(decoded.fragment_offset) ||
      !reader.U16(decoded.element_count) ||
      !reader.U16(decoded.fragment_bytes) ||
      !reader.U8(decoded.fragment_index) ||
      !reader.U8(decoded.fragment_count) || !reader.U8(decoded.group_id) ||
      !reader.U8(encoding) || reader.offset() != kPoseGroupHeaderBytes) {
    return false;
  }
  decoded.encoding = static_cast<PoseGroupEncoding>(encoding);
  if (!PoseGroupHeaderShapeValid(decoded, kind) ||
      payload.size() !=
          std::size_t(kPoseGroupHeaderBytes) + decoded.fragment_bytes) {
    return false;
  }
  output = decoded;
  return true;
}

static_assert(kPoseControlPayloadBytes == 20);
static_assert(kPoseGroupHeaderBytes == 24);
static_assert(kMaximumPoseFragmentBytes == 1136);
static_assert(PoseGroupFragmentCount(kMaximumPoseGroupBytes) == 58);

} // namespace skate3::multiplayer::protocol_v12
