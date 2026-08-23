#pragma once

#include "skate3_multiplayer_protocol_v12_lossless.h"
#include "skate3_multiplayer_protocol_v12_pose.h"

#include <snappy.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

inline constexpr std::uint32_t kSnappyPoseMagic = 0x31504E53u;  // "SNP1"
inline constexpr std::uint8_t kSnappyPoseVersion = 1;
inline constexpr std::size_t kSnappyPoseHeaderBytes = 12;

[[nodiscard]] constexpr bool SnappyInnerEncodingValid(
    PoseGroupEncoding encoding) {
  return PoseGroupEncodingValid(encoding) &&
         encoding != PoseGroupEncoding::kSnappyV1;
}

[[nodiscard]] inline bool EncodeSnappyPoseGroup(
    PoseGroupEncoding inner_encoding,
    std::span<const std::uint8_t> source,
    std::vector<std::uint8_t>& destination) {
  if (!SnappyInnerEncodingValid(inner_encoding) || source.empty() ||
      source.size() > kMaximumLosslessDecodedBytes) {
    return false;
  }
  std::string compressed;
  snappy::Compress(reinterpret_cast<const char*>(source.data()),
                   source.size(), &compressed);
  if (compressed.empty() ||
      compressed.size() + kSnappyPoseHeaderBytes >
          kMaximumPoseGroupBytes) {
    return false;
  }
  destination.resize(kSnappyPoseHeaderBytes + compressed.size());
  detail::LittleEndianWriter writer(
      std::span<std::uint8_t>(destination).first(kSnappyPoseHeaderBytes));
  if (!writer.U32(kSnappyPoseMagic) ||
      !writer.U8(kSnappyPoseVersion) ||
      !writer.U8(static_cast<std::uint8_t>(inner_encoding)) ||
      !writer.U16(0) ||
      !writer.U32(static_cast<std::uint32_t>(source.size())) ||
      writer.offset() != kSnappyPoseHeaderBytes) {
    destination.clear();
    return false;
  }
  std::copy(compressed.begin(), compressed.end(),
            destination.begin() +
                static_cast<std::ptrdiff_t>(kSnappyPoseHeaderBytes));
  return true;
}

[[nodiscard]] inline bool DecodeSnappyPoseGroup(
    std::span<const std::uint8_t> source,
    PoseGroupEncoding& inner_encoding,
    std::vector<std::uint8_t>& destination) {
  if (source.size() <= kSnappyPoseHeaderBytes ||
      source.size() > kMaximumPoseGroupBytes) {
    return false;
  }
  detail::LittleEndianReader reader(
      source.first(kSnappyPoseHeaderBytes));
  std::uint32_t magic = 0;
  std::uint8_t version = 0;
  std::uint8_t encoding = 0;
  std::uint16_t reserved = 0;
  std::uint32_t decoded_bytes = 0;
  if (!reader.U32(magic) || !reader.U8(version) ||
      !reader.U8(encoding) || !reader.U16(reserved) ||
      !reader.U32(decoded_bytes) ||
      reader.offset() != kSnappyPoseHeaderBytes ||
      magic != kSnappyPoseMagic || version != kSnappyPoseVersion ||
      reserved != 0 || decoded_bytes == 0 ||
      decoded_bytes > kMaximumLosslessDecodedBytes) {
    return false;
  }
  const auto decoded_encoding =
      static_cast<PoseGroupEncoding>(encoding);
  if (!SnappyInnerEncodingValid(decoded_encoding)) {
    return false;
  }
  const auto compressed = source.subspan(kSnappyPoseHeaderBytes);
  std::size_t advertised_bytes = 0;
  if (!snappy::GetUncompressedLength(
          reinterpret_cast<const char*>(compressed.data()),
          compressed.size(), &advertised_bytes) ||
      advertised_bytes != decoded_bytes) {
    return false;
  }
  std::vector<std::uint8_t> decoded(decoded_bytes);
  if (!snappy::RawUncompress(
          reinterpret_cast<const char*>(compressed.data()),
          compressed.size(),
          reinterpret_cast<char*>(decoded.data()))) {
    return false;
  }
  inner_encoding = decoded_encoding;
  destination = std::move(decoded);
  return true;
}

}  // namespace skate3::multiplayer::protocol_v12
