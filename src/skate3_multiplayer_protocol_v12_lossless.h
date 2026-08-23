#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

inline constexpr std::size_t kLosslessPrefixBytes = 4;
inline constexpr std::size_t kMaximumLosslessDecodedBytes = 64u * 1024u;

[[nodiscard]] inline bool EncodeLosslessBytes(
    std::span<const std::uint8_t> source,
    std::vector<std::uint8_t>& destination) {
  if (source.empty() ||
      source.size() > kMaximumLosslessDecodedBytes) {
    return false;
  }
  destination.clear();
  destination.reserve(source.size() + kLosslessPrefixBytes);
  const auto size = static_cast<std::uint32_t>(source.size());
  for (unsigned shift = 0; shift < 32; shift += 8) {
    destination.push_back(
        static_cast<std::uint8_t>(size >> shift));
  }
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    std::size_t run = 1;
    while (cursor + run < source.size() &&
           source[cursor + run] == source[cursor] &&
           run < 66) {
      ++run;
    }
    if (run >= 3) {
      if (source[cursor] == 0) {
        destination.push_back(
            static_cast<std::uint8_t>(0x80u + run - 3));
      } else {
        destination.push_back(
            static_cast<std::uint8_t>(0xC0u + run - 3));
        destination.push_back(source[cursor]);
      }
      cursor += run;
      continue;
    }

    const std::size_t literal_start = cursor;
    cursor += run;
    while (cursor < source.size() &&
           cursor - literal_start < 128) {
      std::size_t next_run = 1;
      while (cursor + next_run < source.size() &&
             source[cursor + next_run] == source[cursor] &&
             next_run < 3) {
        ++next_run;
      }
      if (next_run >= 3) {
        break;
      }
      cursor += next_run;
    }
    const std::size_t literal_bytes = cursor - literal_start;
    destination.push_back(
        static_cast<std::uint8_t>(literal_bytes - 1));
    destination.insert(
        destination.end(), source.begin() + literal_start,
        source.begin() + cursor);
  }
  return true;
}

[[nodiscard]] inline bool DecodeLosslessBytes(
    std::span<const std::uint8_t> source,
    std::vector<std::uint8_t>& destination) {
  if (source.size() < kLosslessPrefixBytes) {
    return false;
  }
  const std::uint32_t decoded_size =
      std::uint32_t(source[0]) |
      (std::uint32_t(source[1]) << 8) |
      (std::uint32_t(source[2]) << 16) |
      (std::uint32_t(source[3]) << 24);
  if (decoded_size == 0 ||
      decoded_size > kMaximumLosslessDecodedBytes) {
    return false;
  }
  destination.clear();
  destination.reserve(decoded_size);
  std::size_t cursor = kLosslessPrefixBytes;
  while (cursor < source.size() &&
         destination.size() < decoded_size) {
    const std::uint8_t token = source[cursor++];
    if (token < 0x80) {
      const std::size_t count = std::size_t(token) + 1;
      if (cursor + count > source.size() ||
          destination.size() + count > decoded_size) {
        return false;
      }
      destination.insert(
          destination.end(), source.begin() + cursor,
          source.begin() + cursor + count);
      cursor += count;
    } else if (token < 0xC0) {
      const std::size_t count =
          std::size_t(token - 0x80) + 3;
      if (destination.size() + count > decoded_size) {
        return false;
      }
      destination.insert(destination.end(), count, 0);
    } else {
      const std::size_t count =
          std::size_t(token - 0xC0) + 3;
      if (cursor >= source.size() ||
          destination.size() + count > decoded_size) {
        return false;
      }
      destination.insert(
          destination.end(), count, source[cursor++]);
    }
  }
  return cursor == source.size() &&
         destination.size() == decoded_size;
}

}  // namespace skate3::multiplayer::protocol_v12
