#pragma once

#include "skate3_multiplayer_protocol_v12_block_delta.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

// Exact Phase-5 codec. Values are reorganized into same-component lanes for
// each track. Every block independently chooses raw signed differences from
// the receiver-confirmed baseline or residuals from the previous changed
// bone's difference. No approximation and no delta-to-delta dependency is
// introduced.
inline constexpr std::size_t kPredictiveDeltaBlockBones = 32;
inline constexpr std::uint8_t kPredictiveDeltaWidthMask = 0x1Fu;
inline constexpr std::uint8_t kPredictiveDeltaModeBit = 0x20u;
inline constexpr std::uint8_t kPredictiveDeltaReservedMask = 0xC0u;
inline constexpr std::uint8_t kMaximumPredictiveDeltaBitWidth = 18;

namespace predictive_delta_detail {

struct BlockChoice {
  std::uint8_t width = 0;
  bool predictive = false;
  std::int32_t seed = 0;
  std::size_t bytes = 0;
};

[[nodiscard]] inline std::uint8_t RequiredBitWidth(
    std::span<const std::uint32_t> values) {
  std::uint32_t combined = 0;
  for (const std::uint32_t value : values) {
    combined |= value;
  }
  return static_cast<std::uint8_t>(std::bit_width(combined));
}

[[nodiscard]] constexpr std::size_t PackedBytes(
    std::size_t values, std::uint8_t width) {
  return (values * std::size_t(width) + 7) / 8;
}

[[nodiscard]] inline BlockChoice ChooseBlock(
    std::span<const std::int32_t> differences,
    std::span<std::uint32_t> encoded_values) {
  if (differences.empty() ||
      differences.size() > kPredictiveDeltaBlockBones ||
      encoded_values.size() != differences.size()) {
    return {};
  }
  std::array<std::uint32_t, kPredictiveDeltaBlockBones>
      raw{};
  std::array<std::uint32_t, kPredictiveDeltaBlockBones>
      predictive{};
  for (std::size_t index = 0;
       index < differences.size(); ++index) {
    raw[index] =
        delta_detail::ZigZagEncode(differences[index]);
    if (index != 0) {
      predictive[index - 1] = delta_detail::ZigZagEncode(
          differences[index] - differences[index - 1]);
    }
  }
  const std::uint8_t raw_width = RequiredBitWidth(
      std::span<const std::uint32_t>(raw)
          .first(differences.size()));
  const std::uint8_t predictive_width = RequiredBitWidth(
      std::span<const std::uint32_t>(predictive)
          .first(differences.size() - 1));
  const std::size_t raw_bytes =
      1 + PackedBytes(differences.size(), raw_width);
  const std::size_t predictive_bytes =
      1 + 3 +
      PackedBytes(
          differences.size() - 1, predictive_width);
  const bool use_predictive =
      predictive_bytes < raw_bytes;
  const std::uint8_t width =
      use_predictive ? predictive_width : raw_width;
  std::copy_n(
      (use_predictive ? predictive : raw).begin(),
      differences.size() - (use_predictive ? 1 : 0),
      encoded_values.begin());
  return {
      .width = width,
      .predictive = use_predictive,
      .seed = differences[0],
      .bytes = use_predictive ? predictive_bytes : raw_bytes,
  };
}

inline bool PackValues(
    std::span<const std::uint32_t> values,
    std::uint8_t width,
    std::span<std::uint8_t> destination) {
  if (values.empty() ||
      values.size() > kPredictiveDeltaBlockBones ||
      width > kMaximumPredictiveDeltaBitWidth ||
      destination.size() != PackedBytes(values.size(), width)) {
    return false;
  }
  std::fill(destination.begin(), destination.end(), 0);
  std::size_t bit_cursor = 0;
  const std::uint32_t maximum =
      width == 0 ? 0u : (1u << width) - 1u;
  for (const std::uint32_t value : values) {
    if (value > maximum) {
      return false;
    }
    for (std::uint8_t bit = 0; bit < width; ++bit) {
      if ((value & (1u << bit)) != 0) {
        destination[bit_cursor / 8] |=
            static_cast<std::uint8_t>(
                1u << (bit_cursor % 8));
      }
      ++bit_cursor;
    }
  }
  return true;
}

inline bool UnpackValues(
    std::span<const std::uint8_t> source,
    std::uint8_t width,
    std::span<std::uint32_t> values) {
  if (values.empty() ||
      values.size() > kPredictiveDeltaBlockBones ||
      width > kMaximumPredictiveDeltaBitWidth ||
      source.size() != PackedBytes(values.size(), width)) {
    return false;
  }
  std::fill(values.begin(), values.end(), 0);
  std::size_t bit_cursor = 0;
  for (std::uint32_t& value : values) {
    for (std::uint8_t bit = 0; bit < width; ++bit) {
      if ((source[bit_cursor / 8] &
           static_cast<std::uint8_t>(
               1u << (bit_cursor % 8))) != 0) {
        value |= 1u << bit;
      }
      ++bit_cursor;
    }
  }
  if (bit_cursor % 8 != 0 && !source.empty()) {
    const std::uint8_t used =
        static_cast<std::uint8_t>(bit_cursor % 8);
    const std::uint8_t unused =
        static_cast<std::uint8_t>(0xFFu << used);
    if ((source.back() & unused) != 0) {
      return false;
    }
  }
  return true;
}

inline std::size_t CollectChangedBones(
    std::span<const std::uint16_t> masks,
    std::uint16_t bone_count,
    std::span<std::uint16_t> changed_bones) {
  std::size_t count = 0;
  for (std::uint16_t bone = 0; bone < bone_count; ++bone) {
    if ((masks[bone / 16] &
         std::uint16_t(1u << (bone % 16))) != 0) {
      changed_bones[count++] = bone;
    }
  }
  return count;
}

}  // namespace predictive_delta_detail

[[nodiscard]] inline std::size_t
PredictiveAnimationDeltaByteCount(
    std::span<const std::uint16_t> words,
    std::span<const AnimationDeltaBaselineTrack> baselines) {
  if (SemanticAnimationDeltaByteCount(words, baselines) == 0) {
    return 0;
  }
  std::size_t encoded_bytes =
      kAnimationWordStreamHeaderBytes + 8;
  std::size_t cursor = 4;
  std::array<std::uint16_t, protocol::kMaximumAnimationBones>
      changed_bones{};
  std::array<std::int32_t, kPredictiveDeltaBlockBones>
      differences{};
  std::array<std::uint32_t, kPredictiveDeltaBlockBones>
      encoded_values{};
  for (std::size_t track_index = 0;
       track_index < baselines.size(); ++track_index) {
    const std::uint16_t bone_count = words[cursor + 2];
    const std::uint16_t encoding = words[cursor + 3];
    const std::uint16_t mask_count = words[cursor + 4];
    cursor += 5;
    encoded_bytes += 10 + std::size_t(mask_count) * 2;
    const std::span<const std::uint16_t> masks =
        words.subspan(cursor, mask_count);
    cursor += mask_count;
    const std::size_t changed_count =
        predictive_delta_detail::CollectChangedBones(
            masks, bone_count, changed_bones);
    const std::size_t stride =
        AnimationTrackWordStride(encoding);
    const std::size_t value_start = cursor;
    for (std::size_t component = 0;
         component < stride; ++component) {
      for (std::size_t block_start = 0;
           block_start < changed_count;
           block_start += kPredictiveDeltaBlockBones) {
        const std::size_t count = std::min(
            kPredictiveDeltaBlockBones,
            changed_count - block_start);
        for (std::size_t index = 0; index < count; ++index) {
          const std::size_t changed_index =
              block_start + index;
          const std::size_t bone =
              changed_bones[changed_index];
          differences[index] =
              std::int32_t(
                  words[value_start +
                        changed_index * stride + component]) -
              std::int32_t(
                  baselines[track_index]
                      .words[bone * stride + component]);
        }
        const auto choice =
            predictive_delta_detail::ChooseBlock(
                std::span<const std::int32_t>(differences)
                    .first(count),
                std::span<std::uint32_t>(encoded_values)
                    .first(count));
        if (choice.bytes == 0) {
          return 0;
        }
        encoded_bytes += choice.bytes;
      }
    }
    cursor += changed_count * stride;
  }
  return cursor == words.size() &&
                 encoded_bytes <= kMaximumPoseGroupBytes
             ? encoded_bytes
             : 0;
}

[[nodiscard]] inline bool EncodePredictiveAnimationDelta(
    const float root_position[3], std::uint16_t root_bone,
    std::span<const std::uint16_t> words,
    std::span<const AnimationDeltaBaselineTrack> baselines,
    std::span<std::uint8_t> destination) {
  const std::size_t expected =
      PredictiveAnimationDeltaByteCount(words, baselines);
  if (root_position == nullptr ||
      !std::isfinite(root_position[0]) ||
      !std::isfinite(root_position[1]) ||
      !std::isfinite(root_position[2]) ||
      expected == 0 || destination.size() != expected) {
    return false;
  }
  std::size_t output = 0;
  for (const float value : std::span(root_position, 3)) {
    if (!block_delta_detail::WriteU32(
            destination, output,
            std::bit_cast<std::uint32_t>(value))) {
      return false;
    }
  }
  if (!block_delta_detail::WriteU16(
          destination, output, root_bone) ||
      !block_delta_detail::WriteU16(
          destination, output,
          static_cast<std::uint16_t>(words.size()))) {
    return false;
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!block_delta_detail::WriteU16(
            destination, output, words[index])) {
      return false;
    }
  }

  std::size_t cursor = 4;
  std::array<std::uint16_t, protocol::kMaximumAnimationBones>
      changed_bones{};
  std::array<std::int32_t, kPredictiveDeltaBlockBones>
      differences{};
  std::array<std::uint32_t, kPredictiveDeltaBlockBones>
      encoded_values{};
  for (std::size_t track_index = 0;
       track_index < baselines.size(); ++track_index) {
    const std::uint32_t mesh_key =
        std::uint32_t(words[cursor]) |
        (std::uint32_t(words[cursor + 1]) << 16);
    const std::uint16_t bone_count = words[cursor + 2];
    const std::uint16_t encoding = words[cursor + 3];
    const std::uint16_t mask_count = words[cursor + 4];
    cursor += 5;
    if (!block_delta_detail::WriteU32(
            destination, output, mesh_key) ||
        !block_delta_detail::WriteU16(
            destination, output, bone_count) ||
        !block_delta_detail::WriteU16(
            destination, output, encoding) ||
        !block_delta_detail::WriteU16(
            destination, output, mask_count)) {
      return false;
    }
    const std::size_t mask_start = cursor;
    for (std::size_t index = 0; index < mask_count; ++index) {
      if (!block_delta_detail::WriteU16(
              destination, output, words[cursor++])) {
        return false;
      }
    }
    const std::span<const std::uint16_t> masks =
        words.subspan(mask_start, mask_count);
    const std::size_t changed_count =
        predictive_delta_detail::CollectChangedBones(
            masks, bone_count, changed_bones);
    const std::size_t stride =
        AnimationTrackWordStride(encoding);
    const std::size_t value_start = cursor;
    for (std::size_t component = 0;
         component < stride; ++component) {
      for (std::size_t block_start = 0;
           block_start < changed_count;
           block_start += kPredictiveDeltaBlockBones) {
        const std::size_t count = std::min(
            kPredictiveDeltaBlockBones,
            changed_count - block_start);
        for (std::size_t index = 0; index < count; ++index) {
          const std::size_t changed_index =
              block_start + index;
          const std::size_t bone =
              changed_bones[changed_index];
          differences[index] =
              std::int32_t(
                  words[value_start +
                        changed_index * stride + component]) -
              std::int32_t(
                  baselines[track_index]
                      .words[bone * stride + component]);
        }
        const auto choice =
            predictive_delta_detail::ChooseBlock(
                std::span<const std::int32_t>(differences)
                    .first(count),
                std::span<std::uint32_t>(encoded_values)
                    .first(count));
        const std::size_t packed_values =
            count - (choice.predictive ? 1 : 0);
        const std::size_t packed_bytes =
            predictive_delta_detail::PackedBytes(
                packed_values, choice.width);
        const std::uint8_t header =
            choice.width |
            (choice.predictive
                 ? kPredictiveDeltaModeBit
                 : 0u);
        if (!block_delta_detail::WriteU8(
                destination, output, header)) {
          return false;
        }
        if (choice.predictive) {
          const std::uint32_t seed =
              delta_detail::ZigZagEncode(choice.seed);
          if (seed > 131070u ||
              !block_delta_detail::WriteU8(
                  destination, output,
                  static_cast<std::uint8_t>(seed)) ||
              !block_delta_detail::WriteU8(
                  destination, output,
                  static_cast<std::uint8_t>(seed >> 8)) ||
              !block_delta_detail::WriteU8(
                  destination, output,
                  static_cast<std::uint8_t>(seed >> 16))) {
            return false;
          }
        }
        if (output + packed_bytes > destination.size() ||
            !predictive_delta_detail::PackValues(
                std::span<const std::uint32_t>(encoded_values)
                    .first(packed_values),
                choice.width,
                destination.subspan(output, packed_bytes))) {
          return false;
        }
        output += packed_bytes;
      }
    }
    cursor += changed_count * stride;
  }
  return cursor == words.size() &&
         output == destination.size();
}

[[nodiscard]] inline bool DecodePredictiveAnimationDelta(
    std::span<const std::uint8_t> source,
    std::span<const AnimationDeltaBaselineTrack> baselines,
    float root_position[3], std::uint16_t& root_bone,
    std::vector<std::uint16_t>& words) {
  if (root_position == nullptr ||
      source.size() < kAnimationWordStreamHeaderBytes + 8 ||
      source.size() > kMaximumPoseGroupBytes) {
    return false;
  }
  std::size_t cursor = 0;
  std::array<std::uint32_t, 3> root_bits{};
  std::uint16_t decoded_root_bone = 0;
  std::uint16_t word_count = 0;
  for (std::uint32_t& bits : root_bits) {
    if (!block_delta_detail::ReadU32(source, cursor, bits)) {
      return false;
    }
  }
  if (!block_delta_detail::ReadU16(
          source, cursor, decoded_root_bone) ||
      !block_delta_detail::ReadU16(
          source, cursor, word_count) ||
      word_count < 4 ||
      word_count > protocol::kMaximumAnimationFrameWords) {
    return false;
  }
  const std::array<float, 3> decoded_root = {
      std::bit_cast<float>(root_bits[0]),
      std::bit_cast<float>(root_bits[1]),
      std::bit_cast<float>(root_bits[2]),
  };
  if (!std::isfinite(decoded_root[0]) ||
      !std::isfinite(decoded_root[1]) ||
      !std::isfinite(decoded_root[2])) {
    return false;
  }
  std::vector<std::uint16_t> decoded;
  decoded.reserve(word_count);
  for (std::size_t index = 0; index < 4; ++index) {
    std::uint16_t word = 0;
    if (!block_delta_detail::ReadU16(
            source, cursor, word)) {
      return false;
    }
    decoded.push_back(word);
  }
  if (decoded[0] != 0 || decoded[1] == 0 ||
      decoded[1] > protocol::kMaximumAnimationTracks ||
      decoded[1] != baselines.size()) {
    return false;
  }

  std::array<std::uint16_t, protocol::kMaximumAnimationBones>
      changed_bones{};
  std::array<std::uint32_t, kPredictiveDeltaBlockBones>
      encoded_values{};
  for (std::size_t track_index = 0;
       track_index < baselines.size(); ++track_index) {
    std::uint32_t mesh_key = 0;
    std::uint16_t bone_count = 0;
    std::uint16_t encoding = 0;
    std::uint16_t mask_count = 0;
    if (!block_delta_detail::ReadU32(
            source, cursor, mesh_key) ||
        !block_delta_detail::ReadU16(
            source, cursor, bone_count) ||
        !block_delta_detail::ReadU16(
            source, cursor, encoding) ||
        !block_delta_detail::ReadU16(
            source, cursor, mask_count) ||
        !delta_detail::BaselineTrackMatches(
            mesh_key, bone_count, encoding,
            baselines[track_index]) ||
        mask_count !=
            (std::size_t(bone_count) + 15) / 16) {
      return false;
    }
    decoded.push_back(static_cast<std::uint16_t>(mesh_key));
    decoded.push_back(
        static_cast<std::uint16_t>(mesh_key >> 16));
    decoded.push_back(bone_count);
    decoded.push_back(encoding);
    decoded.push_back(mask_count);
    const std::size_t mask_start = decoded.size();
    for (std::size_t index = 0; index < mask_count; ++index) {
      std::uint16_t mask = 0;
      if (!block_delta_detail::ReadU16(
              source, cursor, mask)) {
        return false;
      }
      decoded.push_back(mask);
    }
    const std::span<const std::uint16_t> masks(
        decoded.data() + mask_start, mask_count);
    const std::size_t changed_count =
        predictive_delta_detail::CollectChangedBones(
            masks, bone_count, changed_bones);
    const std::size_t stride =
        AnimationTrackWordStride(encoding);
    std::vector<std::uint16_t> changed_words(
        changed_count * stride);
    for (std::size_t component = 0;
         component < stride; ++component) {
      for (std::size_t block_start = 0;
           block_start < changed_count;
           block_start += kPredictiveDeltaBlockBones) {
        const std::size_t count = std::min(
            kPredictiveDeltaBlockBones,
            changed_count - block_start);
        std::uint8_t header = 0;
        if (!block_delta_detail::ReadU8(
                source, cursor, header) ||
            (header & kPredictiveDeltaReservedMask) != 0) {
          return false;
        }
        const std::uint8_t width =
            header & kPredictiveDeltaWidthMask;
        const bool predictive =
            (header & kPredictiveDeltaModeBit) != 0;
        if (width > kMaximumPredictiveDeltaBitWidth ||
            (!predictive && width > 17)) {
          return false;
        }
        std::int32_t predictor = 0;
        if (predictive) {
          std::uint8_t seed_low = 0;
          std::uint8_t seed_middle = 0;
          std::uint8_t seed_high = 0;
          if (!block_delta_detail::ReadU8(
                  source, cursor, seed_low) ||
              !block_delta_detail::ReadU8(
                  source, cursor, seed_middle) ||
              !block_delta_detail::ReadU8(
                  source, cursor, seed_high)) {
            return false;
          }
          const std::uint32_t encoded_seed =
              std::uint32_t(seed_low) |
              (std::uint32_t(seed_middle) << 8) |
              (std::uint32_t(seed_high) << 16);
          if (encoded_seed > 131070u) {
            return false;
          }
          predictor =
              delta_detail::ZigZagDecode(encoded_seed);
        }
        const std::size_t packed_values =
            count - (predictive ? 1 : 0);
        const std::size_t packed_bytes =
            predictive_delta_detail::PackedBytes(
                packed_values, width);
        if (cursor + packed_bytes > source.size() ||
            !predictive_delta_detail::UnpackValues(
                source.subspan(cursor, packed_bytes),
                width,
                std::span<std::uint32_t>(encoded_values)
                    .first(packed_values))) {
          return false;
        }
        cursor += packed_bytes;
        for (std::size_t index = 0; index < count; ++index) {
          std::int32_t difference = 0;
          if (predictive) {
            if (index != 0) {
              predictor += delta_detail::ZigZagDecode(
                  encoded_values[index - 1]);
            }
            difference = predictor;
          } else {
            difference = delta_detail::ZigZagDecode(
                encoded_values[index]);
          }
          const std::size_t changed_index =
              block_start + index;
          const std::size_t bone =
              changed_bones[changed_index];
          const std::int32_t reconstructed =
              std::int32_t(
                  baselines[track_index]
                      .words[bone * stride + component]) +
              difference;
          if (difference < -65535 ||
              difference > 65535 ||
              reconstructed < 0 ||
              reconstructed > UINT16_MAX) {
            return false;
          }
          changed_words[
              changed_index * stride + component] =
              static_cast<std::uint16_t>(reconstructed);
        }
      }
    }
    decoded.insert(
        decoded.end(),
        changed_words.begin(), changed_words.end());
    if (decoded.size() > word_count) {
      return false;
    }
  }
  if (cursor != source.size() ||
      decoded.size() != word_count ||
      !AnimationWordStreamShapeValid(
          decoded_root.data(), decoded)) {
    return false;
  }
  root_position[0] = decoded_root[0];
  root_position[1] = decoded_root[1];
  root_position[2] = decoded_root[2];
  root_bone = decoded_root_bone;
  words = std::move(decoded);
  return true;
}

}  // namespace skate3::multiplayer::protocol_v12
