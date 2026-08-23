#pragma once

#include "skate3_multiplayer_protocol_v12_animation.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

// A read-only view of the exact receiver-confirmed quantized track. Semantic
// deltas never approximate these words: they only encode the signed numerical
// difference between a changed word and its corresponding baseline word.
struct AnimationDeltaBaselineTrack {
  std::uint32_t mesh_key = 0;
  std::uint16_t bone_count = 0;
  std::uint16_t encoding = 0;
  std::span<const std::uint16_t> words;
};

[[nodiscard]] constexpr std::size_t AnimationTrackWordStride(
    std::uint16_t encoding) {
  switch (static_cast<protocol::AnimationTrackEncoding>(encoding)) {
    case protocol::AnimationTrackEncoding::kRigidQuaternion:
      return 7;
    case protocol::AnimationTrackEncoding::kAffineRowsWideTranslation:
      return 15;
    case protocol::AnimationTrackEncoding::
        kRigidQuaternionWideTranslation:
      return 10;
    case protocol::AnimationTrackEncoding::kAffineRows:
      return 12;
  }
  return 0;
}

namespace delta_detail {

[[nodiscard]] constexpr std::uint32_t ZigZagEncode(
    std::int32_t value) {
  return value >= 0
             ? static_cast<std::uint32_t>(value) * 2u
             : static_cast<std::uint32_t>(-value) * 2u - 1u;
}

[[nodiscard]] constexpr std::int32_t ZigZagDecode(
    std::uint32_t value) {
  return (value & 1u) == 0
             ? static_cast<std::int32_t>(value >> 1)
             : -static_cast<std::int32_t>((value >> 1) + 1u);
}

[[nodiscard]] constexpr std::size_t VarintByteCount(
    std::uint32_t value) {
  return value < (1u << 7) ? 1 : value < (1u << 14) ? 2 : 3;
}

inline bool WriteVarint(detail::LittleEndianWriter& writer,
                        std::uint32_t value) {
  while (value >= 0x80u) {
    if (!writer.U8(
            static_cast<std::uint8_t>((value & 0x7Fu) | 0x80u))) {
      return false;
    }
    value >>= 7;
  }
  return writer.U8(static_cast<std::uint8_t>(value));
}

inline bool ReadVarint(detail::LittleEndianReader& reader,
                       std::uint32_t& output) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 3; ++index) {
    std::uint8_t byte = 0;
    if (!reader.U8(byte)) {
      return false;
    }
    value |= std::uint32_t(byte & 0x7Fu) << (index * 7);
    if ((byte & 0x80u) == 0) {
      if (value > 131070u ||
          VarintByteCount(value) != index + 1) {
        return false;
      }
      output = value;
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline bool BaselineTrackMatches(
    std::uint32_t mesh_key, std::uint16_t bone_count,
    std::uint16_t encoding,
    const AnimationDeltaBaselineTrack& baseline) {
  const std::size_t stride =
      AnimationTrackWordStride(encoding);
  return mesh_key != 0 && bone_count != 0 &&
         bone_count <= protocol::kMaximumAnimationBones &&
         stride != 0 && baseline.mesh_key == mesh_key &&
         baseline.bone_count == bone_count &&
         baseline.encoding == encoding &&
         baseline.words.size() ==
             std::size_t(bone_count) * stride;
}

}  // namespace delta_detail

// Returns zero for malformed/non-delta input. Otherwise returns the exact
// destination size so the live sender can make a selection decision without
// allocating a speculative output buffer.
[[nodiscard]] inline std::size_t SemanticAnimationDeltaByteCount(
    std::span<const std::uint16_t> words,
    std::span<const AnimationDeltaBaselineTrack> baselines) {
  if (words.size() < 4 || words.size() >
          protocol::kMaximumAnimationFrameWords ||
      words[0] != 0 || words[1] == 0 ||
      words[1] > protocol::kMaximumAnimationTracks ||
      words[1] != baselines.size()) {
    return 0;
  }
  std::size_t encoded_bytes =
      kAnimationWordStreamHeaderBytes + 8;
  std::size_t cursor = 4;
  for (std::size_t track_index = 0;
       track_index < baselines.size(); ++track_index) {
    if (cursor + 5 > words.size()) {
      return 0;
    }
    const std::uint32_t mesh_key =
        std::uint32_t(words[cursor]) |
        (std::uint32_t(words[cursor + 1]) << 16);
    const std::uint16_t bone_count = words[cursor + 2];
    const std::uint16_t encoding = words[cursor + 3];
    const std::uint16_t mask_word_count = words[cursor + 4];
    cursor += 5;
    const std::size_t expected_masks =
        (std::size_t(bone_count) + 15) / 16;
    if (!delta_detail::BaselineTrackMatches(
            mesh_key, bone_count, encoding,
            baselines[track_index]) ||
        mask_word_count != expected_masks ||
        cursor + mask_word_count > words.size()) {
      return 0;
    }
    encoded_bytes += 10 + std::size_t(mask_word_count) * 2;
    const std::size_t mask_start = cursor;
    cursor += mask_word_count;
    const std::size_t stride =
        AnimationTrackWordStride(encoding);
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      if ((words[mask_start + bone / 16] &
           std::uint16_t(1u << (bone % 16))) == 0) {
        continue;
      }
      if (cursor + stride > words.size()) {
        return 0;
      }
      const std::size_t base_offset = bone * stride;
      for (std::size_t component = 0;
           component < stride; ++component) {
        const std::int32_t difference =
            std::int32_t(words[cursor + component]) -
            std::int32_t(
                baselines[track_index]
                    .words[base_offset + component]);
        encoded_bytes += delta_detail::VarintByteCount(
            delta_detail::ZigZagEncode(difference));
      }
      cursor += stride;
    }
  }
  return cursor == words.size() &&
                 encoded_bytes <= kMaximumPoseGroupBytes
             ? encoded_bytes
             : 0;
}

[[nodiscard]] inline bool EncodeSemanticAnimationDelta(
    const float root_position[3], std::uint16_t root_bone,
    std::span<const std::uint16_t> words,
    std::span<const AnimationDeltaBaselineTrack> baselines,
    std::span<std::uint8_t> destination) {
  const std::size_t expected_bytes =
      SemanticAnimationDeltaByteCount(words, baselines);
  if (root_position == nullptr ||
      !std::isfinite(root_position[0]) ||
      !std::isfinite(root_position[1]) ||
      !std::isfinite(root_position[2]) ||
      expected_bytes == 0 ||
      destination.size() != expected_bytes) {
    return false;
  }
  detail::LittleEndianWriter writer(destination);
  if (!writer.U32(std::bit_cast<std::uint32_t>(root_position[0])) ||
      !writer.U32(std::bit_cast<std::uint32_t>(root_position[1])) ||
      !writer.U32(std::bit_cast<std::uint32_t>(root_position[2])) ||
      !writer.U16(root_bone) ||
      !writer.U16(static_cast<std::uint16_t>(words.size()))) {
    return false;
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!writer.U16(words[index])) {
      return false;
    }
  }
  std::size_t cursor = 4;
  for (std::size_t track_index = 0;
       track_index < baselines.size(); ++track_index) {
    const std::uint32_t mesh_key =
        std::uint32_t(words[cursor]) |
        (std::uint32_t(words[cursor + 1]) << 16);
    const std::uint16_t bone_count = words[cursor + 2];
    const std::uint16_t encoding = words[cursor + 3];
    const std::uint16_t mask_word_count = words[cursor + 4];
    cursor += 5;
    if (!writer.U32(mesh_key) || !writer.U16(bone_count) ||
        !writer.U16(encoding) ||
        !writer.U16(mask_word_count)) {
      return false;
    }
    const std::size_t mask_start = cursor;
    for (std::size_t index = 0;
         index < mask_word_count; ++index) {
      if (!writer.U16(words[cursor++])) {
        return false;
      }
    }
    const std::size_t stride =
        AnimationTrackWordStride(encoding);
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      if ((words[mask_start + bone / 16] &
           std::uint16_t(1u << (bone % 16))) == 0) {
        continue;
      }
      const std::size_t base_offset = bone * stride;
      for (std::size_t component = 0;
           component < stride; ++component) {
        const std::int32_t difference =
            std::int32_t(words[cursor++]) -
            std::int32_t(
                baselines[track_index]
                    .words[base_offset + component]);
        if (!delta_detail::WriteVarint(
                writer,
                delta_detail::ZigZagEncode(difference))) {
          return false;
        }
      }
    }
  }
  return cursor == words.size() &&
         writer.offset() == destination.size();
}

[[nodiscard]] inline bool DecodeSemanticAnimationDelta(
    std::span<const std::uint8_t> source,
    std::span<const AnimationDeltaBaselineTrack> baselines,
    float root_position[3], std::uint16_t& root_bone,
    std::vector<std::uint16_t>& words) {
  if (root_position == nullptr ||
      source.size() < kAnimationWordStreamHeaderBytes + 8 ||
      source.size() > kMaximumPoseGroupBytes) {
    return false;
  }
  detail::LittleEndianReader reader(source);
  std::uint32_t root_bits[3] = {};
  std::uint16_t decoded_root_bone = 0;
  std::uint16_t word_count = 0;
  if (!reader.U32(root_bits[0]) ||
      !reader.U32(root_bits[1]) ||
      !reader.U32(root_bits[2]) ||
      !reader.U16(decoded_root_bone) ||
      !reader.U16(word_count) ||
      word_count < 4 ||
      word_count > protocol::kMaximumAnimationFrameWords) {
    return false;
  }
  float decoded_root[3] = {
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
    if (!reader.U16(word)) {
      return false;
    }
    decoded.push_back(word);
  }
  if (decoded[0] != 0 || decoded[1] == 0 ||
      decoded[1] > protocol::kMaximumAnimationTracks ||
      decoded[1] != baselines.size()) {
    return false;
  }
  for (std::size_t track_index = 0;
       track_index < baselines.size(); ++track_index) {
    std::uint32_t mesh_key = 0;
    std::uint16_t bone_count = 0;
    std::uint16_t encoding = 0;
    std::uint16_t mask_word_count = 0;
    if (!reader.U32(mesh_key) || !reader.U16(bone_count) ||
        !reader.U16(encoding) ||
        !reader.U16(mask_word_count) ||
        !delta_detail::BaselineTrackMatches(
            mesh_key, bone_count, encoding,
            baselines[track_index]) ||
        mask_word_count !=
            (std::size_t(bone_count) + 15) / 16) {
      return false;
    }
    decoded.push_back(static_cast<std::uint16_t>(mesh_key));
    decoded.push_back(
        static_cast<std::uint16_t>(mesh_key >> 16));
    decoded.push_back(bone_count);
    decoded.push_back(encoding);
    decoded.push_back(mask_word_count);
    const std::size_t mask_start = decoded.size();
    for (std::size_t index = 0;
         index < mask_word_count; ++index) {
      std::uint16_t mask = 0;
      if (!reader.U16(mask)) {
        return false;
      }
      decoded.push_back(mask);
    }
    const std::size_t stride =
        AnimationTrackWordStride(encoding);
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      if ((decoded[mask_start + bone / 16] &
           std::uint16_t(1u << (bone % 16))) == 0) {
        continue;
      }
      const std::size_t base_offset = bone * stride;
      for (std::size_t component = 0;
           component < stride; ++component) {
        std::uint32_t encoded_difference = 0;
        if (!delta_detail::ReadVarint(
                reader, encoded_difference)) {
          return false;
        }
        const std::int32_t reconstructed =
            std::int32_t(
                baselines[track_index]
                    .words[base_offset + component]) +
            delta_detail::ZigZagDecode(encoded_difference);
        if (reconstructed < 0 || reconstructed > UINT16_MAX) {
          return false;
        }
        decoded.push_back(
            static_cast<std::uint16_t>(reconstructed));
      }
    }
    if (decoded.size() > word_count) {
      return false;
    }
  }
  if (reader.offset() != source.size() ||
      decoded.size() != word_count ||
      !AnimationWordStreamShapeValid(decoded_root, decoded)) {
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
