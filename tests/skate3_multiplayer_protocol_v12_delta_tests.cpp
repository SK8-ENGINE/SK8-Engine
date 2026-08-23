#include "skate3_multiplayer_protocol_v12_delta.h"

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

void AppendU32(std::vector<std::uint16_t>& words,
               std::uint32_t value) {
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
      first[index] = static_cast<std::uint16_t>(
          20000u + (index * 193u) % 20000u);
    }
    for (std::size_t index = 0; index < second.size(); ++index) {
      second[index] = static_cast<std::uint16_t>(
          10000u + (index * 271u) % 40000u);
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

std::vector<std::uint16_t> BuildDelta(
    const Fixture& fixture, std::uint32_t seed,
    bool small_differences = true) {
  std::mt19937 random(seed);
  std::vector<std::uint16_t> words = {
      0, 2, 0x3344u, 0x1122u,
  };
  for (std::size_t track_index = 0;
       track_index < fixture.views.size(); ++track_index) {
    const AnimationDeltaBaselineTrack& baseline =
        fixture.views[track_index];
    AppendU32(words, baseline.mesh_key);
    words.push_back(baseline.bone_count);
    words.push_back(baseline.encoding);
    const std::size_t mask_count =
        (std::size_t(baseline.bone_count) + 15) / 16;
    words.push_back(
        static_cast<std::uint16_t>(mask_count));
    const std::size_t mask_start = words.size();
    words.resize(words.size() + mask_count, 0);
    const std::size_t stride =
        AnimationTrackWordStride(baseline.encoding);
    for (std::size_t bone = 0;
         bone < baseline.bone_count; ++bone) {
      if ((random() % 5u) == 0u && bone != 0) {
        continue;
      }
      words[mask_start + bone / 16] |=
          static_cast<std::uint16_t>(1u << (bone % 16));
      for (std::size_t component = 0;
           component < stride; ++component) {
        const std::uint16_t base =
            baseline.words[bone * stride + component];
        if (small_differences) {
          const std::int32_t offset =
              static_cast<std::int32_t>(random() % 63u) - 31;
          words.push_back(static_cast<std::uint16_t>(
              std::clamp(
                  std::int32_t(base) + offset, 0, 65535)));
        } else {
          words.push_back(
              static_cast<std::uint16_t>(random()));
        }
      }
    }
  }
  return words;
}

std::vector<std::uint8_t> Encode(
    const Fixture& fixture,
    std::span<const std::uint16_t> words) {
  const float root[3] = {1.25f, -200.5f, 0.03125f};
  const std::size_t encoded_size =
      SemanticAnimationDeltaByteCount(words, fixture.views);
  std::vector<std::uint8_t> encoded(encoded_size);
  Expect(encoded_size != 0,
         "valid semantic delta had zero encoded size");
  Expect(EncodeSemanticAnimationDelta(
             root, 77, words, fixture.views, encoded),
         "valid semantic delta did not encode");
  return encoded;
}

void ExpectRoundTrip(const Fixture& fixture,
                     std::span<const std::uint16_t> expected,
                     std::span<const std::uint8_t> encoded) {
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> decoded;
  Expect(DecodeSemanticAnimationDelta(
             encoded, fixture.views, root, root_bone, decoded),
         "valid semantic delta did not decode");
  Expect(std::equal(
             decoded.begin(), decoded.end(),
             expected.begin(), expected.end()),
         "semantic delta changed exact animation words");
  Expect(decoded.size() == expected.size(),
         "semantic delta changed animation word count");
  Expect(root[0] == 1.25f && root[1] == -200.5f &&
             root[2] == 0.03125f && root_bone == 77,
         "semantic delta changed root metadata");
}

void TestExactRoundTripAndSavings() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words =
      BuildDelta(fixture, 1);
  const std::vector<std::uint8_t> encoded =
      Encode(fixture, words);
  ExpectRoundTrip(fixture, words, encoded);
  Expect(encoded.size() ==
             SemanticAnimationDeltaByteCount(
                 words, fixture.views),
         "semantic delta preflight size was not exact");
  Expect(encoded.size() * 100 <
             AnimationWordStreamByteCount(words.size()) * 65,
         "small exact transform differences saved less than 35 percent");
}

void TestMaximumDifferences() {
  Fixture fixture;
  std::fill(fixture.first.begin(), fixture.first.end(), 0);
  std::fill(fixture.second.begin(), fixture.second.end(), UINT16_MAX);
  std::vector<std::uint16_t> words =
      BuildDelta(fixture, 2, false);
  // Force both signed-difference extremes into the stream.
  const std::size_t first_track_values = 4 + 5 + 2;
  words[first_track_values] = UINT16_MAX;
  const std::size_t second_track =
      first_track_values +
      std::size_t(Fixture::kFirstBones) * 7;
  const std::size_t second_track_values = second_track + 5 + 2;
  words[second_track_values] = 0;
  const std::vector<std::uint8_t> encoded =
      Encode(fixture, words);
  ExpectRoundTrip(fixture, words, encoded);
}

void TestMalformedAndTransactionalDecode() {
  const Fixture fixture;
  const std::vector<std::uint16_t> words =
      BuildDelta(fixture, 3);
  const std::vector<std::uint8_t> encoded =
      Encode(fixture, words);
  for (std::size_t size = 0; size < encoded.size(); ++size) {
    float root[3] = {9.0f, 8.0f, 7.0f};
    std::uint16_t root_bone = 123;
    std::vector<std::uint16_t> output = {4, 5, 6};
    Expect(!DecodeSemanticAnimationDelta(
               std::span<const std::uint8_t>(encoded).first(size),
               fixture.views, root, root_bone, output),
           "truncated semantic delta was accepted");
    Expect(root[0] == 9.0f && root[1] == 8.0f &&
               root[2] == 7.0f && root_bone == 123 &&
               output == std::vector<std::uint16_t>({4, 5, 6}),
           "failed semantic delta decode modified output");
  }

  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0);
  float root[3] = {};
  std::uint16_t root_bone = 0;
  std::vector<std::uint16_t> output;
  Expect(!DecodeSemanticAnimationDelta(
             trailing, fixture.views, root, root_bone, output),
         "semantic delta with trailing data was accepted");

  Fixture wrong = fixture;
  wrong.views[0].mesh_key ^= 1u;
  Expect(!DecodeSemanticAnimationDelta(
             encoded, wrong.views, root, root_bone, output),
         "semantic delta accepted a different baseline layout");

  auto malformed = encoded;
  // Word count lives at byte 14 and must match the reconstructed stream.
  malformed[14] = 4;
  malformed[15] = 0;
  Expect(!DecodeSemanticAnimationDelta(
             malformed, fixture.views, root, root_bone, output),
         "semantic delta accepted a false decoded word count");
}

void TestInputValidation() {
  const Fixture fixture;
  std::vector<std::uint16_t> words =
      BuildDelta(fixture, 4);
  words[0] = 1;
  Expect(SemanticAnimationDeltaByteCount(
             words, fixture.views) == 0,
         "semantic delta encoder accepted a keyframe");
  words[0] = 0;
  words[1] = 1;
  Expect(SemanticAnimationDeltaByteCount(
             words, fixture.views) == 0,
         "semantic delta encoder accepted wrong track count");
  words[1] = 2;
  words.pop_back();
  Expect(SemanticAnimationDeltaByteCount(
             words, fixture.views) == 0,
         "semantic delta encoder accepted truncated words");
}

void TestDeterministicPropertyRoundTrips() {
  const Fixture fixture;
  for (std::uint32_t seed = 0; seed < 2000; ++seed) {
    const std::vector<std::uint16_t> words =
        BuildDelta(fixture, 0x9E3779B9u ^ seed,
                   (seed % 3u) != 0);
    const std::vector<std::uint8_t> encoded =
        Encode(fixture, words);
    ExpectRoundTrip(fixture, words, encoded);
  }
}

}  // namespace

int main() {
  TestExactRoundTripAndSavings();
  TestMaximumDifferences();
  TestMalformedAndTransactionalDecode();
  TestInputValidation();
  TestDeterministicPropertyRoundTrips();

  if (g_failures != 0) {
    std::cerr << g_failures
              << " semantic animation delta test(s) failed\n";
    return 1;
  }
  std::cout << "All semantic animation delta tests passed\n";
  return 0;
}
