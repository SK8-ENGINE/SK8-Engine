#include "skate3_multiplayer_protocol_v12_state.h"
#include "skate3_multiplayer_protocol_v12_animation.h"
#include "skate3_multiplayer_protocol_v12_transport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace skate3::multiplayer::protocol_v12;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

std::vector<std::uint8_t> Bytes(std::size_t count, std::uint8_t seed) {
  std::vector<std::uint8_t> result(count);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] =
        static_cast<std::uint8_t>(seed + index * 17u);
  }
  return result;
}

PoseGroupPacketizeRequest Request(
    MessageKind kind,
    std::span<const std::uint8_t> bytes,
    std::uint32_t pose_id,
    std::uint32_t baseline_id,
    std::uint8_t group_id = 0) {
  PoseGroupPacketizeRequest request;
  request.envelope.kind = kind;
  request.envelope.sender_role = 2;
  request.envelope.sender_session = 100;
  request.envelope.stream_id = 3;
  request.envelope.sequence = 500;
  request.envelope.sender_time_us = 1000000;
  request.pose_id = pose_id;
  request.baseline_id = baseline_id;
  request.element_count = 131;
  request.group_id = group_id;
  request.encoding = PoseGroupEncoding::kV11WordStream;
  request.group_bytes = bytes;
  return request;
}

std::vector<std::uint8_t> Encode(
    const PoseGroupDatagram& descriptor,
    std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> packet(
      kEnvelopeBytes + descriptor.envelope.payload_bytes);
  Expect(EncodePoseGroupDatagram(descriptor, bytes, packet),
         "test pose datagram did not encode");
  return packet;
}

void TestPacketizerBudgetAndRoundTrip() {
  const std::vector<std::uint8_t> source = Bytes(2500, 9);
  std::array<PoseGroupDatagram, 8> descriptors{};
  const std::size_t count = BuildPoseGroupDatagrams(
      Request(MessageKind::kPoseBaseline, source, 40, 0, 2),
      descriptors);
  Expect(count == 3, "2500-byte pose group did not make 3 packets");
  Expect(descriptors[0].envelope.sequence == 500 &&
             descriptors[1].envelope.sequence == 501 &&
             descriptors[2].envelope.sequence == 502,
         "packetizer sequence range changed");
  Expect(descriptors[0].header.fragment_bytes == 1136 &&
             descriptors[1].header.fragment_bytes == 1136 &&
             descriptors[2].header.fragment_bytes == 228,
         "packetizer fragment sizing changed");

  for (std::size_t index = 0; index < count; ++index) {
    const std::vector<std::uint8_t> packet =
        Encode(descriptors[index], source);
    Expect(packet.size() <= kMaximumDatagramBytes,
           "packetizer exceeded datagram ceiling");
    if (index < 2) {
      Expect(packet.size() == kMaximumDatagramBytes,
             "full pose fragment did not fill datagram budget");
    }
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(
               packet, envelope, header, fragment),
           "encoded pose datagram did not decode");
    Expect(envelope.sequence == 500 + index &&
               header.fragment_index == index &&
               fragment.size() == header.fragment_bytes &&
               std::equal(
                   fragment.begin(), fragment.end(),
                   source.begin() + header.fragment_offset),
           "decoded pose datagram changed fragment data");
  }
}

void TestAnimationWordStreamGoldenBytesAndRoundTrip() {
  const float root[3] = {1.0f, -2.5f, 0.25f};
  const std::vector<std::uint16_t> words = {
      1, 2, 0x3344, 0x1122, 0x0000, 0xFFFF, 0xA55A,
  };
  std::vector<std::uint8_t> encoded(
      AnimationWordStreamByteCount(words.size()));
  Expect(EncodeAnimationWordStream(
             root, 0x7788, words, encoded),
         "animation word stream did not encode");
  constexpr std::array<std::uint8_t, 30> golden = {
      0x00, 0x00, 0x80, 0x3F,
      0x00, 0x00, 0x20, 0xC0,
      0x00, 0x00, 0x80, 0x3E,
      0x88, 0x77, 0x07, 0x00,
      0x01, 0x00, 0x02, 0x00,
      0x44, 0x33, 0x22, 0x11,
      0x00, 0x00, 0xFF, 0xFF,
      0x5A, 0xA5,
  };
  Expect(std::equal(
             encoded.begin(), encoded.end(), golden.begin()),
         "animation word stream golden bytes changed");

  float decoded_root[3] = {};
  std::uint16_t decoded_root_bone = 0;
  std::vector<std::uint16_t> decoded_words;
  Expect(DecodeAnimationWordStream(
             encoded, decoded_root, decoded_root_bone,
             decoded_words),
         "animation word stream did not decode");
  Expect(std::bit_cast<std::uint32_t>(decoded_root[0]) ==
             std::bit_cast<std::uint32_t>(root[0]) &&
             std::bit_cast<std::uint32_t>(decoded_root[1]) ==
                 std::bit_cast<std::uint32_t>(root[1]) &&
             std::bit_cast<std::uint32_t>(decoded_root[2]) ==
                 std::bit_cast<std::uint32_t>(root[2]) &&
             decoded_root_bone == 0x7788 &&
             decoded_words == words,
         "animation word stream round trip changed data");
}

void TestAnimationWordStreamMaximumGroupRoundTrip() {
  const float root[3] = {123.5f, -45.25f, 0.0f};
  std::vector<std::uint16_t> words(
      skate3::multiplayer::protocol::kMaximumAnimationFrameWords);
  words[0] = 1;
  words[1] = 32;
  for (std::size_t index = 2; index < words.size(); ++index) {
    words[index] = static_cast<std::uint16_t>(
        index * 40503u + 17u);
  }
  std::vector<std::uint8_t> group(
      AnimationWordStreamByteCount(words.size()));
  Expect(EncodeAnimationWordStream(
             root, 130, words, group),
         "maximum animation word stream did not encode");
  Expect(group.size() == kMaximumAnimationWordStreamBytes,
         "maximum animation word stream size changed");

  std::array<PoseGroupDatagram, 58> descriptors{};
  PoseGroupPacketizeRequest request =
      Request(MessageKind::kPoseBaseline, group, 71, 0, 0);
  request.element_count = words[1];
  const std::size_t count =
      BuildPoseGroupDatagrams(request, descriptors);
  Expect(count == 15,
         "maximum v11 animation bridge made wrong packet count");

  PoseGroupReassembler reassembler;
  ReassemblyPushResult result;
  for (std::size_t reverse = count; reverse > 0; --reverse) {
    const PoseGroupDatagram& descriptor =
        descriptors[reverse - 1];
    const std::vector<std::uint8_t> packet =
        Encode(descriptor, group);
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(
               packet, envelope, header, fragment),
           "maximum animation group fragment did not decode");
    result = reassembler.Push(
        envelope, header, fragment, 1000 + reverse);
  }
  Expect(result.disposition ==
             ReassemblyDisposition::kGroupComplete &&
             result.completed.has_value(),
         "maximum animation group did not reassemble");

  float decoded_root[3] = {};
  std::uint16_t decoded_root_bone = 0;
  std::vector<std::uint16_t> decoded_words;
  Expect(DecodeAnimationWordStream(
             result.completed->bytes, decoded_root,
             decoded_root_bone, decoded_words),
         "reassembled maximum animation stream did not decode");
  Expect(decoded_words == words && decoded_root_bone == 130 &&
             std::equal(
                 std::begin(decoded_root), std::end(decoded_root),
                 std::begin(root)),
         "packetized animation bridge changed exact words or root");
}

void TestAnimationWordStreamRejectsMalformedInput() {
  const float root[3] = {1.0f, 2.0f, 3.0f};
  const std::vector<std::uint16_t> words = {
      0, 1, 9, 0, 0x1234,
  };
  std::vector<std::uint8_t> encoded(
      AnimationWordStreamByteCount(words.size()));
  Expect(EncodeAnimationWordStream(
             root, 0xFFFF, words, encoded),
         "malformed-input fixture did not encode");

  float decoded_root[3] = {};
  std::uint16_t decoded_root_bone = 0;
  std::vector<std::uint16_t> decoded_words;
  Expect(!DecodeAnimationWordStream(
             std::span<const std::uint8_t>(encoded).first(
                 encoded.size() - 1),
             decoded_root, decoded_root_bone, decoded_words),
         "truncated animation word stream was accepted");
  encoded.push_back(0);
  Expect(!DecodeAnimationWordStream(
             encoded, decoded_root, decoded_root_bone,
             decoded_words),
         "animation word stream with trailing data was accepted");
  encoded.pop_back();

  std::vector<std::uint8_t> invalid_track_count = encoded;
  invalid_track_count[18] = 0;
  invalid_track_count[19] = 0;
  Expect(!DecodeAnimationWordStream(
             invalid_track_count, decoded_root,
             decoded_root_bone, decoded_words),
         "animation word stream with zero tracks was accepted");

  float invalid_root[3] = {
      std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
  Expect(!EncodeAnimationWordStream(
             invalid_root, 0, words, encoded),
         "animation word stream accepted a non-finite root");
  const std::vector<std::uint16_t> too_short = {1, 1, 0};
  Expect(!EncodeAnimationWordStream(
             root, 0, too_short,
             std::span<std::uint8_t>(encoded).first(
                 AnimationWordStreamByteCount(too_short.size()))),
         "animation word stream accepted a short word header");
}

void TestPacketizerBoundarySizesAndRollover() {
  constexpr std::array<std::size_t, 5> sizes = {
      1,
      kMaximumPoseFragmentBytes,
      kMaximumPoseFragmentBytes + 1,
      kMaximumPoseGroupBytes - 1,
      kMaximumPoseGroupBytes,
  };
  constexpr std::array<std::size_t, 5> expected_counts = {
      1, 1, 2, 58, 58,
  };
  std::array<PoseGroupDatagram, 58> descriptors{};
  for (std::size_t test = 0; test < sizes.size(); ++test) {
    const std::vector<std::uint8_t> source =
        Bytes(sizes[test], static_cast<std::uint8_t>(test));
    const std::size_t count = BuildPoseGroupDatagrams(
        Request(
            MessageKind::kPoseBaseline,
            source,
            static_cast<std::uint32_t>(80 + test),
            0),
        descriptors);
    Expect(count == expected_counts[test],
           "boundary group made wrong fragment count");
    if (count != 0) {
      Expect(descriptors[count - 1].header.fragment_offset +
                 descriptors[count - 1].header.fragment_bytes ==
                 source.size(),
             "boundary fragments did not cover exact group range");
      Expect(Encode(
                 descriptors[count - 1], source)
                 .size() <= kMaximumDatagramBytes,
             "boundary fragment exceeded datagram ceiling");
    }
  }

  const std::vector<std::uint8_t> rollover = Bytes(2500, 7);
  PoseGroupPacketizeRequest request =
      Request(MessageKind::kPoseBaseline, rollover, 90, 0);
  request.envelope.sequence = UINT32_MAX - 1;
  Expect(BuildPoseGroupDatagrams(request, descriptors) == 3,
         "rollover group did not packetize");
  Expect(descriptors[0].envelope.sequence == UINT32_MAX - 1 &&
             descriptors[1].envelope.sequence == UINT32_MAX &&
             descriptors[2].envelope.sequence == 0,
         "packetizer sequence did not roll over safely");
}

void TestReorderDuplicateAndCompletion() {
  const std::vector<std::uint8_t> source = Bytes(2500, 17);
  std::array<PoseGroupDatagram, 3> descriptors{};
  Expect(BuildPoseGroupDatagrams(
             Request(MessageKind::kPoseBaseline, source, 41, 0, 1),
             descriptors) == 3,
         "reorder test did not packetize");

  PoseGroupReassembler reassembler;
  const std::array<std::size_t, 4> order = {2, 0, 0, 1};
  ReassemblyPushResult result;
  for (std::size_t arrival = 0; arrival < order.size(); ++arrival) {
    const std::size_t index = order[arrival];
    const PoseGroupDatagram& packet = descriptors[index];
    const std::span<const std::uint8_t> fragment =
        std::span<const std::uint8_t>(source).subspan(
            packet.header.fragment_offset,
            packet.header.fragment_bytes);
    result = reassembler.Push(
        packet.envelope, packet.header, fragment,
        1000 + arrival);
    if (arrival == 2) {
      Expect(result.disposition ==
                 ReassemblyDisposition::kDuplicateFragment,
             "duplicate fragment was not detected");
    }
  }
  Expect(result.disposition ==
             ReassemblyDisposition::kGroupComplete &&
             result.completed.has_value(),
         "reordered fragments did not complete group");
  Expect(result.completed->bytes == source,
         "reordered reassembly changed group bytes");
  Expect(result.completed->pose_id == 41 &&
             result.completed->group_id == 1 &&
             result.completed->completed_at_sequence == 501,
         "completed group metadata changed");
  Expect(reassembler.active_slots() == 0 &&
             reassembler.buffered_bytes() == 0,
         "completed group retained reassembly memory");
}

void TestLossExpiryRequestsBaseline() {
  const std::vector<std::uint8_t> source = Bytes(2500, 31);
  std::array<PoseGroupDatagram, 3> descriptors{};
  Expect(BuildPoseGroupDatagrams(
             Request(MessageKind::kPoseBaseline, source, 50, 0, 2),
             descriptors) == 3,
         "loss test did not packetize");
  PoseGroupReassembler reassembler(8, 256 * 1024, 100);
  PoseReceiverState receiver(1u << 2);
  receiver.ActivateGeneration(2, 100);

  for (std::size_t index : {std::size_t(0), std::size_t(2)}) {
    const PoseGroupDatagram& packet = descriptors[index];
    const auto fragment =
        std::span<const std::uint8_t>(source).subspan(
            packet.header.fragment_offset,
            packet.header.fragment_bytes);
    Expect(receiver.ReceivePoseFragment(
               packet.envelope.sender_role,
               packet.envelope.sender_session,
               packet.envelope.sequence) ==
               PoseReceiveResult::kFragmentAccepted,
           "received loss-test fragment was not acknowledged");
    const ReassemblyPushResult result = reassembler.Push(
        packet.envelope, packet.header, fragment, 1000 + index);
    Expect(result.disposition ==
               ReassemblyDisposition::kFragmentStored,
           "incomplete baseline fragment was not stored");
  }

  const ReassemblyExpiry expiry = reassembler.Expire(1200);
  Expect(expiry.expired_slots == 1 &&
             expiry.baseline_recovery_mask == (1u << 2),
         "expired baseline did not identify recovery group");
  Expect(reassembler.active_slots() == 0 &&
             reassembler.buffered_bytes() == 0,
         "expired baseline retained reassembly memory");

  if (expiry.baseline_recovery_mask != 0) {
    receiver.NotifyBaselineUnavailable();
  }
  Expect(receiver.baseline_request_pending() &&
             receiver.ConsumeBaselineRequest(),
         "expired baseline did not produce bounded recovery request");
  receiver.NotifyBaselineUnavailable();
  Expect(!receiver.baseline_request_pending(),
         "active recovery emitted a request for every expiry");
  receiver.ScheduleBaselineRetry();
  Expect(receiver.baseline_request_pending(),
         "recovery timeout did not re-arm baseline request");
}

void TestEncodedEndToEndBaselineAndDelta() {
  const std::vector<std::uint8_t> core = Bytes(1800, 81);
  const std::vector<std::uint8_t> board = Bytes(1300, 91);
  const std::vector<std::uint8_t> delta = Bytes(1400, 101);
  std::array<PoseGroupDatagram, 2> core_packets{};
  std::array<PoseGroupDatagram, 2> board_packets{};
  std::array<PoseGroupDatagram, 2> delta_packets{};

  PoseGroupPacketizeRequest core_request =
      Request(MessageKind::kPoseBaseline, core, 200, 0, 0);
  PoseGroupPacketizeRequest board_request =
      Request(MessageKind::kPoseBaseline, board, 200, 0, 1);
  board_request.envelope.sequence = 502;
  PoseGroupPacketizeRequest delta_request =
      Request(MessageKind::kPoseDelta, delta, 201, 200, 0);
  delta_request.envelope.sequence = 504;
  Expect(BuildPoseGroupDatagrams(core_request, core_packets) == 2 &&
             BuildPoseGroupDatagrams(
                 board_request, board_packets) == 2 &&
             BuildPoseGroupDatagrams(
                 delta_request, delta_packets) == 2,
         "end-to-end groups did not packetize");

  PoseReceiverState receiver(0b11u);
  receiver.ActivateGeneration(2, 100);
  PoseGroupReassembler reassembler;
  PoseReceiveResult semantic =
      PoseReceiveResult::kFragmentAccepted;

  auto Deliver = [&](const PoseGroupDatagram& descriptor,
                     std::span<const std::uint8_t> group,
                     std::uint64_t now) {
    const std::vector<std::uint8_t> packet =
        Encode(descriptor, group);
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(
               packet, envelope, header, fragment),
           "end-to-end datagram did not decode");
    const PoseReceiveResult packet_result =
        receiver.ReceivePoseFragment(
            envelope.sender_role,
            envelope.sender_session,
            envelope.sequence);
    if (packet_result != PoseReceiveResult::kFragmentAccepted) {
      return packet_result;
    }
    ReassemblyPushResult result =
        reassembler.Push(envelope, header, fragment, now);
    if (result.disposition !=
        ReassemblyDisposition::kGroupComplete) {
      return PoseReceiveResult::kFragmentAccepted;
    }
    const ReassembledPoseGroup& completed =
        *result.completed;
    if (completed.kind == MessageKind::kPoseBaseline) {
      return receiver.CompleteBaselineGroup(
          completed.sender_role,
          completed.sender_session,
          completed.completed_at_sequence,
          completed.pose_id,
          1u << completed.group_id);
    }
    return receiver.CompleteDelta(
        completed.sender_role,
        completed.sender_session,
        completed.baseline_id,
        1u << completed.group_id);
  };

  semantic = Deliver(core_packets[1], core, 1000);
  Expect(semantic == PoseReceiveResult::kFragmentAccepted,
         "reordered core fragment completed too early");
  semantic = Deliver(core_packets[0], core, 1001);
  Expect(semantic == PoseReceiveResult::kBaselineGroupAccepted,
         "complete core group was not staged");
  semantic = Deliver(board_packets[0], board, 1002);
  Expect(semantic == PoseReceiveResult::kFragmentAccepted,
         "board baseline completed too early");
  semantic = Deliver(board_packets[1], board, 1003);
  Expect(semantic == PoseReceiveResult::kBaselineCompleted &&
             receiver.decoded_baseline_id() == 200,
         "all baseline groups did not confirm baseline");

  semantic = Deliver(delta_packets[1], delta, 1004);
  Expect(semantic == PoseReceiveResult::kFragmentAccepted,
         "reordered delta fragment completed too early");
  semantic = Deliver(delta_packets[0], delta, 1005);
  Expect(semantic == PoseReceiveResult::kDeltaAccepted,
         "delta against decoded baseline was rejected");
  Expect(Deliver(delta_packets[0], delta, 1006) ==
             PoseReceiveResult::kDuplicatePacket,
         "packet history did not stop duplicate before reassembly");
  Expect(receiver.packet_history().latest() == 505,
         "end-to-end packet history newest sequence changed");
}

void TestNewerPoseSupersedesIncompletePose() {
  const std::vector<std::uint8_t> old_bytes = Bytes(2500, 41);
  const std::vector<std::uint8_t> new_bytes = Bytes(2500, 51);
  std::array<PoseGroupDatagram, 3> old_packets{};
  std::array<PoseGroupDatagram, 3> new_packets{};
  Expect(BuildPoseGroupDatagrams(
             Request(
                 MessageKind::kPoseDelta, old_bytes, 101, 100, 3),
             old_packets) == 3 &&
             BuildPoseGroupDatagrams(
                 Request(
                     MessageKind::kPoseDelta,
                     new_bytes, 102, 100, 3),
                 new_packets) == 3,
         "supersession test did not packetize");
  PoseGroupReassembler reassembler;

  auto PushPacket = [&](const PoseGroupDatagram& packet,
                        std::span<const std::uint8_t> bytes,
                        std::uint64_t now) {
    return reassembler.Push(
        packet.envelope, packet.header,
        bytes.subspan(
            packet.header.fragment_offset,
            packet.header.fragment_bytes),
        now);
  };

  Expect(PushPacket(old_packets[0], old_bytes, 1000).disposition ==
             ReassemblyDisposition::kFragmentStored,
         "old pose did not begin reassembly");
  ReassemblyPushResult result =
      PushPacket(new_packets[1], new_bytes, 1001);
  Expect(result.disposition ==
             ReassemblyDisposition::kFragmentStored &&
             result.evicted_slots == 1,
         "new pose did not supersede incomplete old pose");
  Expect(PushPacket(old_packets[1], old_bytes, 1002).disposition ==
             ReassemblyDisposition::kStalePose,
         "old pose fragment resumed after supersession");
  result = PushPacket(new_packets[0], new_bytes, 1003);
  Expect(result.disposition ==
             ReassemblyDisposition::kFragmentStored,
         "new pose first fragment was rejected after reorder");
  result = PushPacket(new_packets[2], new_bytes, 1004);
  Expect(result.disposition ==
             ReassemblyDisposition::kGroupComplete &&
             result.completed->bytes == new_bytes,
         "newest pose did not complete after supersession");
}

void TestBoundedSlotEvictionAndDeltaExpiry() {
  const std::vector<std::uint8_t> source = Bytes(2500, 111);
  std::array<PoseGroupDatagram, 3> group_zero{};
  std::array<PoseGroupDatagram, 3> group_one{};
  PoseGroupPacketizeRequest second =
      Request(MessageKind::kPoseBaseline, source, 300, 0, 1);
  second.envelope.sequence = 503;
  Expect(BuildPoseGroupDatagrams(
             Request(
                 MessageKind::kPoseBaseline, source, 300, 0, 0),
             group_zero) == 3 &&
             BuildPoseGroupDatagrams(second, group_one) == 3,
         "bounded-slot groups did not packetize");
  PoseGroupReassembler one_slot(1, 128 * 1024, 100);
  const auto first_zero =
      std::span<const std::uint8_t>(source).first(
          group_zero[0].header.fragment_bytes);
  Expect(one_slot.Push(
             group_zero[0].envelope,
             group_zero[0].header,
             first_zero,
             1000)
             .disposition ==
             ReassemblyDisposition::kFragmentStored,
         "first bounded baseline slot was not stored");
  const auto first_one =
      std::span<const std::uint8_t>(source).first(
          group_one[0].header.fragment_bytes);
  const ReassemblyPushResult replacement = one_slot.Push(
      group_one[0].envelope,
      group_one[0].header,
      first_one,
      1001);
  Expect(replacement.disposition ==
             ReassemblyDisposition::kFragmentStored &&
             replacement.evicted_slots == 1 &&
             replacement.baseline_recovery_mask == 1,
         "bounded slot eviction did not report lost baseline group");

  std::array<PoseGroupDatagram, 3> delta_packets{};
  Expect(BuildPoseGroupDatagrams(
             Request(
                 MessageKind::kPoseDelta, source, 301, 300, 2),
             delta_packets) == 3,
         "delta expiry group did not packetize");
  PoseGroupReassembler delta_reassembler(2, 128 * 1024, 100);
  Expect(delta_reassembler.Push(
             delta_packets[0].envelope,
             delta_packets[0].header,
             first_zero,
             2000)
             .disposition ==
             ReassemblyDisposition::kFragmentStored,
         "incomplete delta was not stored");
  const ReassemblyExpiry delta_expiry =
      delta_reassembler.Expire(2200);
  Expect(delta_expiry.expired_slots == 1 &&
             delta_expiry.baseline_recovery_mask == 0,
         "ordinary expired delta requested a new baseline");
  one_slot.Reset();
  Expect(one_slot.active_slots() == 0 &&
             one_slot.buffered_bytes() == 0,
         "generation reset retained reassembly allocations");
}

void TestConflictAndResourceBounds() {
  const std::vector<std::uint8_t> source = Bytes(2500, 61);
  std::array<PoseGroupDatagram, 3> descriptors{};
  Expect(BuildPoseGroupDatagrams(
             Request(MessageKind::kPoseBaseline, source, 60, 0, 0),
             descriptors) == 3,
         "conflict test did not packetize");
  PoseGroupReassembler reassembler;
  const PoseGroupDatagram& first = descriptors[0];
  const auto first_fragment =
      std::span<const std::uint8_t>(source).first(
          first.header.fragment_bytes);
  Expect(reassembler.Push(
             first.envelope, first.header, first_fragment, 1000)
             .disposition ==
             ReassemblyDisposition::kFragmentStored,
         "conflict test did not store first fragment");
  std::vector<std::uint8_t> changed(
      first_fragment.begin(), first_fragment.end());
  changed[0] ^= 0xFFu;
  const ReassemblyPushResult conflict = reassembler.Push(
      first.envelope, first.header, changed, 1001);
  Expect(conflict.disposition ==
             ReassemblyDisposition::kConflictingFragment &&
             conflict.baseline_recovery_mask == 1,
         "conflicting baseline fragment did not fail closed");
  Expect(reassembler.active_slots() == 0,
         "conflicting fragment retained poisoned slot");

  Expect(reassembler.Push(
             first.envelope, first.header, first_fragment, 1002)
             .disposition ==
             ReassemblyDisposition::kFragmentStored,
         "metadata conflict test did not restart slot");
  Envelope mismatched_envelope = descriptors[1].envelope;
  ++mismatched_envelope.sender_time_us;
  const auto second_fragment =
      std::span<const std::uint8_t>(source).subspan(
          descriptors[1].header.fragment_offset,
          descriptors[1].header.fragment_bytes);
  const ReassemblyPushResult metadata_conflict =
      reassembler.Push(
          mismatched_envelope,
          descriptors[1].header,
          second_fragment,
          1003);
  Expect(metadata_conflict.disposition ==
             ReassemblyDisposition::kConflictingFragment &&
             metadata_conflict.baseline_recovery_mask == 1,
         "inconsistent pose timestamp was mixed into group");

  PoseGroupReassembler tiny(1, 2000, 1000);
  const ReassemblyPushResult limited = tiny.Push(
      first.envelope, first.header, first_fragment, 1000);
  Expect(limited.disposition ==
             ReassemblyDisposition::kResourceLimit &&
             limited.baseline_recovery_mask == 1 &&
             tiny.buffered_bytes() == 0,
         "oversized group exceeded reassembly memory budget");
}

void TestMalformedDatagramFailsWithoutOutputs() {
  const std::vector<std::uint8_t> source = Bytes(20, 71);
  std::array<PoseGroupDatagram, 1> descriptors{};
  Expect(BuildPoseGroupDatagrams(
             Request(MessageKind::kPoseBaseline, source, 70, 0),
             descriptors) == 1,
         "malformed test did not packetize");
  std::vector<std::uint8_t> packet =
      Encode(descriptors[0], source);
  Envelope envelope;
  envelope.sender_role = 99;
  PoseGroupHeader header;
  header.pose_id = 999;
  std::span<const std::uint8_t> fragment;
  packet.pop_back();
  Expect(!DecodePoseGroupDatagram(
             packet, envelope, header, fragment),
         "truncated complete datagram was accepted");
  Expect(envelope.sender_role == 99 && header.pose_id == 999,
         "failed datagram decode modified outputs");

  std::array<PoseGroupDatagram, 1> output{};
  PoseGroupPacketizeRequest invalid =
      Request(MessageKind::kPoseBaseline, source, 0, 0);
  Expect(BuildPoseGroupDatagrams(invalid, output) == 0,
         "packetizer accepted zero pose ID");
  invalid = Request(MessageKind::kPoseDelta, source, 80, 0);
  Expect(BuildPoseGroupDatagrams(invalid, output) == 0,
         "packetizer accepted delta without baseline");
  invalid = Request(MessageKind::kPoseBaseline, source, 80, 0);
  invalid.envelope.sender_role = 0;
  Expect(BuildPoseGroupDatagrams(invalid, output) == 0,
         "packetizer accepted invalid sender generation");
}

}  // namespace

int main() {
  TestPacketizerBudgetAndRoundTrip();
  TestAnimationWordStreamGoldenBytesAndRoundTrip();
  TestAnimationWordStreamMaximumGroupRoundTrip();
  TestAnimationWordStreamRejectsMalformedInput();
  TestPacketizerBoundarySizesAndRollover();
  TestReorderDuplicateAndCompletion();
  TestLossExpiryRequestsBaseline();
  TestEncodedEndToEndBaselineAndDelta();
  TestNewerPoseSupersedesIncompletePose();
  TestBoundedSlotEvictionAndDeltaExpiry();
  TestConflictAndResourceBounds();
  TestMalformedDatagramFailsWithoutOutputs();

  if (g_failures != 0) {
    std::cerr << g_failures
              << " multiplayer protocol-v12 transport test(s) failed\n";
    return 1;
  }
  std::cout
      << "All multiplayer protocol-v12 transport tests passed\n";
  return 0;
}
