#pragma once

#include "skate3_multiplayer_protocol_v12_delta.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

// Offline Phase-5 candidate: exact signed baseline differences are grouped in
// blocks of 32 words. One byte records each block's required bit width and the
// values follow densely packed, least-significant bit first. Every block is
// independently decodable and the pose still depends only on the existing
// receiver-confirmed baseline, not on an earlier delta.
inline constexpr std::size_t kAnimationDeltaBitBlockWords = 32;
inline constexpr std::uint8_t kMaximumAnimationDeltaBitWidth = 17;

namespace block_delta_detail {

[[nodiscard]] constexpr std::size_t PackedBlockBytes(std::size_t values,
                                                     std::uint8_t bit_width) {
  return (values * std::size_t(bit_width) + 7) / 8;
}

[[nodiscard]] inline std::uint8_t
RequiredBitWidth(std::span<const std::uint32_t> values) {
  std::uint32_t combined = 0;
  for (const std::uint32_t value : values) {
    combined |= value;
  }
  return static_cast<std::uint8_t>(std::bit_width(combined));
}

inline bool WriteU8(std::span<std::uint8_t> destination, std::size_t &cursor,
                    std::uint8_t value) {
  if (cursor >= destination.size()) {
    return false;
  }
  destination[cursor++] = value;
  return true;
}

inline bool WriteU16(std::span<std::uint8_t> destination, std::size_t &cursor,
                     std::uint16_t value) {
  return WriteU8(destination, cursor, static_cast<std::uint8_t>(value)) &&
         WriteU8(destination, cursor, static_cast<std::uint8_t>(value >> 8));
}

inline bool WriteU32(std::span<std::uint8_t> destination, std::size_t &cursor,
                     std::uint32_t value) {
  return WriteU16(destination, cursor, static_cast<std::uint16_t>(value)) &&
         WriteU16(destination, cursor, static_cast<std::uint16_t>(value >> 16));
}

inline bool ReadU8(std::span<const std::uint8_t> source, std::size_t &cursor,
                   std::uint8_t &value) {
  if (cursor >= source.size()) {
    return false;
  }
  value = source[cursor++];
  return true;
}

inline bool ReadU16(std::span<const std::uint8_t> source, std::size_t &cursor,
                    std::uint16_t &value) {
  std::uint8_t low = 0;
  std::uint8_t high = 0;
  if (!ReadU8(source, cursor, low) || !ReadU8(source, cursor, high)) {
    return false;
  }
  value = std::uint16_t(low) | (std::uint16_t(high) << 8);
  return true;
}

inline bool ReadU32(std::span<const std::uint8_t> source, std::size_t &cursor,
                    std::uint32_t &value) {
  std::uint16_t low = 0;
  std::uint16_t high = 0;
  if (!ReadU16(source, cursor, low) || !ReadU16(source, cursor, high)) {
    return false;
  }
  value = std::uint32_t(low) | (std::uint32_t(high) << 16);
  return true;
}

inline bool PackBlock(std::span<const std::uint32_t> values,
                      std::uint8_t bit_width,
                      std::span<std::uint8_t> destination) {
  if (values.empty() || values.size() > kAnimationDeltaBitBlockWords ||
      bit_width > kMaximumAnimationDeltaBitWidth ||
      destination.size() != PackedBlockBytes(values.size(), bit_width)) {
    return false;
  }
  std::fill(destination.begin(), destination.end(), 0);
  std::size_t bit_cursor = 0;
  const std::uint32_t maximum = bit_width == 0 ? 0u : (1u << bit_width) - 1u;
  for (const std::uint32_t value : values) {
    if (value > maximum) {
      return false;
    }
    for (std::uint8_t bit = 0; bit < bit_width; ++bit) {
      if ((value & (1u << bit)) != 0) {
        destination[bit_cursor / 8] |=
            static_cast<std::uint8_t>(1u << (bit_cursor % 8));
      }
      ++bit_cursor;
    }
  }
  return bit_cursor == values.size() * bit_width;
}

inline bool UnpackBlock(std::span<const std::uint8_t> source,
                        std::uint8_t bit_width,
                        std::span<std::uint32_t> values) {
  if (values.empty() || values.size() > kAnimationDeltaBitBlockWords ||
      bit_width > kMaximumAnimationDeltaBitWidth ||
      source.size() != PackedBlockBytes(values.size(), bit_width)) {
    return false;
  }
  std::fill(values.begin(), values.end(), 0);
  std::size_t bit_cursor = 0;
  for (std::uint32_t &value : values) {
    for (std::uint8_t bit = 0; bit < bit_width; ++bit) {
      if ((source[bit_cursor / 8] &
           static_cast<std::uint8_t>(1u << (bit_cursor % 8))) != 0) {
        value |= 1u << bit;
      }
      ++bit_cursor;
    }
  }
  // Unused high bits in the final byte must be zero for one canonical wire
  // representation and fail-closed malformed-input handling.
  if (bit_cursor % 8 != 0 && !source.empty()) {
    const std::uint8_t used = static_cast<std::uint8_t>(bit_cursor % 8);
    const std::uint8_t unused_mask = static_cast<std::uint8_t>(0xFFu << used);
    if ((source.back() & unused_mask) != 0) {
      return false;
    }
  }
  return true;
}

} // namespace block_delta_detail

[[nodiscard]] inline std::size_t BlockPackedAnimationDeltaByteCount(
    std::span<const std::uint16_t> words,
    std::span<const AnimationDeltaBaselineTrack> baselines) {
  SemanticAnimationDeltaStatistics statistics;
  if (!InspectSemanticAnimationDelta(words, baselines, statistics)) {
    return 0;
  }
  std::size_t encoded_bytes = statistics.fixed_header_bytes;
  std::size_t cursor = 4;
  std::array<std::uint32_t, kAnimationDeltaBitBlockWords> block{};
  for (std::size_t track_index = 0; track_index < baselines.size();
       ++track_index) {
    const std::uint16_t bone_count = words[cursor + 2];
    const std::uint16_t encoding = words[cursor + 3];
    const std::uint16_t mask_word_count = words[cursor + 4];
    cursor += 5;
    encoded_bytes += 10 + std::size_t(mask_word_count) * 2;
    const std::size_t mask_start = cursor;
    cursor += mask_word_count;
    const std::size_t stride = AnimationTrackWordStride(encoding);
    std::size_t block_values = 0;
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      if ((words[mask_start + bone / 16] & std::uint16_t(1u << (bone % 16))) ==
          0) {
        continue;
      }
      const std::size_t baseline_offset = bone * stride;
      for (std::size_t component = 0; component < stride; ++component) {
        const std::int32_t difference =
            std::int32_t(words[cursor++]) -
            std::int32_t(
                baselines[track_index].words[baseline_offset + component]);
        block[block_values++] = delta_detail::ZigZagEncode(difference);
        if (block_values == block.size()) {
          const std::uint8_t width =
              block_delta_detail::RequiredBitWidth(block);
          encoded_bytes +=
              1 + block_delta_detail::PackedBlockBytes(block_values, width);
          block_values = 0;
        }
      }
    }
    if (block_values != 0) {
      const std::uint8_t width = block_delta_detail::RequiredBitWidth(
          std::span<const std::uint32_t>(block).first(block_values));
      encoded_bytes +=
          1 + block_delta_detail::PackedBlockBytes(block_values, width);
    }
  }
  return cursor == words.size() && encoded_bytes <= kMaximumPoseGroupBytes
             ? encoded_bytes
             : 0;
}

[[nodiscard]] inline bool EncodeBlockPackedAnimationDelta(
    const float root_position[3], std::uint16_t root_bone,
    std::span<const std::uint16_t> words,
    std::span<const AnimationDeltaBaselineTrack> baselines,
    std::span<std::uint8_t> destination) {
  const std::size_t expected =
      BlockPackedAnimationDeltaByteCount(words, baselines);
  if (root_position == nullptr || !std::isfinite(root_position[0]) ||
      !std::isfinite(root_position[1]) || !std::isfinite(root_position[2]) ||
      expected == 0 || destination.size() != expected) {
    return false;
  }
  std::size_t output = 0;
  for (const float value : std::span(root_position, 3)) {
    if (!block_delta_detail::WriteU32(destination, output,
                                      std::bit_cast<std::uint32_t>(value))) {
      return false;
    }
  }
  if (!block_delta_detail::WriteU16(destination, output, root_bone) ||
      !block_delta_detail::WriteU16(destination, output,
                                    static_cast<std::uint16_t>(words.size()))) {
    return false;
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!block_delta_detail::WriteU16(destination, output, words[index])) {
      return false;
    }
  }

  std::size_t cursor = 4;
  std::array<std::uint32_t, kAnimationDeltaBitBlockWords> block{};
  for (std::size_t track_index = 0; track_index < baselines.size();
       ++track_index) {
    const std::uint32_t mesh_key =
        std::uint32_t(words[cursor]) | (std::uint32_t(words[cursor + 1]) << 16);
    const std::uint16_t bone_count = words[cursor + 2];
    const std::uint16_t encoding = words[cursor + 3];
    const std::uint16_t mask_word_count = words[cursor + 4];
    cursor += 5;
    if (!block_delta_detail::WriteU32(destination, output, mesh_key) ||
        !block_delta_detail::WriteU16(destination, output, bone_count) ||
        !block_delta_detail::WriteU16(destination, output, encoding) ||
        !block_delta_detail::WriteU16(destination, output, mask_word_count)) {
      return false;
    }
    const std::size_t mask_start = cursor;
    for (std::size_t index = 0; index < mask_word_count; ++index) {
      if (!block_delta_detail::WriteU16(destination, output, words[cursor++])) {
        return false;
      }
    }
    const std::size_t stride = AnimationTrackWordStride(encoding);
    std::size_t block_values = 0;
    const auto flush_block = [&]() {
      if (block_values == 0) {
        return true;
      }
      const std::span<const std::uint32_t> values =
          std::span<const std::uint32_t>(block).first(block_values);
      const std::uint8_t width = block_delta_detail::RequiredBitWidth(values);
      const std::size_t packed_bytes =
          block_delta_detail::PackedBlockBytes(block_values, width);
      if (!block_delta_detail::WriteU8(destination, output, width) ||
          output + packed_bytes > destination.size() ||
          !block_delta_detail::PackBlock(
              values, width, destination.subspan(output, packed_bytes))) {
        return false;
      }
      output += packed_bytes;
      block_values = 0;
      return true;
    };
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      if ((words[mask_start + bone / 16] & std::uint16_t(1u << (bone % 16))) ==
          0) {
        continue;
      }
      const std::size_t baseline_offset = bone * stride;
      for (std::size_t component = 0; component < stride; ++component) {
        const std::int32_t difference =
            std::int32_t(words[cursor++]) -
            std::int32_t(
                baselines[track_index].words[baseline_offset + component]);
        block[block_values++] = delta_detail::ZigZagEncode(difference);
        if (block_values == block.size() && !flush_block()) {
          return false;
        }
      }
    }
    if (!flush_block()) {
      return false;
    }
  }
  return cursor == words.size() && output == destination.size();
}

[[nodiscard]] inline bool DecodeBlockPackedAnimationDelta(
    std::span<const std::uint8_t> source,
    std::span<const AnimationDeltaBaselineTrack> baselines,
    float root_position[3], std::uint16_t &root_bone,
    std::vector<std::uint16_t> &words) {
  if (root_position == nullptr ||
      source.size() < kAnimationWordStreamHeaderBytes + 8 ||
      source.size() > kMaximumPoseGroupBytes) {
    return false;
  }
  std::size_t cursor = 0;
  std::array<std::uint32_t, 3> root_bits{};
  std::uint16_t decoded_root_bone = 0;
  std::uint16_t word_count = 0;
  for (std::uint32_t &bits : root_bits) {
    if (!block_delta_detail::ReadU32(source, cursor, bits)) {
      return false;
    }
  }
  if (!block_delta_detail::ReadU16(source, cursor, decoded_root_bone) ||
      !block_delta_detail::ReadU16(source, cursor, word_count) ||
      word_count < 4 || word_count > protocol::kMaximumAnimationFrameWords) {
    return false;
  }
  const std::array<float, 3> decoded_root = {
      std::bit_cast<float>(root_bits[0]),
      std::bit_cast<float>(root_bits[1]),
      std::bit_cast<float>(root_bits[2]),
  };
  if (!std::isfinite(decoded_root[0]) || !std::isfinite(decoded_root[1]) ||
      !std::isfinite(decoded_root[2])) {
    return false;
  }
  std::vector<std::uint16_t> decoded;
  decoded.reserve(word_count);
  for (std::size_t index = 0; index < 4; ++index) {
    std::uint16_t word = 0;
    if (!block_delta_detail::ReadU16(source, cursor, word)) {
      return false;
    }
    decoded.push_back(word);
  }
  if (decoded[0] != 0 || decoded[1] == 0 ||
      decoded[1] > protocol::kMaximumAnimationTracks ||
      decoded[1] != baselines.size()) {
    return false;
  }

  std::array<std::uint32_t, kAnimationDeltaBitBlockWords> block{};
  for (std::size_t track_index = 0; track_index < baselines.size();
       ++track_index) {
    std::uint32_t mesh_key = 0;
    std::uint16_t bone_count = 0;
    std::uint16_t encoding = 0;
    std::uint16_t mask_word_count = 0;
    if (!block_delta_detail::ReadU32(source, cursor, mesh_key) ||
        !block_delta_detail::ReadU16(source, cursor, bone_count) ||
        !block_delta_detail::ReadU16(source, cursor, encoding) ||
        !block_delta_detail::ReadU16(source, cursor, mask_word_count) ||
        !delta_detail::BaselineTrackMatches(mesh_key, bone_count, encoding,
                                            baselines[track_index]) ||
        mask_word_count != (std::size_t(bone_count) + 15) / 16) {
      return false;
    }
    decoded.push_back(static_cast<std::uint16_t>(mesh_key));
    decoded.push_back(static_cast<std::uint16_t>(mesh_key >> 16));
    decoded.push_back(bone_count);
    decoded.push_back(encoding);
    decoded.push_back(mask_word_count);
    const std::size_t mask_start = decoded.size();
    std::size_t changed_bones = 0;
    for (std::size_t index = 0; index < mask_word_count; ++index) {
      std::uint16_t mask = 0;
      if (!block_delta_detail::ReadU16(source, cursor, mask)) {
        return false;
      }
      decoded.push_back(mask);
      changed_bones += std::popcount(mask);
    }
    const std::size_t stride = AnimationTrackWordStride(encoding);
    const std::size_t component_count = changed_bones * stride;
    std::vector<std::uint32_t> differences;
    differences.reserve(component_count);
    while (differences.size() < component_count) {
      const std::size_t values = std::min(kAnimationDeltaBitBlockWords,
                                          component_count - differences.size());
      std::uint8_t width = 0;
      if (!block_delta_detail::ReadU8(source, cursor, width) ||
          width > kMaximumAnimationDeltaBitWidth) {
        return false;
      }
      const std::size_t packed_bytes =
          block_delta_detail::PackedBlockBytes(values, width);
      if (cursor + packed_bytes > source.size() ||
          !block_delta_detail::UnpackBlock(
              source.subspan(cursor, packed_bytes), width,
              std::span<std::uint32_t>(block).first(values))) {
        return false;
      }
      differences.insert(differences.end(), block.begin(),
                         block.begin() + values);
      cursor += packed_bytes;
    }
    std::size_t difference_cursor = 0;
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      if ((decoded[mask_start + bone / 16] &
           std::uint16_t(1u << (bone % 16))) == 0) {
        continue;
      }
      const std::size_t baseline_offset = bone * stride;
      for (std::size_t component = 0; component < stride; ++component) {
        const std::int32_t reconstructed =
            std::int32_t(
                baselines[track_index].words[baseline_offset + component]) +
            delta_detail::ZigZagDecode(differences[difference_cursor++]);
        if (reconstructed < 0 || reconstructed > UINT16_MAX) {
          return false;
        }
        decoded.push_back(static_cast<std::uint16_t>(reconstructed));
      }
    }
    if (difference_cursor != differences.size() ||
        decoded.size() > word_count) {
      return false;
    }
  }
  if (cursor != source.size() || decoded.size() != word_count ||
      !AnimationWordStreamShapeValid(decoded_root.data(), decoded)) {
    return false;
  }
  root_position[0] = decoded_root[0];
  root_position[1] = decoded_root[1];
  root_position[2] = decoded_root[2];
  root_bone = decoded_root_bone;
  words = std::move(decoded);
  return true;
}

} // namespace skate3::multiplayer::protocol_v12
