#include "skate3_multiplayer_protocol.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

using skate3::multiplayer::protocol::AnimationFragmentByteCount;
using skate3::multiplayer::protocol::AnimationFragmentPacket;
using skate3::multiplayer::protocol::AnimationFragmentShapeValid;
using skate3::multiplayer::protocol::AppearanceFragmentByteCount;
using skate3::multiplayer::protocol::AppearanceFragmentPacket;
using skate3::multiplayer::protocol::AppearanceFragmentShapeValid;
using skate3::multiplayer::protocol::AppearanceDeliveryState;
using skate3::multiplayer::protocol::AppearanceTransferReceived;
using skate3::multiplayer::protocol::AppearanceStateProgresses;
using skate3::multiplayer::protocol::ControlMessageType;
using skate3::multiplayer::protocol::ControlPacket;
using skate3::multiplayer::protocol::ControlPacketShapeValid;
using skate3::multiplayer::protocol::PosePacket;
using skate3::multiplayer::protocol::SequenceNewer;
using skate3::multiplayer::protocol::SequenceNewerOrEqual;
using skate3::multiplayer::protocol::SequenceOlder;
using skate3::multiplayer::protocol::kAnimationFragmentWords;
using skate3::multiplayer::protocol::kAnimationPacketMagic;
using skate3::multiplayer::protocol::kAppearanceChunkBytes;
using skate3::multiplayer::protocol::kAppearancePacketMagic;
using skate3::multiplayer::protocol::kCapabilityAppearanceRequest;
using skate3::multiplayer::protocol::kCapabilityAppearanceState;
using skate3::multiplayer::protocol::kCapabilityControlV1;
using skate3::multiplayer::protocol::kControlPacketMagic;
using skate3::multiplayer::protocol::kMaximumAnimationFragments;
using skate3::multiplayer::protocol::kMaximumAnimationFrameWords;
using skate3::multiplayer::protocol::kMaximumAppearanceBytes;
using skate3::multiplayer::protocol::kPacketMagic;
using skate3::multiplayer::protocol::kProtocolVersion;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

template <std::size_t Size>
std::array<std::uint8_t, Size> BytesOf(const auto& value) {
  static_assert(sizeof(value) == Size);
  std::array<std::uint8_t, Size> bytes{};
  std::memcpy(bytes.data(), &value, Size);
  return bytes;
}

void TestWireLayout() {
  Expect(std::endian::native == std::endian::little,
         "protocol-v11 golden bytes require a little-endian host");
  Expect(kProtocolVersion == 11, "protocol version must remain 11");
  Expect(kPacketMagic == 0x504D334Bu, "root magic changed");
  Expect(kAnimationPacketMagic == 0x414D334Bu,
         "animation magic changed");
  Expect(kAppearancePacketMagic == 0x5041334Bu,
         "appearance magic changed");
  Expect(kControlPacketMagic == 0x434D334Bu,
         "control magic changed");
  Expect(sizeof(PosePacket) == 72, "root packet size changed");
  Expect(offsetof(AnimationFragmentPacket, words) == 56,
         "animation payload offset changed");
  Expect(sizeof(AnimationFragmentPacket) == 1096,
         "animation packet capacity changed");
  Expect(offsetof(AppearanceFragmentPacket, bytes) == 38,
         "appearance payload offset changed");
  Expect(sizeof(AppearanceFragmentPacket) == 1062,
         "appearance packet capacity changed");
  Expect(sizeof(ControlPacket) == 40,
         "control packet size changed");
  Expect(kMaximumAnimationFragments == 16,
         "animation fragment limit changed");
  Expect(kMaximumAnimationFrameWords == 8192,
         "animation frame word limit changed");
}

void TestPoseGoldenBytes() {
  PosePacket packet;
  packet.sender_role = 0x01020304u;
  packet.sender_session = 0x05060708u;
  packet.sequence = 0x090A0B0Cu;
  packet.map_hash = 0xA1B2C3D4u;
  packet.sender_time_us = 0x0102030405060708ull;
  packet.position[0] = 1.0f;
  packet.position[1] = -2.0f;
  packet.position[2] = 0.5f;
  packet.x_axis[0] = 1.0f;
  packet.x_axis[1] = 0.0f;
  packet.x_axis[2] = 0.0f;
  packet.z_axis[0] = 0.0f;
  packet.z_axis[1] = 0.0f;
  packet.z_axis[2] = 1.0f;
  packet.board_state_flags = 0x11223344u;

  constexpr std::array<std::uint8_t, 72> expected = {
      0x4B, 0x33, 0x4D, 0x50, 0x0B, 0x00, 0x48, 0x00,
      0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05,
      0x0C, 0x0B, 0x0A, 0x09, 0xD4, 0xC3, 0xB2, 0xA1,
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xC0,
      0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x80, 0x3F,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x80, 0x3F, 0x44, 0x33, 0x22, 0x11,
  };

  Expect(BytesOf<72>(packet) == expected,
         "root packet no longer matches the protocol-v11 golden bytes");
}

void TestAnimationGoldenBytes() {
  AnimationFragmentPacket packet;
  packet.byte_count =
      static_cast<std::uint16_t>(AnimationFragmentByteCount(2));
  packet.sender_role = 0x01020304u;
  packet.sender_session = 0x05060708u;
  packet.sequence = 0x090A0B0Cu;
  packet.map_hash = 0xA1B2C3D4u;
  packet.sender_time_us = 0x0102030405060708ull;
  packet.root_position[0] = 1.0f;
  packet.root_position[1] = -2.0f;
  packet.root_position[2] = 0.5f;
  packet.root_bone = 0x1234u;
  packet.fragment_index = 1;
  packet.fragment_count = 2;
  packet.word_offset = 520;
  packet.total_words = 522;
  packet.word_count = 2;
  packet.words[0] = 0x1122u;
  packet.words[1] = 0x3344u;

  constexpr std::array<std::uint8_t, 60> expected = {
      0x4B, 0x33, 0x4D, 0x41, 0x0B, 0x00, 0x3C, 0x00,
      0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05,
      0x0C, 0x0B, 0x0A, 0x09, 0xD4, 0xC3, 0xB2, 0xA1,
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xC0,
      0x00, 0x00, 0x00, 0x3F, 0x34, 0x12, 0x01, 0x00,
      0x02, 0x00, 0x08, 0x02, 0x0A, 0x02, 0x02, 0x00,
      0x22, 0x11, 0x44, 0x33,
  };

  std::array<std::uint8_t, 60> actual{};
  std::memcpy(actual.data(), &packet, actual.size());
  Expect(actual == expected,
         "animation fragment no longer matches the v11 golden bytes");
}

void TestAppearanceGoldenBytes() {
  AppearanceFragmentPacket packet;
  packet.byte_count =
      static_cast<std::uint16_t>(AppearanceFragmentByteCount(3));
  packet.sender_role = 0x01020304u;
  packet.sender_session = 0x05060708u;
  packet.map_hash = 0xA1B2C3D4u;
  packet.appearance_id = 0x0102030405060708ull;
  packet.total_bytes = 0x11223344u;
  packet.chunk_index = 2;
  packet.chunk_count = 4;
  packet.chunk_bytes = 3;
  packet.bytes[0] = 0xAA;
  packet.bytes[1] = 0xBB;
  packet.bytes[2] = 0xCC;

  constexpr std::array<std::uint8_t, 41> expected = {
      0x4B, 0x33, 0x41, 0x50, 0x0B, 0x00, 0x29, 0x00,
      0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05,
      0xD4, 0xC3, 0xB2, 0xA1, 0x08, 0x07, 0x06, 0x05,
      0x04, 0x03, 0x02, 0x01, 0x44, 0x33, 0x22, 0x11,
      0x02, 0x00, 0x04, 0x00, 0x03, 0x00, 0xAA, 0xBB,
      0xCC,
  };

  std::array<std::uint8_t, 41> actual{};
  std::memcpy(actual.data(), &packet, actual.size());
  Expect(actual == expected,
         "appearance fragment no longer matches the v11 golden bytes");
}

void TestControlGoldenBytes() {
  ControlPacket packet;
  packet.sender_role = 0x01020304u;
  packet.sender_session = 0x05060708u;
  packet.target_role = 0x090A0B0Cu;
  packet.map_hash = 0xA1B2C3D4u;
  packet.capabilities = 0x11223344u;
  packet.appearance_id = 0x0102030405060708ull;

  constexpr std::array<std::uint8_t, 40> expected = {
      0x4B, 0x33, 0x4D, 0x43, 0x0B, 0x00, 0x28, 0x00,
      0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05,
      0x0C, 0x0B, 0x0A, 0x09, 0xD4, 0xC3, 0xB2, 0xA1,
      0x01, 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
  };

  Expect(BytesOf<40>(packet) == expected,
         "control packet no longer matches its golden bytes");
}

void TestVariablePacketSizes() {
  Expect(AnimationFragmentByteCount(0) == 56,
         "empty animation fragment size changed");
  Expect(AnimationFragmentByteCount(1) == 58,
         "one-word animation fragment size changed");
  Expect(AnimationFragmentByteCount(kAnimationFragmentWords) == 1096,
         "full animation fragment size changed");

  Expect(AppearanceFragmentByteCount(0) == 38,
         "empty appearance fragment size changed");
  Expect(AppearanceFragmentByteCount(1) == 39,
         "one-byte appearance fragment size changed");
  Expect(AppearanceFragmentByteCount(kAppearanceChunkBytes) == 1062,
         "full appearance fragment size changed");
}

void TestAnimationFragmentShapes() {
  AnimationFragmentPacket packet;
  packet.total_words = 522;
  packet.fragment_count = 2;
  packet.fragment_index = 0;
  packet.word_offset = 0;
  packet.word_count = kAnimationFragmentWords;
  Expect(AnimationFragmentShapeValid(packet),
         "full non-final animation fragment was rejected");

  packet.fragment_index = 1;
  packet.word_offset = kAnimationFragmentWords;
  packet.word_count = 2;
  Expect(AnimationFragmentShapeValid(packet),
         "short final animation fragment was rejected");

  packet.fragment_index = 0;
  packet.word_offset = 0;
  packet.word_count = kAnimationFragmentWords - 1;
  Expect(!AnimationFragmentShapeValid(packet),
         "undersized non-final fragment left a zero-filled pose gap");

  packet.fragment_index = 1;
  packet.word_offset = kAnimationFragmentWords;
  packet.word_count = 3;
  Expect(!AnimationFragmentShapeValid(packet),
         "oversized final animation fragment was accepted");

  packet.word_count = 2;
  packet.word_offset = kAnimationFragmentWords - 1;
  Expect(!AnimationFragmentShapeValid(packet),
         "animation fragment with a shifted offset was accepted");

  packet.word_offset = kAnimationFragmentWords;
  packet.fragment_count = 3;
  Expect(!AnimationFragmentShapeValid(packet),
         "animation fragment with an inconsistent count was accepted");

  packet.fragment_count = 2;
  packet.fragment_index = 2;
  Expect(!AnimationFragmentShapeValid(packet),
         "out-of-range animation fragment index was accepted");

  packet = {};
  Expect(!AnimationFragmentShapeValid(packet),
         "empty animation frame was accepted");
  packet.total_words = kMaximumAnimationFrameWords + 1;
  Expect(!AnimationFragmentShapeValid(packet),
         "oversized animation frame was accepted");
}

void TestAppearanceFragmentShapes() {
  AppearanceFragmentPacket packet;
  packet.total_bytes = kAppearanceChunkBytes + 1;
  packet.chunk_count = 2;
  packet.chunk_index = 0;
  packet.chunk_bytes = kAppearanceChunkBytes;
  Expect(AppearanceFragmentShapeValid(packet),
         "full non-final appearance chunk was rejected");

  packet.chunk_index = 1;
  packet.chunk_bytes = 1;
  Expect(AppearanceFragmentShapeValid(packet),
         "short final appearance chunk was rejected");

  packet.chunk_index = 0;
  packet.chunk_bytes = kAppearanceChunkBytes - 1;
  Expect(!AppearanceFragmentShapeValid(packet),
         "undersized non-final appearance chunk was accepted");

  packet.chunk_index = 1;
  packet.chunk_bytes = 2;
  Expect(!AppearanceFragmentShapeValid(packet),
         "oversized final appearance chunk was accepted");

  packet.chunk_bytes = 1;
  packet.chunk_count = 3;
  Expect(!AppearanceFragmentShapeValid(packet),
         "appearance chunk with an inconsistent count was accepted");

  packet.chunk_count = 2;
  packet.chunk_index = 2;
  Expect(!AppearanceFragmentShapeValid(packet),
         "out-of-range appearance chunk index was accepted");

  packet = {};
  Expect(!AppearanceFragmentShapeValid(packet),
         "empty appearance payload was accepted");
  packet.total_bytes = kMaximumAppearanceBytes + 1;
  packet.chunk_count = static_cast<std::uint16_t>(
      (packet.total_bytes + kAppearanceChunkBytes - 1) /
      kAppearanceChunkBytes);
  packet.chunk_bytes = kAppearanceChunkBytes;
  Expect(!AppearanceFragmentShapeValid(packet),
         "oversized appearance payload was accepted");
}

void TestControlPacketShapes() {
  ControlPacket packet;
  packet.sender_role = 1;
  packet.sender_session = 100;
  packet.target_role = 2;
  packet.map_hash = 200;
  packet.capabilities = kCapabilityControlV1;
  Expect(ControlPacketShapeValid(packet),
         "valid capability advertisement was rejected");

  packet.target_role = packet.sender_role;
  Expect(!ControlPacketShapeValid(packet),
         "self-targeted control packet was accepted");
  packet.target_role = 2;

  packet.capabilities = 0;
  Expect(!ControlPacketShapeValid(packet),
         "control packet without the base capability was accepted");

  packet.capabilities =
      kCapabilityControlV1 | kCapabilityAppearanceState;
  packet.message_type = ControlMessageType::kAppearanceState;
  packet.appearance_state = AppearanceDeliveryState::kReceived;
  packet.appearance_id = 0x1234;
  Expect(ControlPacketShapeValid(packet),
         "valid appearance-state control packet was rejected");

  packet.appearance_state = AppearanceDeliveryState::kUnknown;
  Expect(!ControlPacketShapeValid(packet),
         "appearance state without a result was accepted");

  packet.message_type = ControlMessageType::kAppearanceRequest;
  packet.capabilities =
      kCapabilityControlV1 | kCapabilityAppearanceRequest;
  Expect(ControlPacketShapeValid(packet),
         "valid appearance request was rejected");

  packet.appearance_id = 0;
  Expect(!ControlPacketShapeValid(packet),
         "appearance request without an identity was accepted");

  Expect(
      AppearanceTransferReceived(
          AppearanceDeliveryState::kReceived),
      "received appearance state did not complete the transfer");
  Expect(
      AppearanceTransferReceived(
          AppearanceDeliveryState::kInstalled),
      "installed appearance state did not complete the transfer");
  Expect(
      !AppearanceTransferReceived(
          AppearanceDeliveryState::kFailed),
      "failed appearance state completed the transfer");
  Expect(
      AppearanceStateProgresses(
          AppearanceDeliveryState::kReceived,
          AppearanceDeliveryState::kInstalled),
      "installed state did not advance received state");
  Expect(
      !AppearanceStateProgresses(
          AppearanceDeliveryState::kInstalled,
          AppearanceDeliveryState::kReceived),
      "late received state regressed installed state");
}

void TestSequenceOrderingAcrossWrap() {
  Expect(SequenceNewer(11, 10), "ordinary newer sequence was rejected");
  Expect(SequenceOlder(10, 11), "ordinary older sequence was accepted");
  Expect(!SequenceNewer(10, 10), "equal sequence must not be newer");
  Expect(!SequenceOlder(10, 10), "equal sequence must not be older");
  Expect(SequenceNewer(0, UINT32_MAX),
         "zero must be newer immediately after uint32 rollover");
  Expect(SequenceNewer(1, UINT32_MAX),
         "post-rollover sequence was rejected");
  Expect(SequenceOlder(UINT32_MAX, 0),
         "pre-rollover sequence must be older than zero");
  Expect(!SequenceNewer(0x80000000u, 0),
         "ambiguous half-range sequence must not be newer");
  Expect(!SequenceOlder(0x80000000u, 0),
         "ambiguous half-range sequence must not be older");
  Expect(SequenceNewerOrEqual(0, 0),
         "sequence zero must be equal to an active zero reference");
  Expect(SequenceNewerOrEqual(0, UINT32_MAX),
         "rolled-over sequence zero must advance a pre-rollover reference");
  Expect(!SequenceNewerOrEqual(UINT32_MAX, 0),
         "late pre-rollover sequence must not replace active sequence zero");
}

}  // namespace

int main() {
  TestWireLayout();
  TestPoseGoldenBytes();
  TestAnimationGoldenBytes();
  TestAppearanceGoldenBytes();
  TestControlGoldenBytes();
  TestVariablePacketSizes();
  TestAnimationFragmentShapes();
  TestAppearanceFragmentShapes();
  TestControlPacketShapes();
  TestSequenceOrderingAcrossWrap();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer protocol test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer protocol-v11 tests passed\n";
  return 0;
}
