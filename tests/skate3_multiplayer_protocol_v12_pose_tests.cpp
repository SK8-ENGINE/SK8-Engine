#include "skate3_multiplayer_protocol_v12_pose.h"
#include "skate3_multiplayer_protocol_v12_root.h"

#include <algorithm>
#include <array>
#include <cstdint>
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

Envelope ControlEnvelope() {
  Envelope envelope;
  envelope.kind = MessageKind::kPoseControl;
  envelope.flags = kFlagReliable;
  envelope.payload_bytes = kPoseControlPayloadBytes;
  envelope.sender_role = 2;
  envelope.sender_session = 0x11223344u;
  envelope.stream_id = 7;
  return envelope;
}

PoseGroupHeader SampleGroupHeader() {
  PoseGroupHeader header;
  header.pose_id = 0x01020304u;
  header.baseline_id = 0;
  header.total_bytes = kMaximumPoseFragmentBytes + 3;
  header.fragment_offset = kMaximumPoseFragmentBytes;
  header.element_count = 0x0112u;
  header.fragment_bytes = 3;
  header.fragment_index = 1;
  header.fragment_count = 2;
  header.group_id = 5;
  header.encoding = PoseGroupEncoding::kV11WordStream;
  return header;
}

void TestConstantsAndMessageKind() {
  Expect(kPoseControlPayloadBytes == 20, "pose control payload size changed");
  Expect(kPoseGroupHeaderBytes == 24, "pose group header size changed");
  Expect(kMaximumPoseFragmentBytes == 1136,
         "pose group fragment budget changed");
  Expect(kMaximumPoseGroupBytes == 65536, "pose group total budget changed");

  Envelope envelope = ControlEnvelope();
  Expect(EnvelopeShapeValid(envelope),
         "pose-control message kind was not accepted by envelope");
  envelope.kind = static_cast<MessageKind>(8);
  Expect(!EnvelopeShapeValid(envelope),
         "unknown message kind after pose control was accepted");
}

void TestPoseControlGoldenBytesAndRoundTrip() {
  PoseControl control;
  control.type = PoseControlType::kDecodedBaseline;
  control.target_role = 5;
  control.target_stream_id = 0x2233u;
  control.target_session = 0x01020304u;
  control.baseline_id = 0x11121314u;
  control.group_mask = 0x21222324u;

  std::array<std::uint8_t, kPoseControlPayloadBytes> encoded{};
  Expect(EncodePoseControl(control, encoded),
         "valid pose control did not encode");
  constexpr std::array<std::uint8_t, kPoseControlPayloadBytes> expected = {
      0x01, 0x00, 0x05, 0x00, 0x33, 0x22, 0x00, 0x00, 0x04, 0x03,
      0x02, 0x01, 0x14, 0x13, 0x12, 0x11, 0x24, 0x23, 0x22, 0x21,
  };
  Expect(encoded == expected,
         "pose control little-endian golden bytes changed");

  PoseControl decoded;
  Expect(DecodePoseControl(encoded, decoded), "pose control did not decode");
  Expect(decoded.type == control.type &&
             decoded.target_role == control.target_role &&
             decoded.target_stream_id == control.target_stream_id &&
             decoded.target_session == control.target_session &&
             decoded.baseline_id == control.baseline_id &&
             decoded.group_mask == control.group_mask,
         "pose control round trip changed fields");
  Expect(PoseControlEnvelopeShapeValid(ControlEnvelope(), control),
         "valid reliable pose control envelope was rejected");
}

void TestPoseControlValidation() {
  PoseControl control;
  control.type = PoseControlType::kRequestBaseline;
  control.target_role = 3;
  control.target_session = 100;
  control.baseline_id = 0;
  control.group_mask = 0b111;
  Expect(PoseControlShapeValid(control),
         "fresh-baseline request with no prior baseline was rejected");

  Envelope envelope = ControlEnvelope();
  Expect(PoseControlEnvelopeShapeValid(envelope, control),
         "valid baseline request envelope was rejected");
  envelope.flags = 0;
  Expect(!PoseControlEnvelopeShapeValid(envelope, control),
         "unreliable baseline request was accepted");
  envelope = ControlEnvelope();
  envelope.flags = kFlagReliable | kFlagExpires;
  Expect(!PoseControlEnvelopeShapeValid(envelope, control),
         "expirable baseline request was accepted");
  envelope = ControlEnvelope();
  envelope.sender_role = control.target_role;
  Expect(!PoseControlEnvelopeShapeValid(envelope, control),
         "self-targeted pose control was accepted");

  std::array<std::uint8_t, kPoseControlPayloadBytes> encoded{};
  Expect(EncodePoseControl(control, encoded),
         "baseline request test packet did not encode");
  PoseControl output;
  auto malformed = encoded;
  malformed[0] = 3;
  Expect(!DecodePoseControl(malformed, output),
         "unknown pose control type was accepted");
  malformed = encoded;
  malformed[1] = 1;
  Expect(!DecodePoseControl(malformed, output),
         "nonzero pose control reserved byte was accepted");
  malformed = encoded;
  malformed[2] = 0;
  malformed[3] = 0;
  Expect(!DecodePoseControl(malformed, output),
         "zero pose control target role was accepted");
  malformed = encoded;
  malformed[8] = 0;
  malformed[9] = 0;
  malformed[10] = 0;
  malformed[11] = 0;
  Expect(!DecodePoseControl(malformed, output),
         "zero pose control target session was accepted");
  malformed = encoded;
  malformed[16] = 0;
  malformed[17] = 0;
  malformed[18] = 0;
  malformed[19] = 0;
  Expect(!DecodePoseControl(malformed, output),
         "zero requested pose group mask was accepted");
  Expect(!DecodePoseControl(std::span<const std::uint8_t>(encoded).first(
                                kPoseControlPayloadBytes - 1),
                            output),
         "truncated pose control was accepted");

  control.type = PoseControlType::kDecodedBaseline;
  control.baseline_id = 0;
  Expect(!PoseControlShapeValid(control),
         "decoded report with zero baseline was accepted");
}

void TestPoseGroupGoldenBytesAndRoundTrip() {
  const PoseGroupHeader header = SampleGroupHeader();
  std::vector<std::uint8_t> payload(kPoseGroupHeaderBytes +
                                    header.fragment_bytes);
  payload[kPoseGroupHeaderBytes + 0] = 0xAA;
  payload[kPoseGroupHeaderBytes + 1] = 0xBB;
  payload[kPoseGroupHeaderBytes + 2] = 0xCC;
  Expect(EncodePoseGroupHeader(header, MessageKind::kPoseBaseline, payload),
         "valid pose group header did not encode");
  constexpr std::array<std::uint8_t, kPoseGroupHeaderBytes> expected = {
      0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x73, 0x04, 0x00, 0x00,
      0x70, 0x04, 0x00, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x02, 0x05, 0x01,
  };
  Expect(std::equal(expected.begin(), expected.end(), payload.begin()),
         "pose group little-endian golden bytes changed");
  Expect(payload[kPoseGroupHeaderBytes] == 0xAA &&
             payload[kPoseGroupHeaderBytes + 1] == 0xBB &&
             payload[kPoseGroupHeaderBytes + 2] == 0xCC,
         "pose group header encoding overwrote fragment bytes");

  PoseGroupHeader decoded;
  Expect(DecodePoseGroupHeader(payload, MessageKind::kPoseBaseline, decoded),
         "pose group payload did not decode");
  Expect(decoded.pose_id == header.pose_id &&
             decoded.baseline_id == header.baseline_id &&
             decoded.total_bytes == header.total_bytes &&
             decoded.fragment_offset == header.fragment_offset &&
             decoded.element_count == header.element_count &&
             decoded.fragment_bytes == header.fragment_bytes &&
             decoded.fragment_index == header.fragment_index &&
             decoded.fragment_count == header.fragment_count &&
             decoded.group_id == header.group_id &&
             decoded.encoding == header.encoding,
         "pose group round trip changed fields");

  Envelope envelope;
  envelope.kind = MessageKind::kPoseBaseline;
  envelope.flags = kFlagKeyframe | kFlagExpires;
  envelope.payload_bytes = kPoseGroupHeaderBytes + header.fragment_bytes;
  envelope.sender_role = 2;
  envelope.sender_session = 100;
  Expect(PoseGroupEnvelopeShapeValid(envelope, header),
         "valid expirable baseline envelope was rejected");
  envelope.flags = kFlagReliable | kFlagKeyframe;
  Expect(!PoseGroupEnvelopeShapeValid(envelope, header),
         "reliable pose baseline was accepted");
}

void TestPoseGroupFragmentBoundaries() {
  PoseGroupHeader header;
  header.pose_id = 100;
  header.baseline_id = 90;
  header.total_bytes = kMaximumPoseGroupBytes;
  header.element_count = kMaximumPoseGroupElements;
  header.fragment_count =
      static_cast<std::uint8_t>(PoseGroupFragmentCount(header.total_bytes));
  header.fragment_index = header.fragment_count - 1;
  header.fragment_offset = static_cast<std::uint32_t>(
      PoseGroupFragmentOffset(header.fragment_index));
  header.fragment_bytes = static_cast<std::uint16_t>(
      PoseGroupFragmentByteCount(header.total_bytes, header.fragment_index));
  header.group_id = 31;
  header.encoding = PoseGroupEncoding::kBitPackedV1;
  Expect(PoseGroupHeaderShapeValid(header, MessageKind::kPoseDelta),
         "maximum bounded pose group fragment was rejected");
  Expect(header.fragment_count == 58 && header.fragment_bytes == 784,
         "maximum pose group fragmentation changed");

  PoseGroupHeader malformed = header;
  malformed.total_bytes = kMaximumPoseGroupBytes + 1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "oversized pose group was accepted");
  malformed = header;
  malformed.fragment_offset -= 1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "pose fragment with wrong offset was accepted");
  malformed = header;
  malformed.fragment_bytes += 1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "pose fragment with wrong size was accepted");
  malformed = header;
  malformed.fragment_count -= 1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "pose group with wrong fragment count was accepted");
  malformed = header;
  malformed.group_id = 32;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "pose group ID above mask width was accepted");
  malformed = header;
  malformed.element_count = 0;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "empty pose group element set was accepted");
  malformed = header;
  malformed.baseline_id = 0;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "delta without confirmed baseline was accepted");
  malformed = header;
  malformed.pose_id = malformed.baseline_id - 1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "delta older than its baseline was accepted");
  malformed = header;
  malformed.baseline_id = 1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseBaseline),
         "baseline carrying a delta reference was accepted");
  malformed = header;
  malformed.encoding = PoseGroupEncoding::kSemanticDeltaV1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseBaseline),
         "baseline carrying semantic-delta encoding was accepted");
  malformed.baseline_id = 1;
  Expect(PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "semantic delta encoding was rejected for a delta");
  malformed.baseline_id = 0;
  malformed.encoding = PoseGroupEncoding::kBlockDeltaV1;
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseBaseline),
         "baseline carrying block-delta encoding was accepted");
  malformed.baseline_id = 1;
  Expect(PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "block delta encoding was rejected for a delta");
  malformed.encoding = static_cast<PoseGroupEncoding>(5);
  Expect(!PoseGroupHeaderShapeValid(malformed, MessageKind::kPoseDelta),
         "unknown pose encoding was accepted");
}

void TestPoseGroupMalformedDecode() {
  const PoseGroupHeader header = SampleGroupHeader();
  std::vector<std::uint8_t> payload(kPoseGroupHeaderBytes +
                                    header.fragment_bytes);
  Expect(EncodePoseGroupHeader(header, MessageKind::kPoseBaseline, payload),
         "malformed pose test header did not encode");
  PoseGroupHeader output = header;
  output.pose_id = 999;
  Expect(!DecodePoseGroupHeader(
             std::span<const std::uint8_t>(payload).first(payload.size() - 1),
             MessageKind::kPoseBaseline, output),
         "truncated pose fragment was accepted");
  Expect(output.pose_id == 999, "failed pose group decode modified output");
  payload.push_back(0);
  Expect(!DecodePoseGroupHeader(payload, MessageKind::kPoseBaseline, output),
         "pose fragment with trailing bytes was accepted");
  payload.pop_back();
  payload[23] = 4;
  Expect(!DecodePoseGroupHeader(payload, MessageKind::kPoseBaseline, output),
         "unknown pose group encoding was accepted");
  Expect(!DecodePoseGroupHeader(payload, MessageKind::kRootSnapshot, output),
         "pose group decoded for unrelated message kind");
}

void TestRootSnapshotRoundTripAndPolicy() {
  RootSnapshot snapshot;
  snapshot.position[0] = 1.0f;
  snapshot.position[1] = -2.5f;
  snapshot.position[2] = 3.25f;
  snapshot.x_axis[0] = 1.0f;
  snapshot.z_axis[2] = -1.0f;
  snapshot.board_state_flags = 0xA1B2C3D4u;

  std::array<std::uint8_t, kRootSnapshotPayloadBytes> bytes{};
  Expect(EncodeRootSnapshot(snapshot, bytes),
         "v12 root snapshot did not encode");
  Expect(bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x80 &&
             bytes[3] == 0x3F && bytes[36] == 0xD4 && bytes[37] == 0xC3 &&
             bytes[38] == 0xB2 && bytes[39] == 0xA1,
         "v12 root snapshot explicit-endian bytes changed");

  RootSnapshot decoded;
  Expect(DecodeRootSnapshot(bytes, decoded),
         "v12 root snapshot did not decode");
  Expect(decoded.position[0] == snapshot.position[0] &&
             decoded.position[1] == snapshot.position[1] &&
             decoded.position[2] == snapshot.position[2] &&
             decoded.x_axis[0] == snapshot.x_axis[0] &&
             decoded.z_axis[2] == snapshot.z_axis[2] &&
             decoded.board_state_flags == snapshot.board_state_flags,
         "v12 root snapshot round trip changed fields");
  Expect(!DecodeRootSnapshot(
             std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1),
             decoded),
         "truncated v12 root snapshot was accepted");

  Envelope envelope;
  envelope.kind = MessageKind::kRootSnapshot;
  envelope.flags = kFlagExpires;
  envelope.payload_bytes = kRootSnapshotPayloadBytes;
  envelope.sender_role = 1;
  envelope.sender_session = 2;
  envelope.stream_id = kRootSnapshotStreamId;
  Expect(RootSnapshotEnvelopeShapeValid(envelope),
         "valid v12 root envelope was rejected");
  envelope.flags = kFlagReliable;
  Expect(!RootSnapshotEnvelopeShapeValid(envelope),
         "reliable v12 root envelope was accepted");
}

} // namespace

int main() {
  TestConstantsAndMessageKind();
  TestPoseControlGoldenBytesAndRoundTrip();
  TestPoseControlValidation();
  TestPoseGroupGoldenBytesAndRoundTrip();
  TestPoseGroupFragmentBoundaries();
  TestPoseGroupMalformedDecode();
  TestRootSnapshotRoundTripAndPolicy();

  if (g_failures != 0) {
    std::cerr << g_failures
              << " multiplayer protocol-v12 pose test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer protocol-v12 pose tests passed\n";
  return 0;
}
