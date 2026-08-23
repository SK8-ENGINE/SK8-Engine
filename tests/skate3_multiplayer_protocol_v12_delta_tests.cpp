#include "skate3_multiplayer_protocol_v12_delta.h"
#include "skate3_multiplayer_protocol_v12_transport.h"

#include <algorithm>
#include <array>
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

struct Fixture {
  static constexpr std::uint32_t kFirstMesh = 0x10203040u;
  static constexpr std::uint32_t kSecondMesh = 0x50607080u;
  static constexpr std::uint16_t kFirstBones = 20;
  static constexpr std::uint16_t kSecondBones = 17;

  std::vector<std::uint16_t> first;
  std::vector<std::uint16_t> second;
  std::array<AnimationDeltaBaselineTrack, 2> views;

  Fixture()
      : first(std::size_t(kFirstBones) * 7),
        second(std::size_t(kSecondBones) * 12) {
    for (std::size_t index = 0; index < first.size(); ++index) {
      first[index] =
          static_cast<std::uint16_t>(20000u + (index * 193u) % 20000u);
    }
    for (std::size_t index = 0; index < second.size(); ++index) {
      second[index] =
          static_cast<std::uint16_t>(10000u + (index * 271u) % 40000u);
    }
    views = {{
        {
            .mesh_key = kFirstMesh,
            .bone_count = kFirstBones,
            .encoding = 1,
            .words = first,
        },
        {
            .mesh_key = kSecondMesh,
            .bone_count = kSecondBones,
            .encoding = 0,
            .words = second,
        },
    }};
  }
};

std::vector<std::uint16_t> BuildDelta(const Fixture &fixture,
                                      std::uint32_t seed,
                                      bool small_differences = true) {
  std::mt19937 random(seed);
  std::vector<std::uint16_t> words = {
      0,
      2,
      0x3344u,
      0x1122u,
  };
  for (std::size_t track_index = 0; track_index < fixture.views.size();
       ++track_index) {
    const AnimationDeltaBaselineTrack &baseline = fixture.views[track_index];
    AppendU32(words, baseline.mesh_key);
    words.push_back(baseline.bone_count);
    words.push_back(baseline.encoding);
    const std::size_t mask_count = (std::size_t(baseline.bone_count) + 15) / 16;
    words.push_back(static_cast<std::uint16_t>(mask_count));
    const std::size_t mask_start = words.size();
    words.resize(words.size() + mask_count, 0);
    const std::size_t stride = AnimationTrackWordStride(baseline.encoding);
    for (std::size_t bone = 0; bone < baseline.bone_count; ++bone) {
      if ((random() % 5u) == 0u && bone != 0) {
        continue;
      }
      words[mask_start + bone / 16] |=
          static_cast<std::uint16_t>(1u << (bone % 16));
      for (std::size_t component = 0; component < stride; ++component) {
        const std::uint16_t base = baseline.words[bone * stride + component];
        if (small_differences) {
          const std::int32_t offset =
              static_cast<std::int32_t>(random() % 63u) - 31;
          words.push_back(static_cast<std::uint16_t>(
              std::clamp(std::int32_t(base) + offset, 0, 65535)));
        } else {
          words.push_back(static_cast<std::uint16_t>(random()));
        }
      }
    }
  }
  return words;
}

std::vector<std::uint8_t> Encode(const Fixture &fixture,
                                 std::span<const std::uint16_t> words) {
  const float root[3] = {1.25f, -200.5f, 0.03125f};
  const std::size_t encoded_size =
      SemanticAnimationDeltaByteCount(words, fixture.views);
  std::vector<std::uint8_t> encoded(encoded_size);
  Expect(encoded_size != 0, "valid semantic delta had zero encoded size");
  Expect(EncodeSemanticAnimationDelta(root, 77, words, fixture.views, encoded),
         "valid semantic delta did not encode");
  return encoded;
}

void ExpectRoundTrip(const Fixture &fixture,
                     std::span<const std::uint16_t> expected,
                     std::span<const std::uint8_t> encoded) {
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> decoded;
  Expect(DecodeSemanticAnimationDelta(encoded, fixture.views, root, root_bone,
                                      decoded),
         "valid semantic delta did not decode");
  Expect(std::equal(decoded.begin(), decoded.end(), expected.begin(),
                    expected.end()),
         "semantic delta changed exact animation words");
  Expect(decoded.size() == expected.size(),
         "semantic delta changed animation word count");
  Expect(root[0] == 1.25f && root[1] == -200.5f && root[2] == 0.03125f &&
             root_bone == 77,
         "semantic delta changed root metadata");
}

void TestExactRoundTripAndSavings() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words = BuildDelta(fixture, 1);
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  ExpectRoundTrip(fixture, words, encoded);
  Expect(encoded.size() ==
             SemanticAnimationDeltaByteCount(words, fixture.views),
         "semantic delta preflight size was not exact");
  Expect(encoded.size() * 100 < AnimationWordStreamByteCount(words.size()) * 65,
         "small exact transform differences saved less than 35 percent");
}

void TestExactCompositionDiagnostics() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words = BuildDelta(fixture, 0xA11CEu);
  SemanticAnimationDeltaStatistics statistics;
  Expect(InspectSemanticAnimationDelta(words, fixture.views, statistics),
         "valid semantic delta diagnostics failed");
  Expect(statistics.encoded_bytes ==
             SemanticAnimationDeltaByteCount(words, fixture.views),
         "diagnostic byte total disagreed with encoder preflight");
  Expect(statistics.encoded_bytes ==
             statistics.fixed_header_bytes + statistics.track_metadata_bytes +
                 statistics.change_mask_bytes + statistics.value_bytes,
         "diagnostic byte categories did not sum to encoded size");
  Expect(statistics.component_words == statistics.varint_word_counts[0] +
                                           statistics.varint_word_counts[1] +
                                           statistics.varint_word_counts[2],
         "diagnostic varint histogram did not cover every component");
  Expect(statistics.component_words == statistics.rigid_rotation_words +
                                           statistics.affine_basis_words +
                                           statistics.translation_words,
         "diagnostic component classes did not cover every word");
  Expect(statistics.value_bytes == statistics.rigid_rotation_bytes +
                                       statistics.affine_basis_bytes +
                                       statistics.translation_bytes,
         "diagnostic component bytes did not sum to value bytes");
  Expect(statistics.track_count == 2 && statistics.fixed_header_bytes == 24 &&
             statistics.track_metadata_bytes == 20 &&
             statistics.change_mask_bytes == 8 && statistics.changed_bones != 0,
         "diagnostic fixed stream composition was incorrect");
  Expect(statistics.rigid_rotation_words != 0 &&
             statistics.affine_basis_words != 0 &&
             statistics.translation_words != 0,
         "diagnostics missed a transform component class");

  std::vector<std::uint16_t> malformed = words;
  malformed.pop_back();
  SemanticAnimationDeltaStatistics unchanged;
  unchanged.encoded_bytes = 12345;
  Expect(!InspectSemanticAnimationDelta(malformed, fixture.views, unchanged),
         "diagnostics accepted a truncated semantic delta");
  Expect(unchanged.encoded_bytes == 12345,
         "failed diagnostics modified the output");
}

void TestMaximumDifferences() {
  Fixture fixture;
  std::fill(fixture.first.begin(), fixture.first.end(), 0);
  std::fill(fixture.second.begin(), fixture.second.end(), UINT16_MAX);
  std::vector<std::uint16_t> words = BuildDelta(fixture, 2, false);
  // Force both signed-difference extremes into the stream.
  const std::size_t first_track_values = 4 + 5 + 2;
  words[first_track_values] = UINT16_MAX;
  const std::size_t second_track =
      first_track_values + std::size_t(Fixture::kFirstBones) * 7;
  const std::size_t second_track_values = second_track + 5 + 2;
  words[second_track_values] = 0;
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  ExpectRoundTrip(fixture, words, encoded);
}

void TestMalformedAndTransactionalDecode() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words = BuildDelta(fixture, 3);
  const std::vector<std::uint8_t> encoded = Encode(fixture, words);
  for (std::size_t size = 0; size < encoded.size(); ++size) {
    float root[3] = {9.0f, 8.0f, 7.0f};
    std::uint16_t root_bone = 123;
    std::vector<std::uint16_t> output = {4, 5, 6};
    Expect(!DecodeSemanticAnimationDelta(
               std::span<const std::uint8_t>(encoded).first(size),
               fixture.views, root, root_bone, output),
           "truncated semantic delta was accepted");
    Expect(root[0] == 9.0f && root[1] == 8.0f && root[2] == 7.0f &&
               root_bone == 123 &&
               output == std::vector<std::uint16_t>({4, 5, 6}),
           "failed semantic delta decode modified output");
  }

  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0);
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> output;
  Expect(!DecodeSemanticAnimationDelta(trailing, fixture.views, root, root_bone,
                                       output),
         "semantic delta with trailing data was accepted");

  Fixture wrong = fixture;
  wrong.views[0].mesh_key ^= 1u;
  Expect(!DecodeSemanticAnimationDelta(encoded, wrong.views, root, root_bone,
                                       output),
         "semantic delta accepted a different baseline layout");

  auto malformed = encoded;
  // Word count lives at byte 14 and must match the reconstructed stream.
  malformed[14] = 4;
  malformed[15] = 0;
  Expect(!DecodeSemanticAnimationDelta(malformed, fixture.views, root,
                                       root_bone, output),
         "semantic delta accepted a false decoded word count");
}

void TestInputValidation() {
  const Fixture fixture;
  std::vector<std::uint16_t> words = BuildDelta(fixture, 4);
  words[0] = 1;
  Expect(SemanticAnimationDeltaByteCount(words, fixture.views) == 0,
         "semantic delta encoder accepted a keyframe");
  words[0] = 0;
  words[1] = 1;
  Expect(SemanticAnimationDeltaByteCount(words, fixture.views) == 0,
         "semantic delta encoder accepted wrong track count");
  words[1] = 2;
  words.pop_back();
  Expect(SemanticAnimationDeltaByteCount(words, fixture.views) == 0,
         "semantic delta encoder accepted truncated words");

  Expect(AnimationPoseGroupEncodingAllowed(PoseGroupEncoding::kSemanticDeltaV1,
                                           /*keyframe=*/false),
         "sender policy rejected semantic delta encoding");
  Expect(!AnimationPoseGroupEncodingAllowed(PoseGroupEncoding::kSemanticDeltaV1,
                                            /*keyframe=*/true),
         "sender policy accepted semantic keyframe encoding");
  Expect(AnimationPoseGroupEncodingAllowed(PoseGroupEncoding::kBlockDeltaV1,
                                           /*keyframe=*/false),
         "sender policy rejected block delta encoding");
  Expect(!AnimationPoseGroupEncodingAllowed(PoseGroupEncoding::kBlockDeltaV1,
                                            /*keyframe=*/true),
         "sender policy accepted block keyframe encoding");
  Expect(AnimationPoseGroupEncodingAllowed(
             PoseGroupEncoding::kPredictiveDeltaV1,
             /*keyframe=*/false),
         "sender policy rejected predictive delta encoding");
  Expect(!AnimationPoseGroupEncodingAllowed(
             PoseGroupEncoding::kPredictiveDeltaV1,
             /*keyframe=*/true),
         "sender policy accepted predictive keyframe encoding");
  Expect(AnimationPoseGroupEncodingAllowed(PoseGroupEncoding::kV11WordStream,
                                           /*keyframe=*/true) &&
             AnimationPoseGroupEncodingAllowed(PoseGroupEncoding::kBitPackedV1,
                                               /*keyframe=*/true),
         "sender policy rejected validated keyframe encodings");
  Expect(!AnimationPoseGroupEncodingAllowed(static_cast<PoseGroupEncoding>(6),
                                            /*keyframe=*/false),
         "sender policy accepted unknown pose encoding");
}

void TestVarintCanonicalValidation() {
  const std::array<std::uint8_t, 2> noncanonical = {
      0x80u,
      0x00u,
  };
  detail::LittleEndianReader noncanonical_reader(noncanonical);
  std::uint32_t value = 0;
  Expect(!delta_detail::ReadVarint(noncanonical_reader, value),
         "noncanonical semantic varint was accepted");

  const std::array<std::uint8_t, 3> oversized = {
      0xFFu,
      0xFFu,
      0x07u,
  };
  detail::LittleEndianReader oversized_reader(oversized);
  Expect(!delta_detail::ReadVarint(oversized_reader, value),
         "semantic varint above exact delta range was accepted");

  const std::array<std::uint8_t, 3> unterminated = {
      0x80u,
      0x80u,
      0x80u,
  };
  detail::LittleEndianReader unterminated_reader(unterminated);
  Expect(!delta_detail::ReadVarint(unterminated_reader, value),
         "unterminated semantic varint was accepted");
}

void TestDeterministicPropertyRoundTrips() {
  const Fixture fixture;
  for (std::uint32_t seed = 0; seed < 2000; ++seed) {
    const std::vector<std::uint16_t> words =
        BuildDelta(fixture, 0x9E3779B9u ^ seed, (seed % 3u) != 0);
    const std::vector<std::uint8_t> encoded = Encode(fixture, words);
    ExpectRoundTrip(fixture, words, encoded);
  }
}

void TestReverseFragmentReassembly() {
  constexpr std::uint16_t bone_count = 131;
  constexpr std::uint16_t encoding = 0;
  constexpr std::size_t stride = 12;
  std::vector<std::uint16_t> baseline_words(std::size_t(bone_count) * stride,
                                            0);
  const std::array<AnimationDeltaBaselineTrack, 1> baselines = {{
      {
          .mesh_key = 0x12345678u,
          .bone_count = bone_count,
          .encoding = encoding,
          .words = baseline_words,
      },
  }};
  std::vector<std::uint16_t> words = {
      0,
      1,
      0x0009u,
      0,
  };
  AppendU32(words, baselines[0].mesh_key);
  words.push_back(bone_count);
  words.push_back(encoding);
  constexpr std::size_t mask_count = (bone_count + 15) / 16;
  words.push_back(mask_count);
  words.insert(words.end(), mask_count, UINT16_MAX);
  words[4 + 5 + mask_count - 1] = 0x0007u;
  std::uint32_t random = 0xC001D00Du;
  for (std::size_t index = 0; index < std::size_t(bone_count) * stride;
       ++index) {
    random = random * 1664525u + 1013904223u;
    words.push_back(static_cast<std::uint16_t>(random));
  }
  const float root[3] = {5.0f, 6.0f, 7.0f};
  const std::size_t byte_count =
      SemanticAnimationDeltaByteCount(words, baselines);
  std::vector<std::uint8_t> encoded(byte_count);
  Expect(EncodeSemanticAnimationDelta(root, 9, words, baselines, encoded),
         "large semantic delta did not encode");
  Expect(encoded.size() > kMaximumPoseFragmentBytes,
         "semantic packet test did not span fragments");

  PoseGroupPacketizeRequest request;
  request.envelope.kind = MessageKind::kPoseDelta;
  request.envelope.sender_role = 2;
  request.envelope.stream_id = 2;
  request.envelope.sender_session = 100;
  request.envelope.sequence = 500;
  request.envelope.sender_time_us = 1000000;
  request.pose_id = 10;
  request.baseline_id = 9;
  request.element_count = 1;
  request.group_id = 0;
  request.encoding = PoseGroupEncoding::kSemanticDeltaV1;
  request.group_bytes = encoded;
  std::array<PoseGroupDatagram, 58> descriptors{};
  const std::size_t count = BuildPoseGroupDatagrams(request, descriptors);
  Expect(count > 1, "semantic delta did not packetize");

  PoseGroupReassembler reassembler;
  std::vector<std::uint8_t> reconstructed;
  for (std::size_t reverse = count; reverse-- > 0;) {
    const PoseGroupDatagram &descriptor = descriptors[reverse];
    std::vector<std::uint8_t> datagram(kEnvelopeBytes +
                                       descriptor.envelope.payload_bytes);
    Expect(EncodePoseGroupDatagram(descriptor, encoded, datagram),
           "semantic fragment did not encode");
    Envelope envelope;
    PoseGroupHeader header;
    std::span<const std::uint8_t> fragment;
    Expect(DecodePoseGroupDatagram(datagram, envelope, header, fragment),
           "semantic fragment did not decode");
    const ReassemblyPushResult result =
        reassembler.Push(envelope, header, fragment, 2000 + reverse);
    if (result.completed.has_value()) {
      Expect(result.completed->encoding == PoseGroupEncoding::kSemanticDeltaV1,
             "reassembly changed semantic encoding");
      reconstructed = result.completed->bytes;
    }
  }
  float decoded_root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> decoded_words;
  Expect(DecodeSemanticAnimationDelta(reconstructed, baselines, decoded_root,
                                      root_bone, decoded_words),
         "reassembled semantic delta did not decode");
  Expect(decoded_words == words && root_bone == 9 &&
             decoded_root[0] == root[0] && decoded_root[1] == root[1] &&
             decoded_root[2] == root[2],
         "reverse fragment reassembly changed semantic delta");
}

} // namespace

int main() {
  TestExactRoundTripAndSavings();
  TestExactCompositionDiagnostics();
  TestMaximumDifferences();
  TestMalformedAndTransactionalDecode();
  TestInputValidation();
  TestVarintCanonicalValidation();
  TestDeterministicPropertyRoundTrips();
  TestReverseFragmentReassembly();

  if (g_failures != 0) {
    std::cerr << g_failures << " semantic animation delta test(s) failed\n";
    return 1;
  }
  std::cout << "All semantic animation delta tests passed\n";
  return 0;
}
