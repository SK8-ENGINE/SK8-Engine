#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace skate3::multiplayer::protocol_v12 {

enum class ReceiveDisposition {
  kNewLatest,
  kReordered,
  kDuplicate,
  kTooOld,
};

class ReceiveHistory {
 public:
  [[nodiscard]] ReceiveDisposition Observe(std::uint32_t sequence) {
    if (!initialized_) {
      initialized_ = true;
      latest_ = sequence;
      history_ = 0;
      return ReceiveDisposition::kNewLatest;
    }
    if (sequence == latest_) {
      return ReceiveDisposition::kDuplicate;
    }
    if (SequenceNewer(sequence, latest_)) {
      const std::uint32_t advance = sequence - latest_;
      if (advance > 32) {
        history_ = 0;
      } else if (advance == 32) {
        history_ = 1u << 31;
      } else {
        history_ =
            (history_ << advance) | (1u << (advance - 1));
      }
      latest_ = sequence;
      return ReceiveDisposition::kNewLatest;
    }

    const std::uint32_t distance = latest_ - sequence;
    if (distance == 0 || distance > 32) {
      return ReceiveDisposition::kTooOld;
    }
    const std::uint32_t bit = 1u << (distance - 1);
    if ((history_ & bit) != 0) {
      return ReceiveDisposition::kDuplicate;
    }
    history_ |= bit;
    return ReceiveDisposition::kReordered;
  }

  void Clear() {
    initialized_ = false;
    latest_ = 0;
    history_ = 0;
  }

  [[nodiscard]] bool initialized() const { return initialized_; }
  [[nodiscard]] std::uint32_t latest() const { return latest_; }
  [[nodiscard]] std::uint32_t history() const { return history_; }

 private:
  bool initialized_ = false;
  std::uint32_t latest_ = 0;
  std::uint32_t history_ = 0;
};

struct GenerationIdentity {
  std::uint16_t role = 0;
  std::uint32_t session = 0;
  std::uint64_t map_hash = 0;
  std::uint64_t build_hash = 0;
  std::uint64_t content_hash = 0;

  [[nodiscard]] constexpr bool operator==(
      const GenerationIdentity&) const = default;
};

struct ContentIdentity {
  std::uint64_t map_hash = 0;
  std::uint64_t build_hash = 0;
  std::uint64_t content_hash = 0;
};

enum class GenerationActivation {
  kFirst,
  kUnchanged,
  kReplaced,
  kStale,
  kIncompatible,
  kInvalid,
};

class PeerGenerationState {
 public:
  // transport_generation is assigned by the authenticated transport or
  // capability-handshake owner. Realtime pose packets must never advance it.
  // That external monotonic identity prevents a delayed capability datagram
  // from rolling this state back to an old random session identifier.
  [[nodiscard]] GenerationActivation ActivateValidated(
      const GenerationIdentity& candidate,
      const ContentIdentity& expected_content,
      std::uint64_t transport_generation) {
    if (candidate.role < 1 || candidate.role > 100 ||
        candidate.session == 0 || transport_generation == 0) {
      return GenerationActivation::kInvalid;
    }
    if (candidate.map_hash != expected_content.map_hash ||
        candidate.build_hash != expected_content.build_hash ||
        candidate.content_hash != expected_content.content_hash) {
      return GenerationActivation::kIncompatible;
    }
    if (active_) {
      if (transport_generation < transport_generation_) {
        return GenerationActivation::kStale;
      }
      if (transport_generation == transport_generation_) {
        return candidate == identity_
                   ? GenerationActivation::kUnchanged
                   : GenerationActivation::kStale;
      }
    }

    const bool replacing = active_;
    active_ = true;
    identity_ = candidate;
    transport_generation_ = transport_generation;
    return replacing ? GenerationActivation::kReplaced
                     : GenerationActivation::kFirst;
  }

  void Clear() {
    active_ = false;
    identity_ = {};
    transport_generation_ = 0;
  }

  [[nodiscard]] bool Matches(
      std::uint16_t role, std::uint32_t session) const {
    return active_ && identity_.role == role &&
           identity_.session == session;
  }

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] const GenerationIdentity& identity() const {
    return identity_;
  }
  [[nodiscard]] std::uint64_t transport_generation() const {
    return transport_generation_;
  }

 private:
  bool active_ = false;
  GenerationIdentity identity_{};
  std::uint64_t transport_generation_ = 0;
};

enum class PoseReceiveResult {
  kFragmentAccepted,
  kBaselineGroupAccepted,
  kBaselineCompleted,
  kDeltaAccepted,
  kDuplicatePacket,
  kPacketTooOld,
  kStaleGeneration,
  kStaleBaseline,
  kMissingBaseline,
  kInvalidGroups,
  kInvalidBaseline,
};

class PoseReceiverState {
 public:
  explicit PoseReceiverState(std::uint32_t required_group_mask)
      : required_group_mask_(required_group_mask) {}

  void ActivateGeneration(
      std::uint16_t role, std::uint32_t session) {
    role_ = role;
    session_ = session;
    active_ = role >= 1 && role <= 100 && session != 0;
    packet_history_.Clear();
    decoded_baseline_id_ = 0;
    decoded_baseline_sequence_ = 0;
    pending_baseline_id_ = 0;
    pending_group_mask_ = 0;
    request_pending_ = false;
    recovery_active_ = false;
  }

  [[nodiscard]] PoseReceiveResult ReceiveBaselineGroup(
      std::uint16_t role,
      std::uint32_t session,
      std::uint32_t packet_sequence,
      std::uint32_t baseline_id,
      std::uint32_t group_mask) {
    if (!Matches(role, session)) {
      return PoseReceiveResult::kStaleGeneration;
    }
    if (baseline_id == 0) {
      return PoseReceiveResult::kInvalidBaseline;
    }
    if (!GroupsValid(group_mask)) {
      return PoseReceiveResult::kInvalidGroups;
    }

    const PoseReceiveResult packet =
        ReceivePoseFragment(role, session, packet_sequence);
    if (packet != PoseReceiveResult::kFragmentAccepted) {
      return packet;
    }
    return CompleteBaselineGroup(
        role, session, packet_sequence, baseline_id, group_mask);
  }

  // Call this after each structurally valid datagram and before adding its
  // bytes to a fragment reassembler. CompleteBaselineGroup or CompleteDelta
  // is called separately only after the full group payload decodes.
  [[nodiscard]] PoseReceiveResult ReceivePoseFragment(
      std::uint16_t role,
      std::uint32_t session,
      std::uint32_t packet_sequence) {
    if (!Matches(role, session)) {
      return PoseReceiveResult::kStaleGeneration;
    }
    const ReceiveDisposition packet =
        packet_history_.Observe(packet_sequence);
    if (packet == ReceiveDisposition::kDuplicate) {
      return PoseReceiveResult::kDuplicatePacket;
    }
    if (packet == ReceiveDisposition::kTooOld) {
      return PoseReceiveResult::kPacketTooOld;
    }
    return PoseReceiveResult::kFragmentAccepted;
  }

  [[nodiscard]] PoseReceiveResult CompleteBaselineGroup(
      std::uint16_t role,
      std::uint32_t session,
      std::uint32_t completed_at_sequence,
      std::uint32_t baseline_id,
      std::uint32_t group_mask) {
    if (!Matches(role, session)) {
      return PoseReceiveResult::kStaleGeneration;
    }
    if (baseline_id == 0) {
      return PoseReceiveResult::kInvalidBaseline;
    }
    if (!GroupsValid(group_mask)) {
      return PoseReceiveResult::kInvalidGroups;
    }
    if (baseline_id == decoded_baseline_id_ ||
        (decoded_baseline_id_ != 0 &&
         !SequenceNewer(baseline_id, decoded_baseline_id_))) {
      return PoseReceiveResult::kStaleBaseline;
    }
    if (pending_baseline_id_ == 0 ||
        SequenceNewer(baseline_id, pending_baseline_id_)) {
      pending_baseline_id_ = baseline_id;
      pending_group_mask_ = 0;
    } else if (baseline_id != pending_baseline_id_) {
      return PoseReceiveResult::kStaleBaseline;
    }

    pending_group_mask_ |= group_mask;
    if ((pending_group_mask_ & required_group_mask_) !=
        required_group_mask_) {
      return PoseReceiveResult::kBaselineGroupAccepted;
    }

    decoded_baseline_id_ = pending_baseline_id_;
    decoded_baseline_sequence_ = completed_at_sequence;
    pending_baseline_id_ = 0;
    pending_group_mask_ = 0;
    request_pending_ = false;
    recovery_active_ = false;
    return PoseReceiveResult::kBaselineCompleted;
  }

  [[nodiscard]] PoseReceiveResult ReceiveDelta(
      std::uint16_t role,
      std::uint32_t session,
      std::uint32_t packet_sequence,
      std::uint32_t referenced_baseline_id,
      std::uint32_t group_mask) {
    if (!Matches(role, session)) {
      return PoseReceiveResult::kStaleGeneration;
    }
    if (referenced_baseline_id == 0) {
      return PoseReceiveResult::kInvalidBaseline;
    }
    if (!GroupsValid(group_mask)) {
      return PoseReceiveResult::kInvalidGroups;
    }

    const PoseReceiveResult packet =
        ReceivePoseFragment(role, session, packet_sequence);
    if (packet != PoseReceiveResult::kFragmentAccepted) {
      return packet;
    }
    return CompleteDelta(
        role, session, referenced_baseline_id, group_mask);
  }

  [[nodiscard]] PoseReceiveResult CompleteDelta(
      std::uint16_t role,
      std::uint32_t session,
      std::uint32_t referenced_baseline_id,
      std::uint32_t group_mask) {
    if (!Matches(role, session)) {
      return PoseReceiveResult::kStaleGeneration;
    }
    if (referenced_baseline_id == 0) {
      return PoseReceiveResult::kInvalidBaseline;
    }
    if (!GroupsValid(group_mask)) {
      return PoseReceiveResult::kInvalidGroups;
    }
    if (referenced_baseline_id != decoded_baseline_id_) {
      RequestBaseline();
      return PoseReceiveResult::kMissingBaseline;
    }
    return PoseReceiveResult::kDeltaAccepted;
  }

  [[nodiscard]] bool ConsumeBaselineRequest() {
    if (!request_pending_) {
      return false;
    }
    request_pending_ = false;
    recovery_active_ = true;
    return true;
  }

  void ScheduleBaselineRetry() {
    if (recovery_active_) {
      request_pending_ = true;
    }
  }

  void NotifyBaselineUnavailable() {
    RequestBaseline();
  }

  [[nodiscard]] bool baseline_request_pending() const {
    return request_pending_;
  }
  [[nodiscard]] bool baseline_recovery_active() const {
    return recovery_active_;
  }
  [[nodiscard]] std::uint32_t decoded_baseline_id() const {
    return decoded_baseline_id_;
  }
  [[nodiscard]] std::uint32_t decoded_baseline_sequence() const {
    return decoded_baseline_sequence_;
  }
  [[nodiscard]] std::uint32_t pending_group_mask() const {
    return pending_group_mask_;
  }
  [[nodiscard]] const ReceiveHistory& packet_history() const {
    return packet_history_;
  }

 private:
  [[nodiscard]] bool Matches(
      std::uint16_t role, std::uint32_t session) const {
    return active_ && role_ == role && session_ == session;
  }

  [[nodiscard]] bool GroupsValid(std::uint32_t group_mask) const {
    return required_group_mask_ != 0 && group_mask != 0 &&
           (group_mask & ~required_group_mask_) == 0;
  }

  void RequestBaseline() {
    if (!request_pending_ && !recovery_active_) {
      request_pending_ = true;
    }
  }

  std::uint32_t required_group_mask_ = 0;
  bool active_ = false;
  std::uint16_t role_ = 0;
  std::uint32_t session_ = 0;
  ReceiveHistory packet_history_;
  std::uint32_t decoded_baseline_id_ = 0;
  std::uint32_t decoded_baseline_sequence_ = 0;
  std::uint32_t pending_baseline_id_ = 0;
  std::uint32_t pending_group_mask_ = 0;
  bool request_pending_ = false;
  bool recovery_active_ = false;
};

class SenderBaselineState {
 public:
  static constexpr std::size_t kMaximumRetainedOffers = 32;

  void ActivateGeneration(std::uint32_t sender_session) {
    sender_session_ = sender_session;
    offered_baseline_id_ = 0;
    offered_baseline_sequence_ = 0;
    confirmed_baseline_id_ = 0;
    offers_ = {};
    next_offer_ = 0;
    retained_offers_ = 0;
  }

  [[nodiscard]] bool OfferBaseline(
      std::uint32_t sender_session,
      std::uint32_t baseline_id,
      std::uint32_t packet_sequence) {
    if (sender_session == 0 || sender_session != sender_session_ ||
        baseline_id == 0) {
      return false;
    }
    if (offered_baseline_id_ != 0 &&
        baseline_id != offered_baseline_id_ &&
        !SequenceNewer(baseline_id, offered_baseline_id_)) {
      return false;
    }
    if (baseline_id == offered_baseline_id_) {
      return packet_sequence == offered_baseline_sequence_;
    }
    offered_baseline_id_ = baseline_id;
    offered_baseline_sequence_ = packet_sequence;
    offers_[next_offer_] = {
        .baseline_id = baseline_id,
        .packet_sequence = packet_sequence,
    };
    next_offer_ =
        (next_offer_ + 1) % kMaximumRetainedOffers;
    retained_offers_ =
        std::min(
            retained_offers_ + 1, kMaximumRetainedOffers);
    return true;
  }

  // Packet acknowledgement is intentionally insufficient here. The receiver
  // reports this identifier only after every required pose group has decoded.
  [[nodiscard]] bool ConfirmDecodedBaseline(
      std::uint32_t sender_session,
      std::uint32_t decoded_baseline_id) {
    if (sender_session == 0 || sender_session != sender_session_ ||
        decoded_baseline_id == 0) {
      return false;
    }
    if (decoded_baseline_id == confirmed_baseline_id_) {
      return true;
    }
    bool offered = false;
    for (std::size_t index = 0;
         index < retained_offers_; ++index) {
      if (offers_[index].baseline_id == decoded_baseline_id) {
        offered = true;
        break;
      }
    }
    if (!offered) {
      return false;
    }
    // A reliable report may arrive after a newer offered baseline has
    // already been decoded and confirmed. It is still authentic, but it
    // must never roll the sender's usable baseline backward.
    if (confirmed_baseline_id_ != 0 &&
        !SequenceNewer(
            decoded_baseline_id, confirmed_baseline_id_)) {
      return true;
    }
    confirmed_baseline_id_ = decoded_baseline_id;
    return true;
  }

  [[nodiscard]] std::uint32_t offered_baseline_id() const {
    return offered_baseline_id_;
  }
  [[nodiscard]] std::uint32_t offered_baseline_sequence() const {
    return offered_baseline_sequence_;
  }
  [[nodiscard]] std::uint32_t confirmed_baseline_id() const {
    return confirmed_baseline_id_;
  }

 private:
  struct Offer {
    std::uint32_t baseline_id = 0;
    std::uint32_t packet_sequence = 0;
  };

  std::uint32_t sender_session_ = 0;
  std::uint32_t offered_baseline_id_ = 0;
  std::uint32_t offered_baseline_sequence_ = 0;
  std::uint32_t confirmed_baseline_id_ = 0;
  std::array<Offer, kMaximumRetainedOffers> offers_{};
  std::size_t next_offer_ = 0;
  std::size_t retained_offers_ = 0;
};

}  // namespace skate3::multiplayer::protocol_v12
