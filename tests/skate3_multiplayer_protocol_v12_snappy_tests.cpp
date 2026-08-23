#include "skate3_multiplayer_protocol_v12_snappy.h"
#include "skate3_multiplayer_protocol_v12_transport.h"

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
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

std::vector<std::uint8_t> Source(std::size_t bytes) {
  std::vector<std::uint8_t> source(bytes);
  std::uint32_t random = 0x51A77011u;
  for (std::size_t index = 0; index < source.size(); ++index) {
    random = random * 1664525u + 1013904223u;
    source[index] = index % 31 < 24
                        ? static_cast<std::uint8_t>((index / 31) & 0xffu)
                        : static_cast<std::uint8_t>(random >> 24);
  }
  return source;
}

void TestEveryInnerEncoding() {
  const auto source = Source(8192);
  for (const PoseGroupEncoding encoding :
       {PoseGroupEncoding::kV11WordStream,
        PoseGroupEncoding::kBitPackedV1,
        PoseGroupEncoding::kSemanticDeltaV1,
        PoseGroupEncoding::kBlockDeltaV1,
        PoseGroupEncoding::kPredictiveDeltaV1}) {
    std::vector<std::uint8_t> encoded;
    Expect(EncodeSnappyPoseGroup(encoding, source, encoded),
           "valid Snappy pose group did not encode");
    Expect(encoded.size() < source.size(),
           "compressible Snappy pose group did not shrink");
    PoseGroupEncoding decoded_encoding = PoseGroupEncoding::kSnappyV1;
    std::vector<std::uint8_t> decoded;
    Expect(DecodeSnappyPoseGroup(encoded, decoded_encoding, decoded) &&
               decoded_encoding == encoding && decoded == source,
           "Snappy pose group changed exact bytes or inner encoding");
  }
}

void TestMaximumAndMalformedInputs() {
  const auto source = Source(kMaximumLosslessDecodedBytes);
  std::vector<std::uint8_t> encoded;
  Expect(EncodeSnappyPoseGroup(PoseGroupEncoding::kPredictiveDeltaV1,
                               source, encoded),
         "maximum bounded Snappy group did not encode");
  for (std::size_t prefix = 0; prefix < encoded.size(); ++prefix) {
    PoseGroupEncoding inner = PoseGroupEncoding::kV11WordStream;
    std::vector<std::uint8_t> decoded{1, 2, 3};
    Expect(!DecodeSnappyPoseGroup(
               std::span<const std::uint8_t>(encoded).first(prefix),
               inner, decoded) &&
               decoded == std::vector<std::uint8_t>({1, 2, 3}),
           "truncated Snappy group decoded or changed destination");
  }

  for (const std::size_t offset : {0u, 4u, 5u, 6u, 8u}) {
    std::vector<std::uint8_t> malformed = encoded;
    malformed[offset] ^= 0x7fu;
    PoseGroupEncoding inner = PoseGroupEncoding::kV11WordStream;
    std::vector<std::uint8_t> decoded;
    Expect(!DecodeSnappyPoseGroup(malformed, inner, decoded),
           "malformed Snappy header decoded");
  }
  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0);
  PoseGroupEncoding inner = PoseGroupEncoding::kV11WordStream;
  std::vector<std::uint8_t> decoded;
  Expect(!DecodeSnappyPoseGroup(trailing, inner, decoded),
         "Snappy group accepted trailing compressed data");
  Expect(!EncodeSnappyPoseGroup(PoseGroupEncoding::kSnappyV1,
                                source, encoded),
         "nested Snappy encoding was accepted");
  Expect(!EncodeSnappyPoseGroup(
             static_cast<PoseGroupEncoding>(7), source, encoded),
         "unknown Snappy inner encoding was accepted");
}

void TestFragmentRoundTrip() {
  const auto source = Source(20'000);
  std::vector<std::uint8_t> encoded;
  Expect(EncodeSnappyPoseGroup(PoseGroupEncoding::kPredictiveDeltaV1,
                               source, encoded),
         "fragment fixture did not encode");
  PoseGroupPacketizeRequest request;
  request.envelope.kind = MessageKind::kPoseDelta;
  request.envelope.flags = kFlagExpires;
  request.envelope.sender_role = 2;
  request.envelope.stream_id = 2;
  request.envelope.sender_session = 123;
  request.envelope.sequence = 500;
  request.envelope.sender_time_us = 9000;
  request.pose_id = 50;
  request.baseline_id = 40;
  request.element_count = 4;
  request.group_id = 0;
  request.encoding = PoseGroupEncoding::kSnappyV1;
  request.group_bytes = encoded;
  std::array<PoseGroupDatagram, 58> descriptors{};
  const std::size_t descriptor_count =
      BuildPoseGroupDatagrams(request, descriptors);
  Expect(descriptor_count > 0, "Snappy group did not packetize");
  PoseGroupReassembler reassembler;
  std::vector<std::uint8_t> reconstructed;
  for (std::size_t index = descriptor_count; index > 0; --index) {
    const PoseGroupDatagram& descriptor = descriptors[index - 1];
    std::vector<std::uint8_t> datagram(
        kEnvelopeBytes + descriptor.envelope.payload_bytes);
    Expect(EncodePoseGroupDatagram(descriptor, encoded, datagram),
           "Snappy fragment did not encode");
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(datagram, envelope, header, fragment),
           "Snappy fragment did not decode");
    const auto result =
        reassembler.Push(envelope, header, fragment, 10'000 + index);
    if (result.completed) {
      reconstructed = result.completed->bytes;
    }
  }
  PoseGroupEncoding inner = PoseGroupEncoding::kV11WordStream;
  std::vector<std::uint8_t> decoded;
  Expect(DecodeSnappyPoseGroup(reconstructed, inner, decoded) &&
             inner == PoseGroupEncoding::kPredictiveDeltaV1 &&
             decoded == source,
         "fragmented Snappy group did not round trip exactly");
}

}  // namespace

int main() {
  TestEveryInnerEncoding();
  TestMaximumAndMalformedInputs();
  TestFragmentRoundTrip();
  if (g_failures != 0) {
    std::cerr << g_failures << " Snappy pose test(s) failed\n";
    return 1;
  }
  std::cout << "multiplayer Snappy pose tests passed\n";
  return 0;
}
