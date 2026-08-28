#include "skate3_multiplayer_protocol_v12_map_edit.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace protocol = skate3::multiplayer::protocol_v12;

namespace {

void Expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "MAP_EDIT_PROTOCOL_TEST_FAIL " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

protocol::Envelope MakeEnvelope(protocol::MessageKind kind,
                                std::uint16_t payload_bytes,
                                std::uint32_t sequence) {
  protocol::Envelope envelope;
  envelope.kind = kind;
  envelope.flags = kind == protocol::MessageKind::kMapEditSpawnChunk
                       ? protocol::kFlagReliable
                       : protocol::kFlagReliable;
  envelope.payload_bytes = payload_bytes;
  envelope.sender_role = 2;
  envelope.stream_id = protocol::kMapEditStreamId;
  envelope.sender_session = 0x12345678u;
  envelope.sequence = sequence;
  envelope.sender_time_us = 1000 + sequence;
  return envelope;
}

void TestControlRoundTrip() {
  protocol::MapEditControl source;
  source.type = protocol::MapEditControlType::kTransformCommitRequest;
  source.source_role = 2;
  source.object_id = 91;
  source.request_id = 42;
  source.translation[0] = 1.25f;
  source.translation[1] = -2.5f;
  source.translation[2] = 3.75f;
  source.basis[0] = 0.0f;
  source.basis[1] = 0.0f;
  source.basis[2] = 1.0f;
  source.basis[3] = 0.0f;
  source.basis[4] = 1.0f;
  source.basis[5] = 0.0f;
  source.basis[6] = -1.0f;
  source.basis[7] = 0.0f;
  source.basis[8] = 0.0f;

  std::array<std::uint8_t, protocol::kMapEditControlPayloadBytes> bytes{};
  Expect(protocol::EncodeMapEditControl(source, bytes),
         "control encode failed");
  protocol::MapEditControl decoded;
  Expect(protocol::DecodeMapEditControl(bytes, decoded),
         "control decode failed");
  Expect(decoded.type == source.type, "control type changed");
  Expect(decoded.source_role == source.source_role,
         "control source role changed");
  Expect(decoded.object_id == source.object_id, "control object id changed");
  Expect(decoded.request_id == source.request_id, "control request id changed");
  Expect(std::equal(std::begin(decoded.translation),
                    std::end(decoded.translation),
                    std::begin(source.translation)),
         "control translation changed");
  Expect(std::equal(std::begin(decoded.basis), std::end(decoded.basis),
                    std::begin(source.basis)),
         "control basis changed");
}

void TestSpawnFragmentRoundTripAndReassembly() {
  std::vector<std::uint8_t> package(protocol::kMapEditSpawnFragmentBytes * 2 +
                                    37);
  for (std::size_t index = 0; index < package.size(); ++index) {
    package[index] = static_cast<std::uint8_t>((index * 37u + 11u) & 0xFFu);
  }
  const std::uint32_t fragment_count = protocol::MapEditSpawnFragmentCount(
      static_cast<std::uint32_t>(package.size()));
  Expect(fragment_count == 3, "unexpected fragment count");
  const std::uint64_t hash = protocol::MapEditContentHash(package);

  struct EncodedFragment {
    protocol::Envelope envelope;
    protocol::MapEditSpawnHeader header;
    std::vector<std::uint8_t> packet;
  };
  std::vector<EncodedFragment> fragments;
  for (std::uint32_t index = 0; index < fragment_count; ++index) {
    EncodedFragment fragment;
    fragment.header.type = protocol::MapEditSpawnType::kSpawnRequest;
    fragment.header.source_role = 2;
    fragment.header.request_id = 77;
    fragment.header.total_bytes = static_cast<std::uint32_t>(package.size());
    fragment.header.fragment_index = index;
    fragment.header.fragment_count = fragment_count;
    fragment.header.fragment_offset =
        protocol::MapEditSpawnFragmentOffset(index);
    fragment.header.fragment_bytes = protocol::MapEditSpawnFragmentByteCount(
        fragment.header.total_bytes, index);
    fragment.header.content_hash = hash;
    fragment.header.position[0] = 10.0f;
    fragment.header.position[1] = 20.0f;
    fragment.header.position[2] = -30.0f;
    fragment.envelope = MakeEnvelope(
        protocol::MessageKind::kMapEditSpawnChunk,
        static_cast<std::uint16_t>(protocol::kMapEditSpawnHeaderBytes +
                                   fragment.header.fragment_bytes),
        100 + index);
    fragment.packet.resize(protocol::kEnvelopeBytes +
                           fragment.envelope.payload_bytes);
    Expect(protocol::EncodeMapEditSpawnDatagram(
               fragment.envelope, fragment.header, package, fragment.packet),
           "spawn datagram encode failed");
    fragments.push_back(std::move(fragment));
  }

  protocol::MapEditSpawnReassembler reassembler;
  std::optional<protocol::ReassembledMapEditSpawn> completed;
  for (const std::size_t index : {2u, 0u, 1u}) {
    protocol::Envelope envelope;
    protocol::MapEditSpawnHeader header;
    std::span<const std::uint8_t> bytes;
    Expect(protocol::DecodeMapEditSpawnDatagram(fragments[index].packet,
                                                envelope, header, bytes),
           "spawn datagram decode failed");
    const protocol::MapEditReassemblyResult result =
        reassembler.Push(envelope, header, bytes, 10000 + index);
    if (result.completed.has_value()) {
      completed = result.completed;
    }
  }
  Expect(completed.has_value(), "spawn did not complete");
  Expect(completed->package == package, "spawn bytes changed");
  Expect(completed->header.request_id == 77, "spawn request id changed");
  Expect(completed->header.source_role == 2, "spawn source role changed");
}

void TestCorruptPackageHashRejected() {
  std::vector<std::uint8_t> package(32, 0xAA);
  protocol::MapEditSpawnHeader header;
  header.type = protocol::MapEditSpawnType::kSpawnApply;
  header.source_role = 1;
  header.request_id = 1;
  header.authority_revision = 1;
  header.total_bytes = static_cast<std::uint32_t>(package.size());
  header.fragment_count = 1;
  header.fragment_bytes = static_cast<std::uint16_t>(package.size());
  header.content_hash = protocol::MapEditContentHash(package) + 1;
  protocol::Envelope envelope =
      MakeEnvelope(protocol::MessageKind::kMapEditSpawnChunk,
                   static_cast<std::uint16_t>(
                       protocol::kMapEditSpawnHeaderBytes + package.size()),
                   1);
  protocol::MapEditSpawnReassembler reassembler;
  const protocol::MapEditReassemblyResult result =
      reassembler.Push(envelope, header, package, 1);
  Expect(result.disposition ==
             protocol::MapEditReassemblyDisposition::kConflicting,
         "corrupt package hash was accepted");
  Expect(!result.completed.has_value(), "corrupt package completed");
}

} // namespace

int main() {
  TestControlRoundTrip();
  TestSpawnFragmentRoundTripAndReassembly();
  TestCorruptPackageHashRejected();
  static_assert(
      (protocol::kKnownFeatureBits & protocol::kFeatureLiveMapEditing) != 0);
  return 0;
}
