#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

inline constexpr std::uint16_t kMapEditStreamId = 3;
inline constexpr std::uint16_t kMapEditControlPayloadBytes = 80;
inline constexpr std::uint16_t kMapEditSpawnHeaderBytes = 68;
inline constexpr std::uint16_t kMapEditSpawnFragmentBytes =
    kMaximumPayloadBytes - kMapEditSpawnHeaderBytes;
inline constexpr std::uint32_t kMaximumMapEditPackageBytes =
    64u * 1024u * 1024u;

enum class MapEditControlType : std::uint8_t {
  kSnapshotBegin = 1,
  kSnapshotEnd = 2,
  kTransformPreviewRequest = 3,
  kTransformCommitRequest = 4,
  kTransformPreviewApply = 5,
  kTransformCommitApply = 6,
};

enum class MapEditSpawnType : std::uint8_t {
  kSpawnRequest = 1,
  kSpawnApply = 2,
};

struct MapEditControl {
  MapEditControlType type = MapEditControlType::kSnapshotBegin;
  std::uint16_t source_role = 0;
  std::uint32_t object_id = 0;
  std::uint64_t request_id = 0;
  std::uint64_t authority_revision = 0;
  std::uint64_t snapshot_id = 0;
  float translation[3] = {};
  float basis[9] = {
      1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
  };
};

struct MapEditSpawnHeader {
  MapEditSpawnType type = MapEditSpawnType::kSpawnRequest;
  std::uint16_t source_role = 0;
  std::uint64_t request_id = 0;
  std::uint64_t authority_revision = 0;
  std::uint64_t snapshot_id = 0;
  std::uint32_t total_bytes = 0;
  std::uint32_t fragment_index = 0;
  std::uint32_t fragment_count = 0;
  std::uint32_t fragment_offset = 0;
  std::uint16_t fragment_bytes = 0;
  std::uint64_t content_hash = 0;
  float position[3] = {};
};

[[nodiscard]] constexpr bool MapEditControlTypeValid(MapEditControlType type) {
  return type >= MapEditControlType::kSnapshotBegin &&
         type <= MapEditControlType::kTransformCommitApply;
}

[[nodiscard]] constexpr bool MapEditSpawnTypeValid(MapEditSpawnType type) {
  return type >= MapEditSpawnType::kSpawnRequest &&
         type <= MapEditSpawnType::kSpawnApply;
}

[[nodiscard]] inline bool FiniteMapEditVector(const float values[3]) {
  return std::isfinite(values[0]) && std::isfinite(values[1]) &&
         std::isfinite(values[2]);
}

[[nodiscard]] inline bool MapEditBasisValid(const float basis[9]) {
  for (std::size_t index = 0; index < 9; ++index) {
    if (!std::isfinite(basis[index])) {
      return false;
    }
  }
  const auto length_squared = [basis](std::size_t row) {
    const std::size_t offset = row * 3;
    return basis[offset] * basis[offset] +
           basis[offset + 1] * basis[offset + 1] +
           basis[offset + 2] * basis[offset + 2];
  };
  const auto dot = [basis](std::size_t left, std::size_t right) {
    const std::size_t a = left * 3;
    const std::size_t b = right * 3;
    return basis[a] * basis[b] + basis[a + 1] * basis[b + 1] +
           basis[a + 2] * basis[b + 2];
  };
  return std::abs(length_squared(0) - 1.0f) <= 0.02f &&
         std::abs(length_squared(1) - 1.0f) <= 0.02f &&
         std::abs(length_squared(2) - 1.0f) <= 0.02f &&
         std::abs(dot(0, 1)) <= 0.02f && std::abs(dot(0, 2)) <= 0.02f &&
         std::abs(dot(1, 2)) <= 0.02f;
}

[[nodiscard]] inline bool
MapEditControlShapeValid(const MapEditControl &control) {
  if (!MapEditControlTypeValid(control.type) || control.source_role < 1 ||
      control.source_role > 100) {
    return false;
  }
  if (control.type == MapEditControlType::kSnapshotBegin ||
      control.type == MapEditControlType::kSnapshotEnd) {
    return control.snapshot_id != 0;
  }
  return control.object_id != 0 && control.request_id != 0 &&
         FiniteMapEditVector(control.translation) &&
         MapEditBasisValid(control.basis);
}

[[nodiscard]] constexpr bool
MapEditControlEnvelopeShapeValid(const Envelope &envelope,
                                 const MapEditControl &control) {
  const bool preview =
      control.type == MapEditControlType::kTransformPreviewRequest ||
      control.type == MapEditControlType::kTransformPreviewApply;
  return EnvelopeShapeValid(envelope) &&
         envelope.kind == MessageKind::kMapEditControl &&
         envelope.stream_id == kMapEditStreamId &&
         envelope.payload_bytes == kMapEditControlPayloadBytes &&
         envelope.flags == (preview ? kFlagExpires : kFlagReliable);
}

[[nodiscard]] inline bool
EncodeMapEditControl(const MapEditControl &control,
                     std::span<std::uint8_t> destination) {
  if (!MapEditControlShapeValid(control) ||
      destination.size() < kMapEditControlPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kMapEditControlPayloadBytes));
  bool encoded =
      writer.U8(static_cast<std::uint8_t>(control.type)) && writer.U8(0) &&
      writer.U16(control.source_role) && writer.U32(control.object_id) &&
      writer.U64(control.request_id) &&
      writer.U64(control.authority_revision) && writer.U64(control.snapshot_id);
  for (const float value : control.translation) {
    encoded = encoded && writer.U32(std::bit_cast<std::uint32_t>(value));
  }
  for (const float value : control.basis) {
    encoded = encoded && writer.U32(std::bit_cast<std::uint32_t>(value));
  }
  return encoded && writer.offset() == kMapEditControlPayloadBytes;
}

[[nodiscard]] inline bool
DecodeMapEditControl(std::span<const std::uint8_t> payload,
                     MapEditControl &output) {
  if (payload.size() != kMapEditControlPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  MapEditControl decoded;
  std::uint8_t type = 0;
  std::uint8_t reserved = 0;
  if (!reader.U8(type) || !reader.U8(reserved) ||
      !reader.U16(decoded.source_role) || !reader.U32(decoded.object_id) ||
      !reader.U64(decoded.request_id) ||
      !reader.U64(decoded.authority_revision) ||
      !reader.U64(decoded.snapshot_id)) {
    return false;
  }
  for (float &value : decoded.translation) {
    std::uint32_t bits = 0;
    if (!reader.U32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
  }
  for (float &value : decoded.basis) {
    std::uint32_t bits = 0;
    if (!reader.U32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
  }
  decoded.type = static_cast<MapEditControlType>(type);
  if (reserved != 0 || reader.offset() != kMapEditControlPayloadBytes ||
      !MapEditControlShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

[[nodiscard]] constexpr std::uint32_t
MapEditSpawnFragmentCount(std::uint32_t total_bytes) {
  return total_bytes == 0 || total_bytes > kMaximumMapEditPackageBytes
             ? 0
             : (total_bytes + kMapEditSpawnFragmentBytes - 1) /
                   kMapEditSpawnFragmentBytes;
}

[[nodiscard]] constexpr std::uint32_t
MapEditSpawnFragmentOffset(std::uint32_t fragment_index) {
  return fragment_index * kMapEditSpawnFragmentBytes;
}

[[nodiscard]] constexpr std::uint16_t
MapEditSpawnFragmentByteCount(std::uint32_t total_bytes,
                              std::uint32_t fragment_index) {
  const std::uint32_t offset = MapEditSpawnFragmentOffset(fragment_index);
  return offset >= total_bytes
             ? 0
             : static_cast<std::uint16_t>(std::min<std::uint32_t>(
                   kMapEditSpawnFragmentBytes, total_bytes - offset));
}

[[nodiscard]] inline std::uint64_t
MapEditContentHash(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

[[nodiscard]] inline bool
MapEditSpawnHeaderShapeValid(const MapEditSpawnHeader &header) {
  return MapEditSpawnTypeValid(header.type) && header.source_role >= 1 &&
         header.source_role <= 100 && header.request_id != 0 &&
         header.total_bytes != 0 &&
         header.total_bytes <= kMaximumMapEditPackageBytes &&
         header.fragment_count ==
             MapEditSpawnFragmentCount(header.total_bytes) &&
         header.fragment_index < header.fragment_count &&
         header.fragment_offset ==
             MapEditSpawnFragmentOffset(header.fragment_index) &&
         header.fragment_bytes ==
             MapEditSpawnFragmentByteCount(header.total_bytes,
                                           header.fragment_index) &&
         header.content_hash != 0 && FiniteMapEditVector(header.position);
}

[[nodiscard]] constexpr bool
MapEditSpawnEnvelopeShapeValid(const Envelope &envelope,
                               const MapEditSpawnHeader &header) {
  return EnvelopeShapeValid(envelope) &&
         envelope.kind == MessageKind::kMapEditSpawnChunk &&
         envelope.stream_id == kMapEditStreamId &&
         envelope.flags == kFlagReliable &&
         envelope.payload_bytes ==
             kMapEditSpawnHeaderBytes + header.fragment_bytes;
}

[[nodiscard]] inline bool
EncodeMapEditSpawnDatagram(const Envelope &envelope,
                           const MapEditSpawnHeader &header,
                           std::span<const std::uint8_t> package,
                           std::span<std::uint8_t> destination) {
  if (!MapEditSpawnHeaderShapeValid(header) ||
      !MapEditSpawnEnvelopeShapeValid(envelope, header) ||
      package.size() != header.total_bytes ||
      destination.size() !=
          std::size_t(kEnvelopeBytes) + envelope.payload_bytes ||
      !EncodeEnvelope(envelope, destination)) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.subspan(kEnvelopeBytes, kMapEditSpawnHeaderBytes));
  bool encoded =
      writer.U8(static_cast<std::uint8_t>(header.type)) && writer.U8(0) &&
      writer.U16(header.source_role) && writer.U64(header.request_id) &&
      writer.U64(header.authority_revision) && writer.U64(header.snapshot_id) &&
      writer.U32(header.total_bytes) && writer.U32(header.fragment_index) &&
      writer.U32(header.fragment_count) && writer.U32(header.fragment_offset) &&
      writer.U16(header.fragment_bytes) && writer.U16(0) &&
      writer.U64(header.content_hash);
  for (const float value : header.position) {
    encoded = encoded && writer.U32(std::bit_cast<std::uint32_t>(value));
  }
  if (!encoded || writer.offset() != kMapEditSpawnHeaderBytes) {
    return false;
  }
  std::copy_n(
      package.begin() + static_cast<std::ptrdiff_t>(header.fragment_offset),
      header.fragment_bytes,
      destination.begin() + static_cast<std::ptrdiff_t>(
                                kEnvelopeBytes + kMapEditSpawnHeaderBytes));
  return true;
}

[[nodiscard]] inline bool
DecodeMapEditSpawnDatagram(std::span<const std::uint8_t> packet,
                           Envelope &envelope_output,
                           MapEditSpawnHeader &header_output,
                           std::span<const std::uint8_t> &fragment_output) {
  Envelope envelope;
  if (!DecodeEnvelope(packet, envelope) ||
      envelope.kind != MessageKind::kMapEditSpawnChunk ||
      envelope.payload_bytes < kMapEditSpawnHeaderBytes) {
    return false;
  }
  detail::LittleEndianReader reader(
      packet.subspan(kEnvelopeBytes, kMapEditSpawnHeaderBytes));
  MapEditSpawnHeader header;
  std::uint8_t type = 0;
  std::uint8_t reserved8 = 0;
  std::uint16_t reserved16 = 0;
  if (!reader.U8(type) || !reader.U8(reserved8) ||
      !reader.U16(header.source_role) || !reader.U64(header.request_id) ||
      !reader.U64(header.authority_revision) ||
      !reader.U64(header.snapshot_id) || !reader.U32(header.total_bytes) ||
      !reader.U32(header.fragment_index) ||
      !reader.U32(header.fragment_count) ||
      !reader.U32(header.fragment_offset) ||
      !reader.U16(header.fragment_bytes) || !reader.U16(reserved16) ||
      !reader.U64(header.content_hash)) {
    return false;
  }
  for (float &value : header.position) {
    std::uint32_t bits = 0;
    if (!reader.U32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
  }
  header.type = static_cast<MapEditSpawnType>(type);
  if (reserved8 != 0 || reserved16 != 0 ||
      reader.offset() != kMapEditSpawnHeaderBytes ||
      !MapEditSpawnHeaderShapeValid(header) ||
      !MapEditSpawnEnvelopeShapeValid(envelope, header)) {
    return false;
  }
  envelope_output = envelope;
  header_output = header;
  fragment_output = packet.subspan(kEnvelopeBytes + kMapEditSpawnHeaderBytes,
                                   header.fragment_bytes);
  return true;
}

enum class MapEditReassemblyDisposition {
  kStored,
  kDuplicate,
  kComplete,
  kConflicting,
  kResourceLimit,
  kInvalid,
};

struct ReassembledMapEditSpawn {
  Envelope envelope;
  MapEditSpawnHeader header;
  std::vector<std::uint8_t> package;
};

struct MapEditReassemblyResult {
  MapEditReassemblyDisposition disposition =
      MapEditReassemblyDisposition::kInvalid;
  std::optional<ReassembledMapEditSpawn> completed;
};

class MapEditSpawnReassembler {
public:
  explicit MapEditSpawnReassembler(
      std::size_t maximum_slots = 4,
      std::size_t maximum_buffered_bytes =
          std::size_t(kMaximumMapEditPackageBytes) * 2,
      std::uint64_t timeout_us = 30000000)
      : maximum_slots_(maximum_slots),
        maximum_buffered_bytes_(maximum_buffered_bytes),
        timeout_us_(timeout_us) {}

  [[nodiscard]] MapEditReassemblyResult
  Push(const Envelope &envelope, const MapEditSpawnHeader &header,
       std::span<const std::uint8_t> fragment, std::uint64_t now_us) {
    Expire(now_us);
    MapEditReassemblyResult result;
    if (!MapEditSpawnEnvelopeShapeValid(envelope, header) ||
        !MapEditSpawnHeaderShapeValid(header) ||
        fragment.size() != header.fragment_bytes) {
      return result;
    }

    auto slot = std::find_if(
        slots_.begin(), slots_.end(),
        [&envelope, &header](const Slot &candidate) {
          return candidate.sender_role == envelope.sender_role &&
                 candidate.sender_session == envelope.sender_session &&
                 candidate.type == header.type &&
                 candidate.request_id == header.request_id &&
                 candidate.authority_revision == header.authority_revision;
        });
    if (slot == slots_.end()) {
      if (maximum_slots_ == 0 || header.total_bytes > maximum_buffered_bytes_ ||
          slots_.size() >= maximum_slots_ ||
          buffered_bytes_ + header.total_bytes > maximum_buffered_bytes_) {
        result.disposition = MapEditReassemblyDisposition::kResourceLimit;
        return result;
      }
      Slot created;
      created.envelope = envelope;
      created.sender_role = envelope.sender_role;
      created.sender_session = envelope.sender_session;
      created.type = header.type;
      created.source_role = header.source_role;
      created.request_id = header.request_id;
      created.authority_revision = header.authority_revision;
      created.snapshot_id = header.snapshot_id;
      created.total_bytes = header.total_bytes;
      created.fragment_count = header.fragment_count;
      created.content_hash = header.content_hash;
      std::copy_n(header.position, 3, created.position);
      created.last_update_us = now_us;
      created.bytes.resize(header.total_bytes);
      created.received.resize(header.fragment_count);
      buffered_bytes_ += header.total_bytes;
      slots_.push_back(std::move(created));
      slot = std::prev(slots_.end());
    }

    if (!MetadataMatches(*slot, header)) {
      Remove(slot);
      result.disposition = MapEditReassemblyDisposition::kConflicting;
      return result;
    }
    if (slot->received[header.fragment_index]) {
      const auto existing =
          std::span<const std::uint8_t>(slot->bytes)
              .subspan(header.fragment_offset, header.fragment_bytes);
      result.disposition =
          std::equal(fragment.begin(), fragment.end(), existing.begin())
              ? MapEditReassemblyDisposition::kDuplicate
              : MapEditReassemblyDisposition::kConflicting;
      if (result.disposition == MapEditReassemblyDisposition::kConflicting) {
        Remove(slot);
      }
      return result;
    }

    std::copy(fragment.begin(), fragment.end(),
              slot->bytes.begin() +
                  static_cast<std::ptrdiff_t>(header.fragment_offset));
    slot->received[header.fragment_index] = true;
    ++slot->received_count;
    slot->last_update_us = now_us;
    if (slot->received_count != slot->fragment_count) {
      result.disposition = MapEditReassemblyDisposition::kStored;
      return result;
    }
    if (MapEditContentHash(slot->bytes) != slot->content_hash) {
      Remove(slot);
      result.disposition = MapEditReassemblyDisposition::kConflicting;
      return result;
    }

    ReassembledMapEditSpawn completed;
    completed.envelope = slot->envelope;
    completed.header = header;
    completed.header.fragment_index = 0;
    completed.header.fragment_offset = 0;
    completed.header.fragment_bytes = 0;
    completed.package = std::move(slot->bytes);
    buffered_bytes_ -= slot->total_bytes;
    slots_.erase(slot);
    result.disposition = MapEditReassemblyDisposition::kComplete;
    result.completed = std::move(completed);
    return result;
  }

  void Reset() {
    slots_.clear();
    buffered_bytes_ = 0;
  }

  [[nodiscard]] std::size_t buffered_bytes() const { return buffered_bytes_; }

private:
  struct Slot {
    Envelope envelope;
    std::uint16_t sender_role = 0;
    std::uint32_t sender_session = 0;
    MapEditSpawnType type = MapEditSpawnType::kSpawnRequest;
    std::uint16_t source_role = 0;
    std::uint64_t request_id = 0;
    std::uint64_t authority_revision = 0;
    std::uint64_t snapshot_id = 0;
    std::uint32_t total_bytes = 0;
    std::uint32_t fragment_count = 0;
    std::uint64_t content_hash = 0;
    float position[3] = {};
    std::uint64_t last_update_us = 0;
    std::uint32_t received_count = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<bool> received;
  };

  [[nodiscard]] static bool MetadataMatches(const Slot &slot,
                                            const MapEditSpawnHeader &header) {
    return slot.type == header.type && slot.source_role == header.source_role &&
           slot.request_id == header.request_id &&
           slot.authority_revision == header.authority_revision &&
           slot.snapshot_id == header.snapshot_id &&
           slot.total_bytes == header.total_bytes &&
           slot.fragment_count == header.fragment_count &&
           slot.content_hash == header.content_hash &&
           std::equal(std::begin(slot.position), std::end(slot.position),
                      std::begin(header.position));
  }

  void Expire(std::uint64_t now_us) {
    for (auto slot = slots_.begin(); slot != slots_.end();) {
      if (now_us >= slot->last_update_us &&
          now_us - slot->last_update_us >= timeout_us_) {
        buffered_bytes_ -= slot->total_bytes;
        slot = slots_.erase(slot);
      } else {
        ++slot;
      }
    }
  }

  void Remove(std::vector<Slot>::iterator slot) {
    buffered_bytes_ -= slot->total_bytes;
    slots_.erase(slot);
  }

  std::size_t maximum_slots_ = 0;
  std::size_t maximum_buffered_bytes_ = 0;
  std::uint64_t timeout_us_ = 0;
  std::size_t buffered_bytes_ = 0;
  std::vector<Slot> slots_;
};

static_assert(kMapEditControlPayloadBytes == 80);
static_assert(kMapEditSpawnHeaderBytes == 68);
static_assert(kMapEditSpawnFragmentBytes == 1092);

} // namespace skate3::multiplayer::protocol_v12
