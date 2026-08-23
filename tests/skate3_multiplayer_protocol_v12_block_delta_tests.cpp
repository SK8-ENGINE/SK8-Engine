#include "skate3_multiplayer_protocol_v12_block_delta.h"
#include "skate3_multiplayer_protocol_v12_predictive_delta.h"
#include "skate3_multiplayer_protocol_v12_transport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <random>
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

void AppendU32(std::vector<std::uint16_t> &words, std::uint32_t value) {
  words.push_back(static_cast<std::uint16_t>(value));
  words.push_back(static_cast<std::uint16_t>(value >> 16));
}

struct StoredTrack {
  std::uint32_t mesh_key = 0;
  std::uint16_t bone_count = 0;
  std::uint16_t encoding = 0;
  std::vector<std::uint16_t> words;
};

struct Fixture {
  std::array<StoredTrack, 4> tracks;
  std::array<AnimationDeltaBaselineTrack, 4> views;

  Fixture()
      : tracks{{
            {
                .mesh_key = 0x11111111u,
                .bone_count = 17,
                .encoding = 0,
                .words = {},
            },
            {
                .mesh_key = 0x22222222u,
                .bone_count = 19,
                .encoding = 1,
                .words = {},
            },
            {
                .mesh_key = 0x33333333u,
                .bone_count = 23,
                .encoding = 2,
                .words = {},
            },
            {
                .mesh_key = 0x44444444u,
                .bone_count = 29,
                .encoding = 3,
                .words = {},
            },
        }} {
    for (std::size_t track_index = 0; track_index < tracks.size();
         ++track_index) {
      StoredTrack &track = tracks[track_index];
      const std::size_t stride = AnimationTrackWordStride(track.encoding);
      track.words.resize(std::size_t(track.bone_count) * stride);
      for (std::size_t index = 0; index < track.words.size(); ++index) {
        track.words[index] = static_cast<std::uint16_t>(
            8192u + (track_index * 1237u + index * 977u) % 49152u);
      }
      views[track_index] = {
          .mesh_key = track.mesh_key,
          .bone_count = track.bone_count,
          .encoding = track.encoding,
          .words = track.words,
      };
    }
  }
};

std::vector<std::uint16_t> BuildDelta(const Fixture &fixture,
                                      std::uint32_t seed,
                                      std::int32_t maximum_difference) {
  std::mt19937 random(seed);
  std::vector<std::uint16_t> words = {
      0,
      static_cast<std::uint16_t>(fixture.tracks.size()),
      0x5678u,
      0x1234u,
  };
  for (const StoredTrack &track : fixture.tracks) {
    AppendU32(words, track.mesh_key);
    words.push_back(track.bone_count);
    words.push_back(track.encoding);
    const std::size_t mask_words = (std::size_t(track.bone_count) + 15) / 16;
    words.push_back(static_cast<std::uint16_t>(mask_words));
    const std::size_t mask_start = words.size();
    words.resize(words.size() + mask_words, 0);
    const std::size_t stride = AnimationTrackWordStride(track.encoding);
    for (std::size_t bone = 0; bone < track.bone_count; ++bone) {
      if (bone != 0 && random() % 5u == 0) {
        continue;
      }
      words[mask_start + bone / 16] |=
          static_cast<std::uint16_t>(1u << (bone % 16));
      for (std::size_t component = 0; component < stride; ++component) {
        const std::int32_t baseline = track.words[bone * stride + component];
        const std::uint32_t range =
            static_cast<std::uint32_t>(maximum_difference * 2 + 1);
        const std::int32_t difference =
            static_cast<std::int32_t>(random() % range) - maximum_difference;
        words.push_back(static_cast<std::uint16_t>(
            std::clamp(baseline + difference, 0, 65535)));
      }
    }
  }
  return words;
}

std::vector<std::uint8_t> Encode(const Fixture &fixture,
                                 std::span<const std::uint16_t> words) {
  const float root[3] = {12.5f, -87.25f, 0.125f};
  const std::size_t byte_count =
      BlockPackedAnimationDeltaByteCount(words, fixture.views);
  std::vector<std::uint8_t> output(byte_count);
  Expect(byte_count != 0, "valid block delta had zero encoded size");
  Expect(
      EncodeBlockPackedAnimationDelta(root, 31, words, fixture.views, output),
      "valid block delta did not encode");
  return output;
}

void ExpectRoundTrip(const Fixture &fixture,
                     std::span<const std::uint16_t> expected,
                     std::span<const std::uint8_t> encoded) {
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> decoded;
  Expect(DecodeBlockPackedAnimationDelta(encoded, fixture.views, root,
                                         root_bone, decoded),
         "valid block delta did not decode");
  Expect(decoded.size() == expected.size() &&
             std::equal(decoded.begin(), decoded.end(), expected.begin(),
                        expected.end()),
         "block delta changed exact animation words");
  Expect(root[0] == 12.5f && root[1] == -87.25f && root[2] == 0.125f &&
             root_bone == 31,
         "block delta changed root metadata");
}

std::vector<std::uint8_t> EncodePredictive(
    const Fixture& fixture,
    std::span<const std::uint16_t> words) {
  const float root[3] = {12.5f, -87.25f, 0.125f};
  const std::size_t byte_count =
      PredictiveAnimationDeltaByteCount(
          words, fixture.views);
  std::vector<std::uint8_t> output(byte_count);
  Expect(byte_count != 0,
         "valid predictive delta had zero encoded size");
  Expect(EncodePredictiveAnimationDelta(
             root, 31, words, fixture.views, output),
         "valid predictive delta did not encode");
  return output;
}

void ExpectPredictiveRoundTrip(
    const Fixture& fixture,
    std::span<const std::uint16_t> expected,
    std::span<const std::uint8_t> encoded) {
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> decoded;
  Expect(DecodePredictiveAnimationDelta(
             encoded, fixture.views, root, root_bone, decoded),
         "valid predictive delta did not decode");
  Expect(decoded.size() == expected.size() &&
             std::equal(
                 decoded.begin(), decoded.end(),
                 expected.begin(), expected.end()),
         "predictive delta changed exact animation words");
  Expect(root[0] == 12.5f && root[1] == -87.25f &&
             root[2] == 0.125f && root_bone == 31,
         "predictive delta changed root metadata");
}

std::vector<std::uint16_t> BuildCoherentDelta(
    const Fixture& fixture) {
  std::vector<std::uint16_t> words = {
      0,
      static_cast<std::uint16_t>(fixture.tracks.size()),
      0x5678u,
      0x1234u,
  };
  for (const StoredTrack& track : fixture.tracks) {
    AppendU32(words, track.mesh_key);
    words.push_back(track.bone_count);
    words.push_back(track.encoding);
    const std::size_t mask_count =
        (std::size_t(track.bone_count) + 15) / 16;
    words.push_back(static_cast<std::uint16_t>(mask_count));
    words.insert(words.end(), mask_count, UINT16_MAX);
    const std::size_t valid_last_bits =
        std::size_t(track.bone_count) % 16;
    if (valid_last_bits != 0) {
      words[words.size() - 1] = static_cast<std::uint16_t>(
          (1u << valid_last_bits) - 1u);
    }
    const std::size_t stride =
        AnimationTrackWordStride(track.encoding);
    for (std::size_t bone = 0; bone < track.bone_count; ++bone) {
      for (std::size_t component = 0;
           component < stride; ++component) {
        const std::int32_t baseline =
            track.words[bone * stride + component];
        const std::int32_t difference =
            1200 + std::int32_t(component) * 31 +
            std::int32_t(bone) * 3;
        words.push_back(static_cast<std::uint16_t>(
            std::clamp(
                baseline + difference, 0, 65535)));
      }
    }
  }
  return words;
}

void TestExactRoundTripAndSavings() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words = BuildDelta(fixture, 1, 4095);
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  ExpectRoundTrip(fixture, words, encoded);
  const std::size_t semantic =
      SemanticAnimationDeltaByteCount(words, fixture.views);
  Expect(encoded.size() * 100 < semantic * 90,
         "block packing saved less than ten percent on "
         "two-byte semantic differences");
}

void TestDifferenceExtremes() {
  Fixture fixture;
  std::fill(fixture.tracks[0].words.begin(), fixture.tracks[0].words.end(), 0);
  std::fill(fixture.tracks[1].words.begin(), fixture.tracks[1].words.end(),
            UINT16_MAX);
  fixture.views[0].words = fixture.tracks[0].words;
  fixture.views[1].words = fixture.tracks[1].words;
  std::vector<std::uint16_t> words = BuildDelta(fixture, 2, 1);
  const std::size_t first_values = 4 + 5 + 2;
  words[first_values] = UINT16_MAX;
  const std::size_t first_mask_start = 4 + 5;
  std::size_t first_changed_bones = 0;
  for (std::size_t index = 0; index < 2; ++index) {
    first_changed_bones += std::popcount(words[first_mask_start + index]);
  }
  const std::size_t first_track_words =
      first_changed_bones *
      AnimationTrackWordStride(fixture.tracks[0].encoding);
  const std::size_t second_values = first_values + first_track_words + 5 + 2;
  words[second_values] = 0;
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  ExpectRoundTrip(fixture, words, encoded);
}

void TestBlockPrimitiveCanonicalForm() {
  const std::array<std::uint32_t, 5> values = {
      0, 1, 31, 17, 3,
  };
  const std::uint8_t width = block_delta_detail::RequiredBitWidth(values);
  std::vector<std::uint8_t> packed(
      block_delta_detail::PackedBlockBytes(values.size(), width));
  Expect(width == 5 && block_delta_detail::PackBlock(values, width, packed),
         "block primitive did not pack");
  std::array<std::uint32_t, 5> decoded{};
  Expect(block_delta_detail::UnpackBlock(packed, width, decoded) &&
             decoded == values,
         "block primitive changed values");
  packed.back() |= 0x80u;
  Expect(!block_delta_detail::UnpackBlock(packed, width, decoded),
         "block primitive accepted nonzero padding");

  const std::array<std::uint32_t, 4> zeros{};
  std::span<std::uint8_t> no_bytes;
  Expect(block_delta_detail::PackBlock(zeros, 0, no_bytes),
         "zero-width block did not encode");
}

void TestPredictivePrimitiveAndSavings() {
  std::array<std::int32_t, 32> coherent{};
  for (std::size_t index = 0; index < coherent.size(); ++index) {
    coherent[index] =
        12000 + static_cast<std::int32_t>(index) * 3;
  }
  std::array<std::uint32_t, 32> encoded{};
  const auto choice =
      predictive_delta_detail::ChooseBlock(
          coherent, encoded);
  Expect(choice.predictive && choice.width <= 15,
         "coherent block did not select its exact predictor");

  const Fixture fixture;
  const std::vector<std::uint16_t> words =
      BuildCoherentDelta(fixture);
  const std::vector<std::uint8_t> predictive =
      EncodePredictive(fixture, words);
  const std::size_t block =
      BlockPackedAnimationDeltaByteCount(
          words, fixture.views);
  ExpectPredictiveRoundTrip(fixture, words, predictive);
  Expect(predictive.size() * 100 < block * 70,
         "lane predictor saved less than thirty percent "
         "on coherent exact transforms");
}

void TestPredictiveMalformedTransactionalDecode() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words =
      BuildDelta(fixture, 0x12345678u, 4095);
  const std::vector<std::uint8_t> encoded =
      EncodePredictive(fixture, words);
  for (std::size_t size = 0; size < encoded.size(); ++size) {
    float root[3] = {9.0f, 8.0f, 7.0f};
    std::uint16_t root_bone = 123;
    std::vector<std::uint16_t> output = {4, 5, 6};
    Expect(!DecodePredictiveAnimationDelta(
               std::span<const std::uint8_t>(encoded).first(size),
               fixture.views, root, root_bone, output),
           "truncated predictive delta was accepted");
    Expect(root[0] == 9.0f && root[1] == 8.0f &&
               root[2] == 7.0f && root_bone == 123 &&
               output == std::vector<std::uint16_t>({4, 5, 6}),
           "failed predictive decode modified output");
  }
  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0);
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> output;
  Expect(!DecodePredictiveAnimationDelta(
             trailing, fixture.views, root, root_bone, output),
         "predictive delta accepted trailing data");

  Fixture wrong = fixture;
  wrong.views[0].mesh_key ^= 1u;
  Expect(!DecodePredictiveAnimationDelta(
             encoded, wrong.views, root, root_bone, output),
         "predictive delta accepted a different baseline layout");

  std::vector<std::uint8_t> reserved = encoded;
  const std::size_t first_mask_words =
      (std::size_t(fixture.tracks[0].bone_count) + 15) / 16;
  const std::size_t first_block_header =
      kAnimationWordStreamHeaderBytes + 8 + 10 + first_mask_words * 2;
  reserved[first_block_header] |= kPredictiveDeltaReservedMask;
  Expect(!DecodePredictiveAnimationDelta(
             reserved, fixture.views, root, root_bone, output),
         "predictive delta accepted reserved header bits");
}

void TestPredictivePropertyRoundTrips() {
  const Fixture fixture;
  for (std::uint32_t seed = 0; seed < 3000; ++seed) {
    const std::int32_t maximum =
        seed % 4 == 0 ? 31
        : seed % 4 == 1 ? 4095
        : seed % 4 == 2 ? 16383
                        : 32767;
    const std::vector<std::uint16_t> words =
        BuildDelta(
            fixture, seed ^ 0xA511E9B3u, maximum);
    const std::vector<std::uint8_t> encoded =
        EncodePredictive(fixture, words);
    ExpectPredictiveRoundTrip(fixture, words, encoded);
  }
}

void TestMalformedTransactionalDecode() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words = BuildDelta(fixture, 3, 255);
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  for (std::size_t size = 0; size < encoded.size(); ++size) {
    float root[3] = {9.0f, 8.0f, 7.0f};
    std::uint16_t root_bone = 123;
    std::vector<std::uint16_t> output = {4, 5, 6};
    Expect(!DecodeBlockPackedAnimationDelta(
               std::span<const std::uint8_t>(encoded).first(size),
               fixture.views, root, root_bone, output),
           "truncated block delta was accepted");
    Expect(root[0] == 9.0f && root[1] == 8.0f && root[2] == 7.0f &&
               root_bone == 123 &&
               output == std::vector<std::uint16_t>({4, 5, 6}),
           "failed block decode modified output");
  }
  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0);
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> output;
  Expect(!DecodeBlockPackedAnimationDelta(trailing, fixture.views, root,
                                          root_bone, output),
         "block delta accepted trailing data");

  Fixture wrong = fixture;
  wrong.views[0].mesh_key ^= 1u;
  Expect(!DecodeBlockPackedAnimationDelta(encoded, wrong.views, root, root_bone,
                                          output),
         "block delta accepted a different baseline layout");
}

void TestDeterministicPropertyRoundTrips() {
  const Fixture fixture;
  for (std::uint32_t seed = 0; seed < 3000; ++seed) {
    const std::int32_t maximum = seed % 4 == 0   ? 31
                                 : seed % 4 == 1 ? 4095
                                 : seed % 4 == 2 ? 16383
                                                 : 32767;
    const std::vector<std::uint16_t> words =
        BuildDelta(fixture, seed ^ 0x9E3779B9u, maximum);
    const std::vector<std::uint8_t> encoded = Encode(fixture, words);
    ExpectRoundTrip(fixture, words, encoded);
  }
}

void TestReverseFragmentReassembly() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words =
      BuildDelta(fixture, 0xC001D00Du, 32767);
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  Expect(encoded.size() > kMaximumPoseFragmentBytes,
         "block delta packet test did not span fragments");

  PoseGroupPacketizeRequest request;
  request.envelope.kind = MessageKind::kPoseDelta;
  request.envelope.sender_role = 2;
  request.envelope.stream_id = 2;
  request.envelope.sender_session = 100;
  request.envelope.sequence = 500;
  request.envelope.sender_time_us = 1000000;
  request.pose_id = 10;
  request.baseline_id = 9;
  request.element_count = static_cast<std::uint16_t>(fixture.tracks.size());
  request.group_id = 0;
  request.encoding = PoseGroupEncoding::kBlockDeltaV1;
  request.group_bytes = encoded;
  std::array<PoseGroupDatagram, 58> descriptors{};
  const std::size_t count = BuildPoseGroupDatagrams(request, descriptors);
  Expect(count > 1, "block delta did not packetize");

  PoseGroupReassembler reassembler;
  std::vector<std::uint8_t> reconstructed;
  for (std::size_t reverse = count; reverse-- > 0;) {
    const PoseGroupDatagram &descriptor = descriptors[reverse];
    std::vector<std::uint8_t> datagram(kEnvelopeBytes +
                                       descriptor.envelope.payload_bytes);
    Expect(EncodePoseGroupDatagram(descriptor, encoded, datagram),
           "block delta fragment did not encode");
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(datagram, envelope, header, fragment),
           "block delta fragment did not decode");
    const ReassemblyPushResult result =
        reassembler.Push(envelope, header, fragment, 2000 + reverse);
    if (result.completed.has_value()) {
      Expect(result.completed->encoding == PoseGroupEncoding::kBlockDeltaV1,
             "reassembly changed block delta encoding");
      reconstructed = result.completed->bytes;
    }
  }
  ExpectRoundTrip(fixture, words, reconstructed);
}

void TestPredictiveReverseFragmentReassembly() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words =
      BuildDelta(fixture, 0x51A7E123u, 32767);
  const std::vector<std::uint8_t> encoded =
      EncodePredictive(fixture, words);
  Expect(encoded.size() > kMaximumPoseFragmentBytes,
         "predictive packet test did not span fragments");

  PoseGroupPacketizeRequest request;
  request.envelope.kind = MessageKind::kPoseDelta;
  request.envelope.sender_role = 2;
  request.envelope.stream_id = 2;
  request.envelope.sender_session = 100;
  request.envelope.sequence = 500;
  request.envelope.sender_time_us = 1000000;
  request.pose_id = 10;
  request.baseline_id = 9;
  request.element_count =
      static_cast<std::uint16_t>(fixture.tracks.size());
  request.group_id = 0;
  request.encoding = PoseGroupEncoding::kPredictiveDeltaV1;
  request.group_bytes = encoded;
  std::array<PoseGroupDatagram, 58> descriptors{};
  const std::size_t count =
      BuildPoseGroupDatagrams(request, descriptors);
  Expect(count > 1, "predictive delta did not packetize");

  PoseGroupReassembler reassembler;
  std::vector<std::uint8_t> reconstructed;
  for (std::size_t reverse = count; reverse-- > 0;) {
    const PoseGroupDatagram& descriptor = descriptors[reverse];
    std::vector<std::uint8_t> datagram(
        kEnvelopeBytes + descriptor.envelope.payload_bytes);
    Expect(EncodePoseGroupDatagram(
               descriptor, encoded, datagram),
           "predictive fragment did not encode");
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(
               datagram, envelope, header, fragment),
           "predictive fragment did not decode");
    const ReassemblyPushResult result =
        reassembler.Push(
            envelope, header, fragment, 3000 + reverse);
    if (result.completed.has_value()) {
      Expect(result.completed->encoding ==
                 PoseGroupEncoding::kPredictiveDeltaV1,
             "reassembly changed predictive delta encoding");
      reconstructed = result.completed->bytes;
    }
  }
  ExpectPredictiveRoundTrip(
      fixture, words, reconstructed);
}

} // namespace

int main() {
  TestExactRoundTripAndSavings();
  TestDifferenceExtremes();
  TestBlockPrimitiveCanonicalForm();
  TestPredictivePrimitiveAndSavings();
  TestMalformedTransactionalDecode();
  TestPredictiveMalformedTransactionalDecode();
  TestDeterministicPropertyRoundTrips();
  TestPredictivePropertyRoundTrips();
  TestReverseFragmentReassembly();
  TestPredictiveReverseFragmentReassembly();

  if (g_failures != 0) {
    std::cerr << g_failures << " block animation delta test(s) failed\n";
    return 1;
  }
  std::cout << "All block animation delta tests passed\n";
  return 0;
}
