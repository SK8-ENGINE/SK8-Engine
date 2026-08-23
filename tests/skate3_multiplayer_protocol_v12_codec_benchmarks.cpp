#include "skate3_multiplayer_protocol_v12_block_delta.h"
#include "skate3_multiplayer_protocol_v12_lossless.h"
#include "skate3_multiplayer_protocol_v12_predictive_delta.h"

#include <snappy.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
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

struct AvatarCorpus {
  std::vector<StoredTrack> tracks;
  std::vector<AnimationDeltaBaselineTrack> baselines;
};

AvatarCorpus BuildAvatar() {
  AvatarCorpus avatar;
  // Approximately 4.9 KiB of transform words before the v12 root/header,
  // matching the order of magnitude measured from live full-avatar groups.
  const std::array<StoredTrack, 4> layout = {{
      {
          .mesh_key = 0x10010001u,
          .bone_count = 131,
          .encoding = 1,
          .words = {},
      },
      {
          .mesh_key = 0x20020002u,
          .bone_count = 96,
          .encoding = 1,
          .words = {},
      },
      {
          .mesh_key = 0x30030003u,
          .bone_count = 64,
          .encoding = 1,
          .words = {},
      },
      {
          .mesh_key = 0x40040004u,
          .bone_count = 32,
          .encoding = 0,
          .words = {},
      },
  }};
  avatar.tracks.reserve(layout.size());
  for (std::size_t track_index = 0; track_index < layout.size();
       ++track_index) {
    StoredTrack track = layout[track_index];
    const std::size_t stride = AnimationTrackWordStride(track.encoding);
    track.words.resize(std::size_t(track.bone_count) * stride);
    for (std::size_t bone = 0; bone < track.bone_count; ++bone) {
      for (std::size_t component = 0; component < stride; ++component) {
        const std::uint32_t value =
            12000u + std::uint32_t(track_index) * 1301u +
            std::uint32_t(bone) * 131u + std::uint32_t(component) * 977u;
        track.words[bone * stride + component] =
            static_cast<std::uint16_t>(4096u + value % 53248u);
      }
    }
    avatar.tracks.push_back(std::move(track));
  }
  avatar.baselines.reserve(avatar.tracks.size());
  for (const StoredTrack &track : avatar.tracks) {
    avatar.baselines.push_back({
        .mesh_key = track.mesh_key,
        .bone_count = track.bone_count,
        .encoding = track.encoding,
        .words = track.words,
    });
  }
  return avatar;
}

struct Scenario {
  std::string_view name;
  std::int32_t maximum_difference;
  std::uint32_t unchanged_modulus;
};

std::vector<std::uint16_t> BuildDelta(const AvatarCorpus &avatar,
                                      const Scenario &scenario,
                                      std::uint32_t frame) {
  std::vector<std::uint16_t> words = {
      0,
      static_cast<std::uint16_t>(avatar.tracks.size()),
      0xBEEFu,
      0x0001u,
  };
  for (std::size_t track_index = 0; track_index < avatar.tracks.size();
       ++track_index) {
    const StoredTrack &track = avatar.tracks[track_index];
    AppendU32(words, track.mesh_key);
    words.push_back(track.bone_count);
    words.push_back(track.encoding);
    const std::size_t mask_words = (std::size_t(track.bone_count) + 15) / 16;
    words.push_back(static_cast<std::uint16_t>(mask_words));
    const std::size_t mask_start = words.size();
    words.resize(words.size() + mask_words, 0);
    const std::size_t stride = AnimationTrackWordStride(track.encoding);
    for (std::size_t bone = 0; bone < track.bone_count; ++bone) {
      const bool unchanged = scenario.unchanged_modulus != 0 &&
                             (frame + std::uint32_t(track_index * 3 + bone)) %
                                     scenario.unchanged_modulus ==
                                 0;
      if (unchanged) {
        continue;
      }
      words[mask_start + bone / 16] |=
          static_cast<std::uint16_t>(1u << (bone % 16));
      for (std::size_t component = 0; component < stride; ++component) {
        const std::uint16_t baseline = track.words[bone * stride + component];
        const std::uint32_t phase =
            frame * 17u + std::uint32_t(track_index) * 43u +
            std::uint32_t(bone) * 7u + std::uint32_t(component) * 13u;
        const std::int32_t span = scenario.maximum_difference * 2 + 1;
        const std::int32_t difference =
            static_cast<std::int32_t>(phase %
                                      static_cast<std::uint32_t>(span)) -
            scenario.maximum_difference;
        words.push_back(static_cast<std::uint16_t>(
            std::clamp(std::int32_t(baseline) + difference, 0, 65535)));
      }
    }
  }
  return words;
}

std::vector<std::uint8_t> EncodeRaw(std::span<const std::uint16_t> words) {
  const float root[3] = {125.25f, -47.5f, 8.0f};
  std::vector<std::uint8_t> output(AnimationWordStreamByteCount(words.size()));
  Expect(EncodeAnimationWordStream(root, 3, words, output),
         "raw corpus encoding failed");
  return output;
}

std::vector<std::uint8_t> EncodeSemantic(const AvatarCorpus &avatar,
                                         std::span<const std::uint16_t> words) {
  const float root[3] = {125.25f, -47.5f, 8.0f};
  std::vector<std::uint8_t> output(
      SemanticAnimationDeltaByteCount(words, avatar.baselines));
  Expect(!output.empty() && EncodeSemanticAnimationDelta(
                                root, 3, words, avatar.baselines, output),
         "semantic corpus encoding failed");
  return output;
}

std::vector<std::uint8_t>
EncodeBlockDelta(const AvatarCorpus &avatar,
                 std::span<const std::uint16_t> words) {
  const float root[3] = {125.25f, -47.5f, 8.0f};
  std::vector<std::uint8_t> output(
      BlockPackedAnimationDeltaByteCount(words, avatar.baselines));
  Expect(!output.empty() && EncodeBlockPackedAnimationDelta(
                                root, 3, words, avatar.baselines, output),
         "block delta corpus encoding failed");
  return output;
}

std::vector<std::uint8_t>
EncodePredictiveDelta(const AvatarCorpus &avatar,
                      std::span<const std::uint16_t> words) {
  const float root[3] = {125.25f, -47.5f, 8.0f};
  std::vector<std::uint8_t> output(
      PredictiveAnimationDeltaByteCount(words, avatar.baselines));
  Expect(!output.empty() && EncodePredictiveAnimationDelta(
                                root, 3, words, avatar.baselines, output),
         "predictive delta corpus encoding failed");
  return output;
}

std::string SnappyCompress(std::span<const std::uint8_t> source) {
  std::string compressed;
  snappy::Compress(reinterpret_cast<const char *>(source.data()), source.size(),
                   &compressed);
  return compressed;
}

void ExpectSnappyRoundTrip(std::span<const std::uint8_t> expected,
                           const std::string &compressed) {
  std::string decoded;
  Expect(snappy::Uncompress(compressed.data(), compressed.size(), &decoded),
         "Snappy rejected its own output");
  Expect(decoded.size() == expected.size() &&
             std::equal(decoded.begin(), decoded.end(),
                        reinterpret_cast<const char *>(expected.data())),
         "Snappy round trip changed exact bytes");
}

struct Totals {
  std::uint64_t frames = 0;
  std::uint64_t raw_bytes = 0;
  std::uint64_t semantic_bytes = 0;
  std::uint64_t block_delta_bytes = 0;
  std::uint64_t predictive_delta_bytes = 0;
  std::uint64_t byte_run_bytes = 0;
  std::uint64_t snappy_raw_bytes = 0;
  std::uint64_t snappy_semantic_bytes = 0;
  std::uint64_t snappy_predictive_bytes = 0;
  std::uint64_t selected_group_bytes = 0;
  std::uint64_t selected_datagram_bytes = 0;
  std::uint64_t selected_fragments = 0;
  std::uint64_t changed_bones = 0;
  std::uint64_t component_words = 0;
  std::array<std::uint64_t, 3> varint_words{};
  std::uint64_t rotation_bytes = 0;
  std::uint64_t basis_bytes = 0;
  std::uint64_t translation_bytes = 0;
  std::chrono::nanoseconds snappy_raw_time{};
  std::chrono::nanoseconds snappy_semantic_time{};
  std::chrono::nanoseconds snappy_predictive_time{};
  std::chrono::nanoseconds block_delta_time{};
  std::chrono::nanoseconds predictive_delta_time{};
};

double Average(std::uint64_t total, std::uint64_t count) {
  return count == 0 ? 0.0
                    : static_cast<double>(total) / static_cast<double>(count);
}

double Percent(std::uint64_t value, std::uint64_t reference) {
  return reference == 0 ? 0.0
                        : static_cast<double>(value) * 100.0 /
                              static_cast<double>(reference);
}

Totals RunScenario(const AvatarCorpus &avatar, const Scenario &scenario) {
  constexpr std::uint32_t kFrames = 600;
  Totals totals;
  for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
    const std::vector<std::uint16_t> words =
        BuildDelta(avatar, scenario, frame);
    const std::vector<std::uint8_t> raw = EncodeRaw(words);
    const std::vector<std::uint8_t> semantic = EncodeSemantic(avatar, words);
    const auto block_started = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> block_delta =
        EncodeBlockDelta(avatar, words);
    totals.block_delta_time += std::chrono::steady_clock::now() - block_started;
    const auto predictive_started = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> predictive_delta =
        EncodePredictiveDelta(avatar, words);
    totals.predictive_delta_time +=
        std::chrono::steady_clock::now() - predictive_started;
    SemanticAnimationDeltaStatistics statistics;
    Expect(InspectSemanticAnimationDelta(words, avatar.baselines, statistics),
           "corpus diagnostics rejected a valid frame");

    std::vector<std::uint8_t> byte_run;
    Expect(EncodeLosslessBytes(semantic, byte_run),
           "byte-run corpus encoding failed");

    const auto raw_started = std::chrono::steady_clock::now();
    const std::string snappy_raw = SnappyCompress(raw);
    totals.snappy_raw_time += std::chrono::steady_clock::now() - raw_started;
    const auto semantic_started = std::chrono::steady_clock::now();
    const std::string snappy_semantic = SnappyCompress(semantic);
    totals.snappy_semantic_time +=
        std::chrono::steady_clock::now() - semantic_started;
    const auto predictive_compress_started =
        std::chrono::steady_clock::now();
    const std::string snappy_predictive =
        SnappyCompress(predictive_delta);
    totals.snappy_predictive_time +=
        std::chrono::steady_clock::now() - predictive_compress_started;

    if (frame == 0 || frame + 1 == kFrames) {
      ExpectSnappyRoundTrip(raw, snappy_raw);
      ExpectSnappyRoundTrip(semantic, snappy_semantic);
      ExpectSnappyRoundTrip(predictive_delta, snappy_predictive);
      float decoded_root[3] = {};
      std::uint16_t decoded_root_bone = 0;
      std::vector<std::uint16_t> decoded_words;
      Expect(DecodeBlockPackedAnimationDelta(block_delta, avatar.baselines,
                                             decoded_root, decoded_root_bone,
                                             decoded_words) &&
                 decoded_words == words && decoded_root[0] == 125.25f &&
                 decoded_root[1] == -47.5f && decoded_root[2] == 8.0f &&
                 decoded_root_bone == 3,
             "block delta corpus round trip changed exact data");
      decoded_words.clear();
      Expect(DecodePredictiveAnimationDelta(
                 predictive_delta, avatar.baselines, decoded_root,
                 decoded_root_bone, decoded_words) &&
                 decoded_words == words && decoded_root[0] == 125.25f &&
                 decoded_root[1] == -47.5f && decoded_root[2] == 8.0f &&
                 decoded_root_bone == 3,
             "predictive delta corpus round trip changed exact data");
    }
    ++totals.frames;
    totals.raw_bytes += raw.size();
    totals.semantic_bytes += semantic.size();
    totals.block_delta_bytes += block_delta.size();
    totals.predictive_delta_bytes += predictive_delta.size();
    totals.byte_run_bytes += byte_run.size();
    totals.snappy_raw_bytes += snappy_raw.size();
    totals.snappy_semantic_bytes += snappy_semantic.size();
    totals.snappy_predictive_bytes += snappy_predictive.size();
    const std::size_t selected = std::min({
        raw.size(),
        semantic.size(),
        block_delta.size(),
        predictive_delta.size(),
        snappy_semantic.size(),
        snappy_predictive.size(),
    });
    const std::size_t fragments =
        PoseGroupFragmentCount(static_cast<std::uint32_t>(selected));
    totals.selected_group_bytes += selected;
    totals.selected_fragments += fragments;
    totals.selected_datagram_bytes +=
        selected + fragments * (kEnvelopeBytes + kPoseGroupHeaderBytes);
    totals.changed_bones += statistics.changed_bones;
    totals.component_words += statistics.component_words;
    for (std::size_t index = 0; index < 3; ++index) {
      totals.varint_words[index] += statistics.varint_word_counts[index];
    }
    totals.rotation_bytes += statistics.rigid_rotation_bytes;
    totals.basis_bytes += statistics.affine_basis_bytes;
    totals.translation_bytes += statistics.translation_bytes;
  }
  return totals;
}

void PrintScenario(const Scenario &scenario, const Totals &totals) {
  const double raw = Average(totals.raw_bytes, totals.frames);
  const double semantic = Average(totals.semantic_bytes, totals.frames);
  std::cout << "\nscenario=" << scenario.name << " frames=" << totals.frames
            << " changed_bones/frame="
            << Average(totals.changed_bones, totals.frames)
            << " components/frame="
            << Average(totals.component_words, totals.frames) << '\n';
  std::cout << "  average_bytes raw=" << raw << " semantic=" << semantic << " ("
            << Percent(totals.semantic_bytes, totals.raw_bytes) << "% raw)"
            << " block_delta="
            << Average(totals.block_delta_bytes, totals.frames) << " ("
            << Percent(totals.block_delta_bytes, totals.semantic_bytes)
            << "% semantic)"
            << " predictive_delta="
            << Average(totals.predictive_delta_bytes, totals.frames) << " ("
            << Percent(totals.predictive_delta_bytes, totals.block_delta_bytes)
            << "% block)"
            << " byte_run_semantic="
            << Average(totals.byte_run_bytes, totals.frames)
            << " snappy_raw=" << Average(totals.snappy_raw_bytes, totals.frames)
            << " snappy_semantic="
            << Average(totals.snappy_semantic_bytes, totals.frames) << " ("
            << Percent(totals.snappy_semantic_bytes, totals.semantic_bytes)
            << "% semantic)"
            << " snappy_predictive="
            << Average(totals.snappy_predictive_bytes, totals.frames) << " ("
            << Percent(totals.snappy_predictive_bytes,
                       totals.predictive_delta_bytes)
            << "% predictive)\n";
  std::cout << "  varint_words 1_byte="
            << Percent(totals.varint_words[0], totals.component_words)
            << "% 2_byte="
            << Percent(totals.varint_words[1], totals.component_words)
            << "% 3_byte="
            << Percent(totals.varint_words[2], totals.component_words) << "%\n";
  const std::uint64_t component_bytes =
      totals.rotation_bytes + totals.basis_bytes + totals.translation_bytes;
  std::cout << "  component_value_bytes rotation="
            << Percent(totals.rotation_bytes, component_bytes)
            << "% affine_basis=" << Percent(totals.basis_bytes, component_bytes)
            << "% translation="
            << Percent(totals.translation_bytes, component_bytes) << "%\n";
  std::cout
      << "  snappy_encode_us/frame raw="
      << std::chrono::duration<double, std::micro>(totals.snappy_raw_time)
                 .count() /
             static_cast<double>(totals.frames)
      << " semantic="
      << std::chrono::duration<double, std::micro>(totals.snappy_semantic_time)
                 .count() /
             static_cast<double>(totals.frames)
      << " predictive="
      << std::chrono::duration<double, std::micro>(
             totals.snappy_predictive_time)
                 .count() /
             static_cast<double>(totals.frames)
      << " block_delta_preflight+encode="
      << std::chrono::duration<double, std::micro>(totals.block_delta_time)
                 .count() /
             static_cast<double>(totals.frames)
      << " predictive_delta_preflight+encode="
      << std::chrono::duration<double, std::micro>(
             totals.predictive_delta_time)
                 .count() /
             static_cast<double>(totals.frames)
      << '\n';

  const double selected = Average(totals.selected_group_bytes, totals.frames);
  const double datagram =
      Average(totals.selected_datagram_bytes, totals.frames);
  std::cout << "  selected_candidate_bytes/frame=" << selected
            << " fragments/frame="
            << Average(totals.selected_fragments, totals.frames)
            << " datagram_bytes/frame=" << datagram << '\n';
  std::cout << "  projected_60hz group_payload=" << selected * 60.0 / 1024.0
            << " KiB/s/stream datagrams=" << datagram * 60.0 / 1024.0
            << " KiB/s/stream\n";
  std::cout << "  projected_20hz group_payload=" << selected * 20.0 / 1024.0
            << " KiB/s/stream datagrams=" << datagram * 20.0 / 1024.0
            << " KiB/s/stream\n";
  for (const std::uint32_t players : {2u, 5u, 20u, 50u, 100u}) {
    const double direct_upload =
        datagram * 60.0 * (players - 1) / (1024.0 * 1024.0);
    const double star_host_egress =
        datagram * 60.0 * (players - 1) * (players - 1) / (1024.0 * 1024.0);
    std::cout << "    direct_mesh_players=" << players
              << " sender_upload=" << direct_upload
              << " MiB/s star_host_egress=" << star_host_egress << " MiB/s\n";
  }
}

void TestRandomSnappyIntegrity(const AvatarCorpus &avatar) {
  const Scenario scenario{
      .name = "property",
      .maximum_difference = 32767,
      .unchanged_modulus = 7,
  };
  for (std::uint32_t frame = 0; frame < 2000; ++frame) {
    const std::vector<std::uint16_t> words =
        BuildDelta(avatar, scenario, frame * 7919u);
    const std::vector<std::uint8_t> semantic = EncodeSemantic(avatar, words);
    ExpectSnappyRoundTrip(semantic, SnappyCompress(semantic));
  }
}

} // namespace

int main() {
  std::cout << std::fixed << std::setprecision(2);
  std::cout
      << "Synthetic exact-pose codec study; results are not live telemetry.\n";
  const AvatarCorpus avatar = BuildAvatar();
  const std::array<Scenario, 3> scenarios = {{
      {
          .name = "gentle-confirmed-baseline",
          .maximum_difference = 31,
          .unchanged_modulus = 5,
      },
      {
          .name = "typical-confirmed-baseline",
          .maximum_difference = 4095,
          .unchanged_modulus = 11,
      },
      {
          .name = "stress-confirmed-baseline",
          .maximum_difference = 16383,
          .unchanged_modulus = 0,
      },
  }};
  for (const Scenario &scenario : scenarios) {
    const Totals totals = RunScenario(avatar, scenario);
    Expect(totals.frames == 600 && totals.raw_bytes > totals.frames * 3000 &&
               totals.raw_bytes < totals.frames * 6000,
           "synthetic corpus left the measured full-pose size class");
    if (scenario.name == "typical-confirmed-baseline") {
      Expect(totals.block_delta_bytes * 100 < totals.semantic_bytes * 85,
             "exact block candidate saved less than fifteen percent "
             "on the typical corpus");
      Expect(totals.predictive_delta_bytes < totals.block_delta_bytes,
             "exact predictive candidate did not improve the block codec "
             "on the typical corpus");
    }
    Expect(totals.selected_fragments >= totals.frames &&
               totals.selected_datagram_bytes > totals.selected_group_bytes,
           "scale projection omitted packet framing");
    PrintScenario(scenario, totals);
  }
  TestRandomSnappyIntegrity(avatar);

  if (g_failures != 0) {
    std::cerr << g_failures << " codec benchmark/property test(s) failed\n";
    return 1;
  }
  std::cout << "\nAll codec benchmark/property tests passed\n";
  return 0;
}
