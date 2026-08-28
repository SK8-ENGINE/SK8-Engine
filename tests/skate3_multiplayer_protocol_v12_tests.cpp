#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_protocol_v12_live.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using skate3::multiplayer::protocol_v12::Capabilities;
using skate3::multiplayer::protocol_v12::CapabilitiesShapeValid;
using skate3::multiplayer::protocol_v12::DecodeCapabilities;
using skate3::multiplayer::protocol_v12::DecodeEnvelope;
using skate3::multiplayer::protocol_v12::EncodeCapabilities;
using skate3::multiplayer::protocol_v12::EncodeEnvelope;
using skate3::multiplayer::protocol_v12::Envelope;
using skate3::multiplayer::protocol_v12::EnvelopeShapeValid;
using skate3::multiplayer::protocol_v12::kCapabilitiesPayloadBytes;
using skate3::multiplayer::protocol_v12::kEnvelopeBytes;
using skate3::multiplayer::protocol_v12::kEnvelopeMagic;
using skate3::multiplayer::protocol_v12::kFeatureAppearanceRecipes;
using skate3::multiplayer::protocol_v12::kFeatureExplicitLittleEndian;
using skate3::multiplayer::protocol_v12::kFeatureLiveMapEditing;
using skate3::multiplayer::protocol_v12::kFeaturePoseAcknowledgements;
using skate3::multiplayer::protocol_v12::kFeaturePoseGroups;
using skate3::multiplayer::protocol_v12::kFlagExpires;
using skate3::multiplayer::protocol_v12::kFlagKeyframe;
using skate3::multiplayer::protocol_v12::kFlagReliable;
using skate3::multiplayer::protocol_v12::kMaximumDatagramBytes;
using skate3::multiplayer::protocol_v12::kMaximumPayloadBytes;
using skate3::multiplayer::protocol_v12::kProtocolVersion;
using skate3::multiplayer::protocol_v12::MessageKind;
using skate3::multiplayer::protocol_v12::NegotiateFeatureBits;
using skate3::multiplayer::protocol_v12::SequenceAcknowledged;
namespace live = skate3::multiplayer::protocol_v12::live;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

Envelope SampleEnvelope() {
  Envelope envelope;
  envelope.kind = MessageKind::kPoseBaseline;
  envelope.flags = kFlagKeyframe | kFlagExpires;
  envelope.sender_role = 5;
  envelope.stream_id = 0x2233u;
  envelope.sender_session = 0x01020304u;
  envelope.sequence = 0x11121314u;
  envelope.acknowledged_sequence = 0x21222324u;
  envelope.receive_history = 0x31323334u;
  envelope.sender_time_us = 0x3132333435363738ull;
  return envelope;
}

void TestEnvelopeConstants() {
  Expect(kProtocolVersion == 12, "v12 protocol version changed");
  Expect(kEnvelopeMagic == 0x324D334Bu, "v12 envelope magic changed");
  Expect(kEnvelopeBytes == 40, "v12 envelope size changed");
  Expect(kMaximumDatagramBytes == 1200, "v12 datagram budget changed");
  Expect(kMaximumPayloadBytes == 1160, "v12 payload budget changed");
  Expect(kCapabilitiesPayloadBytes == 36,
         "v12 capabilities payload size changed");
}

void TestEnvelopeGoldenBytes() {
  const Envelope envelope = SampleEnvelope();
  std::array<std::uint8_t, kEnvelopeBytes> encoded{};
  Expect(EncodeEnvelope(envelope, encoded),
         "valid v12 envelope did not encode");

  constexpr std::array<std::uint8_t, kEnvelopeBytes> expected = {
      0x4B, 0x33, 0x4D, 0x32, 0x0C, 0x00, 0x03, 0x05, 0x28, 0x00,
      0x00, 0x00, 0x05, 0x00, 0x33, 0x22, 0x04, 0x03, 0x02, 0x01,
      0x14, 0x13, 0x12, 0x11, 0x24, 0x23, 0x22, 0x21, 0x34, 0x33,
      0x32, 0x31, 0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
  };
  Expect(encoded == expected,
         "v12 explicit little-endian golden bytes changed");
}

void TestEnvelopeRoundTripWithPayload() {
  Envelope envelope = SampleEnvelope();
  envelope.payload_bytes = 3;
  std::vector<std::uint8_t> packet(kEnvelopeBytes + envelope.payload_bytes);
  packet[kEnvelopeBytes + 0] = 0xAA;
  packet[kEnvelopeBytes + 1] = 0xBB;
  packet[kEnvelopeBytes + 2] = 0xCC;
  Expect(EncodeEnvelope(envelope, packet),
         "v12 payload envelope did not encode");

  Envelope decoded;
  Expect(DecodeEnvelope(packet, decoded),
         "v12 payload envelope did not decode");
  Expect(decoded.magic == envelope.magic &&
             decoded.version == envelope.version &&
             decoded.kind == envelope.kind && decoded.flags == envelope.flags &&
             decoded.payload_bytes == envelope.payload_bytes &&
             decoded.sender_role == envelope.sender_role &&
             decoded.stream_id == envelope.stream_id &&
             decoded.sender_session == envelope.sender_session &&
             decoded.sequence == envelope.sequence &&
             decoded.acknowledged_sequence == envelope.acknowledged_sequence &&
             decoded.receive_history == envelope.receive_history &&
             decoded.sender_time_us == envelope.sender_time_us,
         "v12 decoded envelope fields changed");
  Expect(packet[kEnvelopeBytes] == 0xAA && packet[kEnvelopeBytes + 1] == 0xBB &&
             packet[kEnvelopeBytes + 2] == 0xCC,
         "v12 header encoding overwrote its payload");
}

void TestCapabilitiesGoldenBytesAndRoundTrip() {
  Capabilities capabilities;
  capabilities.feature_bits = kFeatureExplicitLittleEndian |
                              kFeaturePoseAcknowledgements |
                              kFeaturePoseGroups | kFeatureAppearanceRecipes;
  capabilities.map_hash = 0x0102030405060708ull;
  capabilities.build_hash = 0x1112131415161718ull;
  capabilities.content_hash = 0x2122232425262728ull;
  capabilities.maximum_datagram_bytes = 1200;
  capabilities.maximum_pose_groups = 6;

  std::array<std::uint8_t, kCapabilitiesPayloadBytes> encoded{};
  Expect(EncodeCapabilities(capabilities, encoded),
         "valid v12 capabilities did not encode");
  constexpr std::array<std::uint8_t, kCapabilitiesPayloadBytes> expected = {
      0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05,
      0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
      0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 0xB0, 0x04, 0x06, 0x00,
  };
  Expect(encoded == expected, "v12 capabilities golden bytes changed");

  Capabilities decoded;
  Expect(DecodeCapabilities(encoded, decoded),
         "v12 capabilities did not decode");
  Expect(decoded.feature_bits == capabilities.feature_bits &&
             decoded.map_hash == capabilities.map_hash &&
             decoded.build_hash == capabilities.build_hash &&
             decoded.content_hash == capabilities.content_hash &&
             decoded.maximum_datagram_bytes ==
                 capabilities.maximum_datagram_bytes &&
             decoded.maximum_pose_groups == capabilities.maximum_pose_groups,
         "v12 capabilities round trip changed fields");

  auto malformed = encoded;
  malformed[0] = 0;
  Expect(!DecodeCapabilities(malformed, decoded),
         "v12 capabilities without explicit-endian support were accepted");
  malformed = encoded;
  malformed[0] |= 0x80u;
  Expect(DecodeCapabilities(malformed, decoded),
         "forward-compatible v12 feature bit was rejected");
  Expect(
      NegotiateFeatureBits(capabilities.feature_bits, decoded.feature_bits) ==
          capabilities.feature_bits,
      "unknown v12 feature bit entered the negotiated feature set");
  malformed = encoded;
  malformed[32] = 0xFFu;
  malformed[33] = 0xFFu;
  Expect(!DecodeCapabilities(malformed, decoded),
         "oversized negotiated datagram was accepted");
  malformed = encoded;
  malformed[34] = 0;
  Expect(!DecodeCapabilities(malformed, decoded),
         "zero negotiated pose groups were accepted");
  malformed = encoded;
  malformed[35] = 1;
  Expect(!DecodeCapabilities(malformed, decoded),
         "nonzero reserved capabilities byte was accepted");
  Expect(!DecodeCapabilities(std::span<const std::uint8_t>(encoded).first(
                                 kCapabilitiesPayloadBytes - 1),
                             decoded),
         "truncated v12 capabilities were accepted");
}

void TestLiveCapabilityContract() {
  constexpr live::CompatibilityIdentity identity =
      live::MakeCompatibilityIdentity("test-map");
  constexpr live::CompatibilityIdentity same =
      live::MakeCompatibilityIdentity("test-map");
  constexpr live::CompatibilityIdentity other =
      live::MakeCompatibilityIdentity("other-map");
  static_assert(identity == same);
  static_assert(identity.map_hash != other.map_hash);
  static_assert(identity.build_hash != 0);
  static_assert(identity.content_hash != 0);

  constexpr Capabilities capabilities = live::MakeCapabilities(identity);
  static_assert(capabilities.feature_bits ==
                (kFeatureExplicitLittleEndian | kFeaturePoseAcknowledgements |
                 kFeaturePoseGroups | kFeatureLiveMapEditing));
  static_assert(capabilities.map_hash == identity.map_hash);
  static_assert(capabilities.build_hash == identity.build_hash);
  static_assert(capabilities.content_hash == identity.content_hash);
  static_assert(CapabilitiesShapeValid(capabilities));

  Envelope envelope;
  envelope.flags = kFlagReliable;
  envelope.payload_bytes = kCapabilitiesPayloadBytes;
  envelope.sender_role = 1;
  envelope.sender_session = 2;
  Expect(live::CapabilityEnvelopeShapeValid(envelope),
         "live capability envelope was rejected");
  envelope.flags = 0;
  Expect(!live::CapabilityEnvelopeShapeValid(envelope),
         "unreliable live capability envelope was accepted");
  envelope.flags = kFlagReliable;
  envelope.stream_id = 1;
  Expect(!live::CapabilityEnvelopeShapeValid(envelope),
         "stream-scoped live capability envelope was accepted");
}

void TestMalformedEnvelopeRejection() {
  Envelope envelope = SampleEnvelope();
  std::array<std::uint8_t, kEnvelopeBytes> packet{};
  Expect(EncodeEnvelope(envelope, packet), "test envelope did not encode");

  Envelope output = SampleEnvelope();
  output.sender_role = 99;
  Expect(!DecodeEnvelope(
             std::span<const std::uint8_t>(packet).first(kEnvelopeBytes - 1),
             output),
         "truncated v12 envelope was accepted");
  Expect(output.sender_role == 99, "failed v12 decode modified its output");

  auto malformed = packet;
  malformed[0] ^= 0xFFu;
  Expect(!DecodeEnvelope(malformed, output), "wrong v12 magic was accepted");
  malformed = packet;
  malformed[4] = 11;
  Expect(!DecodeEnvelope(malformed, output), "wrong v12 version was accepted");
  malformed = packet;
  malformed[6] = 0;
  Expect(!DecodeEnvelope(malformed, output),
         "unknown v12 message kind was accepted");
  malformed = packet;
  malformed[7] = 0x80u;
  Expect(!DecodeEnvelope(malformed, output),
         "unknown v12 envelope flag was accepted");
  malformed = packet;
  malformed[8] = 63;
  Expect(!DecodeEnvelope(malformed, output),
         "wrong v12 header size was accepted");
  malformed = packet;
  malformed[12] = 0;
  malformed[13] = 0;
  Expect(!DecodeEnvelope(malformed, output), "role zero was accepted");
  malformed = packet;
  malformed[12] = 101;
  malformed[13] = 0;
  Expect(!DecodeEnvelope(malformed, output), "role above 100 was accepted");
  malformed = packet;
  malformed[16] = 0;
  malformed[17] = 0;
  malformed[18] = 0;
  malformed[19] = 0;
  Expect(!DecodeEnvelope(malformed, output),
         "zero sender session was accepted");

  envelope.payload_bytes = kMaximumPayloadBytes + 1;
  Expect(!EnvelopeShapeValid(envelope),
         "oversized v12 payload shape was accepted");
  Expect(!EncodeEnvelope(envelope, packet), "oversized v12 payload encoded");

  envelope = SampleEnvelope();
  envelope.payload_bytes = 1;
  Expect(EncodeEnvelope(envelope, packet),
         "mismatched-payload test envelope did not encode");
  Expect(!DecodeEnvelope(packet, output),
         "v12 packet missing its declared payload was accepted");
}

void TestAcknowledgementHistory() {
  constexpr std::uint32_t latest = 100;
  constexpr std::uint32_t history = (1u << 0) | (1u << 4) | (1u << 31);
  Expect(SequenceAcknowledged(latest, latest, history),
         "latest v12 sequence was not acknowledged");
  Expect(SequenceAcknowledged(99, latest, history),
         "v12 acknowledgement bit zero was ignored");
  Expect(SequenceAcknowledged(95, latest, history),
         "v12 acknowledgement history bit four was ignored");
  Expect(SequenceAcknowledged(68, latest, history),
         "v12 acknowledgement history bit 31 was ignored");
  Expect(!SequenceAcknowledged(98, latest, history),
         "unset v12 acknowledgement bit was accepted");
  Expect(!SequenceAcknowledged(67, latest, UINT32_MAX),
         "sequence older than the v12 history window was accepted");
  Expect(!SequenceAcknowledged(101, latest, UINT32_MAX),
         "future sequence was accepted by v12 acknowledgement history");
  Expect(SequenceAcknowledged(UINT32_MAX, 0, 1u),
         "v12 acknowledgement history failed across rollover");
}

} // namespace

int main() {
  TestEnvelopeConstants();
  TestEnvelopeGoldenBytes();
  TestEnvelopeRoundTripWithPayload();
  TestCapabilitiesGoldenBytesAndRoundTrip();
  TestLiveCapabilityContract();
  TestMalformedEnvelopeRejection();
  TestAcknowledgementHistory();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer protocol-v12 test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer protocol-v12 tests passed\n";
  return 0;
}
