#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace skate3::multiplayer::protocol_v12 {

inline constexpr std::uint32_t kEnvelopeMagic =
    0x324D334Bu;  // "K3M2"
inline constexpr std::uint16_t kProtocolVersion = 12;
inline constexpr std::uint16_t kEnvelopeBytes = 40;
inline constexpr std::uint16_t kMaximumDatagramBytes = 1200;
inline constexpr std::uint16_t kMaximumPayloadBytes =
    kMaximumDatagramBytes - kEnvelopeBytes;
inline constexpr std::uint16_t kCapabilitiesPayloadBytes = 36;

enum class MessageKind : std::uint8_t {
  kCapabilities = 1,
  kRootSnapshot = 2,
  kPoseBaseline = 3,
  kPoseDelta = 4,
  kAppearanceControl = 5,
  kAppearanceChunk = 6,
  kPoseControl = 7,
};

enum EnvelopeFlag : std::uint8_t {
  kFlagKeyframe = 1u << 0,
  kFlagReliable = 1u << 1,
  kFlagExpires = 1u << 2,
};

inline constexpr std::uint8_t kKnownEnvelopeFlags =
    kFlagKeyframe | kFlagReliable | kFlagExpires;

struct Envelope {
  std::uint32_t magic = kEnvelopeMagic;
  std::uint16_t version = kProtocolVersion;
  MessageKind kind = MessageKind::kCapabilities;
  std::uint8_t flags = 0;
  std::uint16_t header_bytes = kEnvelopeBytes;
  std::uint16_t payload_bytes = 0;
  std::uint16_t sender_role = 0;
  std::uint16_t stream_id = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t sequence = 0;
  std::uint32_t acknowledged_sequence = 0;
  std::uint32_t receive_history = 0;
  std::uint64_t sender_time_us = 0;
};

inline constexpr std::uint64_t kFeatureExplicitLittleEndian =
    1ull << 0;
inline constexpr std::uint64_t kFeaturePoseAcknowledgements =
    1ull << 1;
inline constexpr std::uint64_t kFeaturePoseGroups = 1ull << 2;
inline constexpr std::uint64_t kFeatureAppearanceRecipes =
    1ull << 3;
inline constexpr std::uint64_t kKnownFeatureBits =
    kFeatureExplicitLittleEndian |
    kFeaturePoseAcknowledgements |
    kFeaturePoseGroups |
    kFeatureAppearanceRecipes;

struct Capabilities {
  std::uint64_t feature_bits = kFeatureExplicitLittleEndian;
  std::uint64_t map_hash = 0;
  std::uint64_t build_hash = 0;
  std::uint64_t content_hash = 0;
  std::uint16_t maximum_datagram_bytes =
      kMaximumDatagramBytes;
  std::uint8_t maximum_pose_groups = 1;
  std::uint8_t reserved = 0;
};

[[nodiscard]] constexpr bool MessageKindValid(MessageKind kind) {
  return kind >= MessageKind::kCapabilities &&
         kind <= MessageKind::kPoseControl;
}

[[nodiscard]] constexpr bool EnvelopeShapeValid(
    const Envelope& envelope) {
  return envelope.magic == kEnvelopeMagic &&
         envelope.version == kProtocolVersion &&
         MessageKindValid(envelope.kind) &&
         (envelope.flags & ~kKnownEnvelopeFlags) == 0 &&
         envelope.header_bytes == kEnvelopeBytes &&
         envelope.payload_bytes <= kMaximumPayloadBytes &&
         envelope.sender_role >= 1 && envelope.sender_role <= 100 &&
         envelope.sender_session != 0;
}

[[nodiscard]] constexpr bool CapabilitiesShapeValid(
    const Capabilities& capabilities) {
  return (capabilities.feature_bits &
          kFeatureExplicitLittleEndian) != 0 &&
         capabilities.maximum_datagram_bytes >=
             kEnvelopeBytes + kCapabilitiesPayloadBytes &&
         capabilities.maximum_datagram_bytes <=
             kMaximumDatagramBytes &&
         capabilities.maximum_pose_groups >= 1 &&
         capabilities.maximum_pose_groups <= 32 &&
         capabilities.reserved == 0;
}

[[nodiscard]] constexpr std::uint64_t NegotiateFeatureBits(
    std::uint64_t local_features,
    std::uint64_t remote_features) {
  return local_features & remote_features & kKnownFeatureBits;
}

[[nodiscard]] constexpr bool SequenceNewer(
    std::uint32_t candidate, std::uint32_t reference) {
  const std::uint32_t distance = candidate - reference;
  return distance != 0 && distance < 0x80000000u;
}

namespace detail {

class LittleEndianWriter {
 public:
  explicit LittleEndianWriter(std::span<std::uint8_t> bytes)
      : bytes_(bytes) {}

  bool U8(std::uint8_t value) {
    if (offset_ >= bytes_.size()) {
      return false;
    }
    bytes_[offset_++] = value;
    return true;
  }

  bool U16(std::uint16_t value) {
    return U8(static_cast<std::uint8_t>(value)) &&
           U8(static_cast<std::uint8_t>(value >> 8));
  }

  bool U32(std::uint32_t value) {
    return U16(static_cast<std::uint16_t>(value)) &&
           U16(static_cast<std::uint16_t>(value >> 16));
  }

  bool U64(std::uint64_t value) {
    return U32(static_cast<std::uint32_t>(value)) &&
           U32(static_cast<std::uint32_t>(value >> 32));
  }

  [[nodiscard]] std::size_t offset() const { return offset_; }

 private:
  std::span<std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

class LittleEndianReader {
 public:
  explicit LittleEndianReader(std::span<const std::uint8_t> bytes)
      : bytes_(bytes) {}

  bool U8(std::uint8_t& value) {
    if (offset_ >= bytes_.size()) {
      return false;
    }
    value = bytes_[offset_++];
    return true;
  }

  bool U16(std::uint16_t& value) {
    std::uint8_t low = 0;
    std::uint8_t high = 0;
    if (!U8(low) || !U8(high)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        std::uint16_t(low) | (std::uint16_t(high) << 8));
    return true;
  }

  bool U32(std::uint32_t& value) {
    std::uint16_t low = 0;
    std::uint16_t high = 0;
    if (!U16(low) || !U16(high)) {
      return false;
    }
    value = std::uint32_t(low) | (std::uint32_t(high) << 16);
    return true;
  }

  bool U64(std::uint64_t& value) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!U32(low) || !U32(high)) {
      return false;
    }
    value = std::uint64_t(low) | (std::uint64_t(high) << 32);
    return true;
  }

  [[nodiscard]] std::size_t offset() const { return offset_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

}  // namespace detail

[[nodiscard]] inline bool EncodeEnvelope(
    const Envelope& envelope, std::span<std::uint8_t> destination) {
  if (!EnvelopeShapeValid(envelope) ||
      destination.size() < kEnvelopeBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kEnvelopeBytes));
  const bool encoded =
      writer.U32(envelope.magic) &&
      writer.U16(envelope.version) &&
      writer.U8(static_cast<std::uint8_t>(envelope.kind)) &&
      writer.U8(envelope.flags) &&
      writer.U16(envelope.header_bytes) &&
      writer.U16(envelope.payload_bytes) &&
      writer.U16(envelope.sender_role) &&
      writer.U16(envelope.stream_id) &&
      writer.U32(envelope.sender_session) &&
      writer.U32(envelope.sequence) &&
      writer.U32(envelope.acknowledged_sequence) &&
      writer.U32(envelope.receive_history) &&
      writer.U64(envelope.sender_time_us);
  return encoded && writer.offset() == kEnvelopeBytes;
}

[[nodiscard]] inline bool DecodeEnvelope(
    std::span<const std::uint8_t> packet, Envelope& output) {
  if (packet.size() < kEnvelopeBytes) {
    return false;
  }
  detail::LittleEndianReader reader(packet.first(kEnvelopeBytes));
  Envelope decoded;
  std::uint8_t kind = 0;
  if (!reader.U32(decoded.magic) ||
      !reader.U16(decoded.version) ||
      !reader.U8(kind) ||
      !reader.U8(decoded.flags) ||
      !reader.U16(decoded.header_bytes) ||
      !reader.U16(decoded.payload_bytes) ||
      !reader.U16(decoded.sender_role) ||
      !reader.U16(decoded.stream_id) ||
      !reader.U32(decoded.sender_session) ||
      !reader.U32(decoded.sequence) ||
      !reader.U32(decoded.acknowledged_sequence) ||
      !reader.U32(decoded.receive_history) ||
      !reader.U64(decoded.sender_time_us) ||
      reader.offset() != kEnvelopeBytes) {
    return false;
  }
  decoded.kind = static_cast<MessageKind>(kind);
  if (!EnvelopeShapeValid(decoded) ||
      packet.size() !=
          std::size_t(kEnvelopeBytes) + decoded.payload_bytes) {
    return false;
  }
  output = decoded;
  return true;
}

[[nodiscard]] inline bool EncodeCapabilities(
    const Capabilities& capabilities,
    std::span<std::uint8_t> destination) {
  if (!CapabilitiesShapeValid(capabilities) ||
      destination.size() < kCapabilitiesPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kCapabilitiesPayloadBytes));
  const bool encoded =
      writer.U64(capabilities.feature_bits) &&
      writer.U64(capabilities.map_hash) &&
      writer.U64(capabilities.build_hash) &&
      writer.U64(capabilities.content_hash) &&
      writer.U16(capabilities.maximum_datagram_bytes) &&
      writer.U8(capabilities.maximum_pose_groups) &&
      writer.U8(capabilities.reserved);
  return encoded &&
         writer.offset() == kCapabilitiesPayloadBytes;
}

[[nodiscard]] inline bool DecodeCapabilities(
    std::span<const std::uint8_t> payload,
    Capabilities& output) {
  if (payload.size() != kCapabilitiesPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  Capabilities decoded;
  if (!reader.U64(decoded.feature_bits) ||
      !reader.U64(decoded.map_hash) ||
      !reader.U64(decoded.build_hash) ||
      !reader.U64(decoded.content_hash) ||
      !reader.U16(decoded.maximum_datagram_bytes) ||
      !reader.U8(decoded.maximum_pose_groups) ||
      !reader.U8(decoded.reserved) ||
      reader.offset() != kCapabilitiesPayloadBytes ||
      !CapabilitiesShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

// receive_history bit zero acknowledges the packet immediately before
// acknowledged_sequence, and bit 31 acknowledges the packet 32 positions
// behind it. Unsigned subtraction preserves sequence rollover semantics.
[[nodiscard]] constexpr bool SequenceAcknowledged(
    std::uint32_t candidate,
    std::uint32_t acknowledged_sequence,
    std::uint32_t receive_history) {
  if (candidate == acknowledged_sequence) {
    return true;
  }
  const std::uint32_t distance =
      acknowledged_sequence - candidate;
  return distance >= 1 && distance <= 32 &&
         (receive_history & (1u << (distance - 1))) != 0;
}

static_assert(kEnvelopeBytes == 40);
static_assert(kMaximumPayloadBytes == 1160);
static_assert(kCapabilitiesPayloadBytes == 36);

}  // namespace skate3::multiplayer::protocol_v12
