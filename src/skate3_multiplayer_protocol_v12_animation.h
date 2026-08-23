#pragma once

#include "skate3_multiplayer_protocol.h"
#include "skate3_multiplayer_protocol_v12_pose.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

// Migration bridge for the exact, already visually validated protocol-v11
// animation word stream. The root metadata used to live in every v11
// fragment; v12 stores it once in the reassembled group instead.
inline constexpr std::uint16_t kAnimationWordStreamHeaderBytes = 16;
inline constexpr std::uint32_t kMaximumAnimationWordStreamBytes =
    kAnimationWordStreamHeaderBytes +
    std::uint32_t(protocol::kMaximumAnimationFrameWords) * 2u;

[[nodiscard]] constexpr std::size_t AnimationWordStreamByteCount(
    std::size_t word_count) {
  return kAnimationWordStreamHeaderBytes + word_count * 2u;
}

[[nodiscard]] inline bool AnimationWordStreamShapeValid(
    const float root_position[3],
    std::span<const std::uint16_t> words) {
  return root_position != nullptr &&
         std::isfinite(root_position[0]) &&
         std::isfinite(root_position[1]) &&
         std::isfinite(root_position[2]) &&
         words.size() >= 4 &&
         words.size() <= protocol::kMaximumAnimationFrameWords &&
         (words[0] & ~std::uint16_t{1}) == 0 &&
         words[1] >= 1 &&
         words[1] <= protocol::kMaximumAnimationTracks;
}

[[nodiscard]] inline bool AnimationWordStreamMatchesPoseGroup(
    MessageKind kind, const PoseGroupHeader& header,
    std::span<const std::uint16_t> words) {
  if (words.size() < 4 ||
      header.encoding != PoseGroupEncoding::kV11WordStream ||
      header.element_count != words[1]) {
    return false;
  }
  const bool keyframe = (words[0] & 1u) != 0;
  const std::uint32_t embedded_baseline =
      static_cast<std::uint32_t>(words[2]) |
      (static_cast<std::uint32_t>(words[3]) << 16);
  if (kind == MessageKind::kPoseBaseline) {
    return keyframe && header.baseline_id == 0 &&
           embedded_baseline == header.pose_id;
  }
  return kind == MessageKind::kPoseDelta && !keyframe &&
         header.baseline_id != 0 &&
         embedded_baseline == header.baseline_id;
}

[[nodiscard]] inline bool EncodeAnimationWordStream(
    const float root_position[3], std::uint16_t root_bone,
    std::span<const std::uint16_t> words,
    std::span<std::uint8_t> destination) {
  if (!AnimationWordStreamShapeValid(root_position, words) ||
      destination.size() !=
          AnimationWordStreamByteCount(words.size())) {
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
  for (const std::uint16_t word : words) {
    if (!writer.U16(word)) {
      return false;
    }
  }
  return writer.offset() == destination.size();
}

[[nodiscard]] inline bool DecodeAnimationWordStream(
    std::span<const std::uint8_t> source,
    float root_position[3], std::uint16_t& root_bone,
    std::vector<std::uint16_t>& words) {
  if (root_position == nullptr ||
      source.size() < kAnimationWordStreamHeaderBytes) {
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
      source.size() != AnimationWordStreamByteCount(word_count)) {
    return false;
  }
  float decoded_root[3] = {
      std::bit_cast<float>(root_bits[0]),
      std::bit_cast<float>(root_bits[1]),
      std::bit_cast<float>(root_bits[2]),
  };
  std::vector<std::uint16_t> decoded_words(word_count);
  for (std::uint16_t& word : decoded_words) {
    if (!reader.U16(word)) {
      return false;
    }
  }
  if (reader.offset() != source.size() ||
      !AnimationWordStreamShapeValid(
          decoded_root, decoded_words)) {
    return false;
  }
  root_position[0] = decoded_root[0];
  root_position[1] = decoded_root[1];
  root_position[2] = decoded_root[2];
  root_bone = decoded_root_bone;
  words = std::move(decoded_words);
  return true;
}

static_assert(kAnimationWordStreamHeaderBytes == 16);
static_assert(kMaximumAnimationWordStreamBytes == 16400);
static_assert(
    kMaximumAnimationWordStreamBytes <= kMaximumPoseGroupBytes);

}  // namespace skate3::multiplayer::protocol_v12
