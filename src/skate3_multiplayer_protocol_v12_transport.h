#pragma once

#include "skate3_multiplayer_protocol_v12_pose.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

struct PoseGroupPacketizeRequest {
  Envelope envelope;
  std::uint32_t pose_id = 0;
  std::uint32_t baseline_id = 0;
  std::uint16_t element_count = 0;
  std::uint8_t group_id = 0;
  PoseGroupEncoding encoding = PoseGroupEncoding::kV11WordStream;
  std::span<const std::uint8_t> group_bytes;
};

struct PoseGroupDatagram {
  Envelope envelope;
  PoseGroupHeader header;
};

// Builds descriptors only. The encoded group remains caller-owned and is
// copied directly into a transport datagram by EncodePoseGroupDatagram.
[[nodiscard]] inline std::size_t BuildPoseGroupDatagrams(
    const PoseGroupPacketizeRequest& request,
    std::span<PoseGroupDatagram> output) {
  if (request.group_bytes.empty() ||
      request.group_bytes.size() > kMaximumPoseGroupBytes ||
      (request.envelope.kind != MessageKind::kPoseBaseline &&
       request.envelope.kind != MessageKind::kPoseDelta)) {
    return 0;
  }
  const std::size_t fragment_count =
      PoseGroupFragmentCount(
          static_cast<std::uint32_t>(request.group_bytes.size()));
  if (fragment_count == 0 || fragment_count > output.size()) {
    return 0;
  }

  PoseGroupHeader first_header;
  first_header.pose_id = request.pose_id;
  first_header.baseline_id = request.baseline_id;
  first_header.total_bytes =
      static_cast<std::uint32_t>(request.group_bytes.size());
  first_header.element_count = request.element_count;
  first_header.fragment_bytes = static_cast<std::uint16_t>(
      PoseGroupFragmentByteCount(first_header.total_bytes, 0));
  first_header.fragment_count =
      static_cast<std::uint8_t>(fragment_count);
  first_header.group_id = request.group_id;
  first_header.encoding = request.encoding;

  Envelope first_envelope = request.envelope;
  first_envelope.flags =
      first_envelope.kind == MessageKind::kPoseBaseline
          ? kFlagKeyframe | kFlagExpires
          : kFlagExpires;
  first_envelope.payload_bytes =
      kPoseGroupHeaderBytes + first_header.fragment_bytes;
  if (!PoseGroupEnvelopeShapeValid(
          first_envelope, first_header)) {
    return 0;
  }

  for (std::size_t index = 0; index < fragment_count; ++index) {
    PoseGroupHeader header = first_header;
    header.fragment_index = static_cast<std::uint8_t>(index);
    header.fragment_offset = static_cast<std::uint32_t>(
        PoseGroupFragmentOffset(header.fragment_index));
    header.fragment_bytes = static_cast<std::uint16_t>(
        PoseGroupFragmentByteCount(
            header.total_bytes, header.fragment_index));

    Envelope envelope = first_envelope;
    envelope.sequence += static_cast<std::uint32_t>(index);
    envelope.payload_bytes =
        kPoseGroupHeaderBytes + header.fragment_bytes;
    output[index] = {envelope, header};
  }
  return fragment_count;
}

[[nodiscard]] inline bool EncodePoseGroupDatagram(
    const PoseGroupDatagram& descriptor,
    std::span<const std::uint8_t> group_bytes,
    std::span<std::uint8_t> destination) {
  const std::size_t expected_packet_bytes =
      std::size_t(kEnvelopeBytes) +
      descriptor.envelope.payload_bytes;
  if (!PoseGroupEnvelopeShapeValid(
          descriptor.envelope, descriptor.header) ||
      group_bytes.size() != descriptor.header.total_bytes ||
      destination.size() != expected_packet_bytes ||
      !EncodeEnvelope(descriptor.envelope, destination) ||
      !EncodePoseGroupHeader(
          descriptor.header,
          descriptor.envelope.kind,
          destination.subspan(kEnvelopeBytes))) {
    return false;
  }

  const std::size_t source_offset =
      descriptor.header.fragment_offset;
  std::memcpy(
      destination.data() + kEnvelopeBytes + kPoseGroupHeaderBytes,
      group_bytes.data() + source_offset,
      descriptor.header.fragment_bytes);
  return true;
}

[[nodiscard]] inline bool DecodePoseGroupDatagram(
    std::span<const std::uint8_t> packet,
    Envelope& envelope_output,
    PoseGroupHeader& header_output,
    std::span<const std::uint8_t>& fragment_output) {
  Envelope envelope;
  if (!DecodeEnvelope(packet, envelope) ||
      (envelope.kind != MessageKind::kPoseBaseline &&
       envelope.kind != MessageKind::kPoseDelta)) {
    return false;
  }
  const std::span<const std::uint8_t> payload =
      packet.subspan(kEnvelopeBytes);
  PoseGroupHeader header;
  if (!DecodePoseGroupHeader(payload, envelope.kind, header) ||
      !PoseGroupEnvelopeShapeValid(envelope, header)) {
    return false;
  }
  envelope_output = envelope;
  header_output = header;
  fragment_output = payload.subspan(kPoseGroupHeaderBytes);
  return true;
}

enum class ReassemblyDisposition {
  kFragmentStored,
  kDuplicateFragment,
  kGroupComplete,
  kStalePose,
  kConflictingFragment,
  kResourceLimit,
  kInvalidFragment,
};

struct ReassembledPoseGroup {
  std::uint16_t sender_role = 0;
  std::uint16_t stream_id = 0;
  std::uint32_t sender_session = 0;
  MessageKind kind = MessageKind::kPoseBaseline;
  std::uint32_t pose_id = 0;
  std::uint32_t baseline_id = 0;
  std::uint16_t element_count = 0;
  std::uint8_t group_id = 0;
  PoseGroupEncoding encoding = PoseGroupEncoding::kV11WordStream;
  std::uint32_t completed_at_sequence = 0;
  std::vector<std::uint8_t> bytes;
};

struct ReassemblyExpiry {
  std::size_t expired_slots = 0;
  std::uint32_t baseline_recovery_mask = 0;
};

struct ReassemblyPushResult {
  ReassemblyDisposition disposition =
      ReassemblyDisposition::kInvalidFragment;
  std::size_t evicted_slots = 0;
  std::uint32_t baseline_recovery_mask = 0;
  std::optional<ReassembledPoseGroup> completed;
};

class PoseGroupReassembler {
 public:
  explicit PoseGroupReassembler(
      std::size_t maximum_slots = 8,
      std::size_t maximum_buffered_bytes = 256 * 1024,
      std::uint64_t fragment_timeout_us = 250000)
      : maximum_slots_(maximum_slots),
        maximum_buffered_bytes_(maximum_buffered_bytes),
        fragment_timeout_us_(fragment_timeout_us) {}

  [[nodiscard]] ReassemblyExpiry Expire(std::uint64_t now_us) {
    ReassemblyExpiry result;
    for (std::size_t index = slots_.size(); index > 0; --index) {
      const Slot& slot = slots_[index - 1];
      if (now_us < slot.last_update_us ||
          now_us - slot.last_update_us < fragment_timeout_us_) {
        continue;
      }
      ++result.expired_slots;
      if (slot.kind == MessageKind::kPoseBaseline) {
        result.baseline_recovery_mask |=
            1u << slot.group_id;
      }
      RemoveSlot(index - 1);
    }
    return result;
  }

  [[nodiscard]] ReassemblyPushResult Push(
      const Envelope& envelope,
      const PoseGroupHeader& header,
      std::span<const std::uint8_t> fragment,
      std::uint64_t now_us) {
    ReassemblyPushResult result;
    const ReassemblyExpiry expiry = Expire(now_us);
    result.evicted_slots = expiry.expired_slots;
    result.baseline_recovery_mask =
        expiry.baseline_recovery_mask;

    if (!PoseGroupEnvelopeShapeValid(envelope, header) ||
        fragment.size() != header.fragment_bytes) {
      return result;
    }

    std::optional<std::size_t> matching_slot;
    for (std::size_t index = slots_.size(); index > 0; --index) {
      Slot& slot = slots_[index - 1];
      if (!SameChannel(slot, envelope, header.group_id) ||
          slot.kind != envelope.kind) {
        continue;
      }
      if (slot.pose_id == header.pose_id) {
        if (!MetadataMatches(slot, envelope, header)) {
          result.disposition =
              ReassemblyDisposition::kConflictingFragment;
          if (slot.kind == MessageKind::kPoseBaseline) {
            result.baseline_recovery_mask |=
                1u << slot.group_id;
          }
          RemoveSlot(index - 1);
          return result;
        }
        matching_slot = index - 1;
        break;
      }
      if (SequenceNewer(slot.pose_id, header.pose_id)) {
        result.disposition = ReassemblyDisposition::kStalePose;
        return result;
      }
      if (!SequenceNewer(header.pose_id, slot.pose_id)) {
        result.disposition = ReassemblyDisposition::kStalePose;
        return result;
      }
      RemoveSlot(index - 1);
      ++result.evicted_slots;
    }

    if (!matching_slot.has_value()) {
      if (maximum_slots_ == 0 ||
          header.total_bytes > maximum_buffered_bytes_) {
        result.disposition =
            ReassemblyDisposition::kResourceLimit;
        if (envelope.kind == MessageKind::kPoseBaseline) {
          result.baseline_recovery_mask |=
              1u << header.group_id;
        }
        return result;
      }
      while (!slots_.empty() &&
             (slots_.size() >= maximum_slots_ ||
              buffered_bytes_ + header.total_bytes >
                  maximum_buffered_bytes_)) {
        const std::size_t oldest = OldestSlotIndex();
        if (slots_[oldest].kind ==
            MessageKind::kPoseBaseline) {
          result.baseline_recovery_mask |=
              1u << slots_[oldest].group_id;
        }
        RemoveSlot(oldest);
        ++result.evicted_slots;
      }
      if (slots_.size() >= maximum_slots_ ||
          buffered_bytes_ + header.total_bytes >
              maximum_buffered_bytes_) {
        result.disposition =
            ReassemblyDisposition::kResourceLimit;
        return result;
      }

      Slot slot;
      slot.sender_role = envelope.sender_role;
      slot.stream_id = envelope.stream_id;
      slot.sender_session = envelope.sender_session;
      slot.kind = envelope.kind;
      slot.pose_id = header.pose_id;
      slot.baseline_id = header.baseline_id;
      slot.total_bytes = header.total_bytes;
      slot.element_count = header.element_count;
      slot.fragment_count = header.fragment_count;
      slot.group_id = header.group_id;
      slot.encoding = header.encoding;
      slot.sender_time_us = envelope.sender_time_us;
      slot.base_sequence =
          envelope.sequence - header.fragment_index;
      slot.last_update_us = now_us;
      slot.bytes.resize(header.total_bytes);
      buffered_bytes_ += header.total_bytes;
      slots_.push_back(std::move(slot));
      matching_slot = slots_.size() - 1;
    }

    Slot& slot = slots_[*matching_slot];
    const std::uint64_t fragment_bit =
        1ull << header.fragment_index;
    const std::size_t offset = header.fragment_offset;
    if ((slot.received_fragments & fragment_bit) != 0) {
      if (std::equal(
              fragment.begin(), fragment.end(),
              slot.bytes.begin() +
                  static_cast<std::ptrdiff_t>(offset))) {
        result.disposition =
            ReassemblyDisposition::kDuplicateFragment;
        return result;
      }
      result.disposition =
          ReassemblyDisposition::kConflictingFragment;
      if (slot.kind == MessageKind::kPoseBaseline) {
        result.baseline_recovery_mask |=
            1u << slot.group_id;
      }
      RemoveSlot(*matching_slot);
      return result;
    }

    std::copy(
        fragment.begin(), fragment.end(),
        slot.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    slot.received_fragments |= fragment_bit;
    ++slot.received_count;
    slot.last_update_us = now_us;
    if (slot.received_count != slot.fragment_count) {
      result.disposition =
          ReassemblyDisposition::kFragmentStored;
      return result;
    }

    ReassembledPoseGroup completed;
    completed.sender_role = slot.sender_role;
    completed.stream_id = slot.stream_id;
    completed.sender_session = slot.sender_session;
    completed.kind = slot.kind;
    completed.pose_id = slot.pose_id;
    completed.baseline_id = slot.baseline_id;
    completed.element_count = slot.element_count;
    completed.group_id = slot.group_id;
    completed.encoding = slot.encoding;
    completed.completed_at_sequence = envelope.sequence;
    completed.bytes = std::move(slot.bytes);
    buffered_bytes_ -= slot.total_bytes;
    slots_.erase(
        slots_.begin() +
        static_cast<std::ptrdiff_t>(*matching_slot));
    result.disposition =
        ReassemblyDisposition::kGroupComplete;
    result.completed = std::move(completed);
    return result;
  }

  void Reset() {
    slots_.clear();
    buffered_bytes_ = 0;
  }

  [[nodiscard]] std::size_t active_slots() const {
    return slots_.size();
  }
  [[nodiscard]] std::size_t buffered_bytes() const {
    return buffered_bytes_;
  }

 private:
  struct Slot {
    std::uint16_t sender_role = 0;
    std::uint16_t stream_id = 0;
    std::uint32_t sender_session = 0;
    MessageKind kind = MessageKind::kPoseBaseline;
    std::uint32_t pose_id = 0;
    std::uint32_t baseline_id = 0;
    std::uint32_t total_bytes = 0;
    std::uint16_t element_count = 0;
    std::uint8_t fragment_count = 0;
    std::uint8_t group_id = 0;
    PoseGroupEncoding encoding =
        PoseGroupEncoding::kV11WordStream;
    std::uint64_t sender_time_us = 0;
    std::uint32_t base_sequence = 0;
    std::uint64_t last_update_us = 0;
    std::uint64_t received_fragments = 0;
    std::uint8_t received_count = 0;
    std::vector<std::uint8_t> bytes;
  };

  [[nodiscard]] static bool SameChannel(
      const Slot& slot,
      const Envelope& envelope,
      std::uint8_t group_id) {
    return slot.sender_role == envelope.sender_role &&
           slot.stream_id == envelope.stream_id &&
           slot.sender_session == envelope.sender_session &&
           slot.group_id == group_id;
  }

  [[nodiscard]] static bool MetadataMatches(
      const Slot& slot,
      const Envelope& envelope,
      const PoseGroupHeader& header) {
    return SameChannel(slot, envelope, header.group_id) &&
           slot.kind == envelope.kind &&
           slot.pose_id == header.pose_id &&
           slot.baseline_id == header.baseline_id &&
           slot.total_bytes == header.total_bytes &&
           slot.element_count == header.element_count &&
           slot.fragment_count == header.fragment_count &&
           slot.encoding == header.encoding &&
           slot.sender_time_us == envelope.sender_time_us &&
           slot.base_sequence ==
               envelope.sequence - header.fragment_index;
  }

  [[nodiscard]] std::size_t OldestSlotIndex() const {
    std::size_t oldest = 0;
    for (std::size_t index = 1; index < slots_.size(); ++index) {
      if (slots_[index].last_update_us <
          slots_[oldest].last_update_us) {
        oldest = index;
      }
    }
    return oldest;
  }

  void RemoveSlot(std::size_t index) {
    buffered_bytes_ -= slots_[index].total_bytes;
    slots_.erase(
        slots_.begin() + static_cast<std::ptrdiff_t>(index));
  }

  std::size_t maximum_slots_ = 0;
  std::size_t maximum_buffered_bytes_ = 0;
  std::uint64_t fragment_timeout_us_ = 0;
  std::size_t buffered_bytes_ = 0;
  std::vector<Slot> slots_;
};

}  // namespace skate3::multiplayer::protocol_v12
