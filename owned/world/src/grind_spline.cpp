#include "skate/world/grind_spline.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace skate::world {
namespace {

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kRailSize = 32;
constexpr std::size_t kSegmentSize = 144;
constexpr std::uint64_t kSplineTypeSignature =
    0x2C7017070007004Aull;
constexpr float kMinimumSegmentLength = 0.001f;

void WriteU32(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              std::uint32_t value) {
  bytes.at(offset) = static_cast<std::uint8_t>(value >> 24u);
  bytes.at(offset + 1) =
      static_cast<std::uint8_t>(value >> 16u);
  bytes.at(offset + 2) =
      static_cast<std::uint8_t>(value >> 8u);
  bytes.at(offset + 3) = static_cast<std::uint8_t>(value);
}

void WriteU64(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              std::uint64_t value) {
  WriteU32(bytes, offset, static_cast<std::uint32_t>(value >> 32u));
  WriteU32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

void WriteF32(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              float value) {
  WriteU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

void WriteVec4(std::vector<std::uint8_t>& bytes,
               std::size_t offset,
               Vec3 value,
               float w) {
  WriteF32(bytes, offset, value.x);
  WriteF32(bytes, offset + 4, value.y);
  WriteF32(bytes, offset + 8, value.z);
  WriteF32(bytes, offset + 12, w);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes.at(offset)) << 24u) |
         (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 16u) |
         (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 8u) |
         bytes.at(offset + 3);
}

bool IsFinite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

float WordFloat(std::uint32_t value) {
  return std::bit_cast<float>(value);
}

void WriteNativeSegment(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    const NativeGrindSegment& segment,
    Vec3 translation) {
  for (std::size_t word = 0; word < segment.words.size(); ++word) {
    WriteU32(bytes, offset + word * 4, segment.words[word]);
  }
  for (const std::size_t vector_word : {12u, 20u, 24u}) {
    WriteF32(
        bytes, offset + (vector_word + 0) * 4,
        WordFloat(segment.words[vector_word + 0]) + translation.x);
    WriteF32(
        bytes, offset + (vector_word + 1) * 4,
        WordFloat(segment.words[vector_word + 1]) + translation.y);
    WriteF32(
        bytes, offset + (vector_word + 2) * 4,
        WordFloat(segment.words[vector_word + 2]) + translation.z);
  }
}

std::uint64_t HashRail(const GrindRail& rail) {
  // Stable project-owned identity. Skate only requires a durable unique
  // 64-bit value here; ArenaBuilder derives the same field from point text.
  std::uint64_t hash = 1469598103934665603ull;
  const auto add_byte = [&hash](std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  for (const unsigned char value : rail.name) {
    add_byte(value);
  }
  for (const Vec3 point : rail.points) {
    for (const float component : {point.x, point.y, point.z}) {
      const std::uint32_t bits = std::bit_cast<std::uint32_t>(component);
      add_byte(static_cast<std::uint8_t>(bits >> 24u));
      add_byte(static_cast<std::uint8_t>(bits >> 16u));
      add_byte(static_cast<std::uint8_t>(bits >> 8u));
      add_byte(static_cast<std::uint8_t>(bits));
    }
  }
  return hash;
}

std::vector<Vec3> ExpandedPoints(const GrindRail& rail) {
  std::vector<Vec3> points = rail.points;
  if (rail.closed &&
      LengthSquared(points.front() - points.back()) >
          kMinimumSegmentLength * kMinimumSegmentLength) {
    points.push_back(points.front());
  }
  return points;
}

bool AddGuestBase(std::vector<std::uint8_t>& bytes,
                  std::size_t field,
                  std::uint32_t guest_base,
                  bool allow_zero) {
  const std::uint32_t offset = ReadU32(bytes, field);
  if (offset == 0 && allow_zero) {
    return true;
  }
  if (offset >= bytes.size() ||
      guest_base >
          std::numeric_limits<std::uint32_t>::max() - offset) {
    return false;
  }
  WriteU32(bytes, field, guest_base + offset);
  return true;
}

}  // namespace

GrindSplineBuildResult BuildGrindSplineData(
    const MapDefinition& map,
    Vec3 translation) {
  GrindSplineBuildResult result;
  if (!IsFinite(translation)) {
    result.error = "grind-spline translation is not finite";
    return result;
  }
  if (map.grind_rails.empty()) {
    result.error = "map has no authored grind rails";
    return result;
  }
  if (map.grind_rails.size() >
      std::numeric_limits<std::uint16_t>::max()) {
    result.error = "grind rail count exceeds the native format";
    return result;
  }

  std::vector<std::vector<Vec3>> rail_points;
  rail_points.reserve(map.grind_rails.size());
  std::size_t segment_count = 0;
  for (const GrindRail& rail : map.grind_rails) {
    const bool native = !rail.native_segments.empty();
    if (rail.id == 0 || rail.name.empty() ||
        (native && (rail.retail_spline_id == 0 ||
                    rail.retail_type_signature == 0)) ||
        (!native && rail.points.size() < 2)) {
      result.error = "grind rail definition is invalid";
      return result;
    }
    if (native) {
      for (const NativeGrindSegment& segment : rail.native_segments) {
        if (!std::all_of(
                segment.words.begin(), segment.words.end(),
                [](std::uint32_t word) {
                  return std::isfinite(WordFloat(word));
                })) {
          result.error = "native grind segment contains non-finite data";
          return result;
        }
      }
      segment_count += rail.native_segments.size();
      rail_points.emplace_back();
      continue;
    }
    std::vector<Vec3> points = ExpandedPoints(rail);
    for (Vec3& point : points) {
      if (!IsFinite(point)) {
        result.error = "grind rail point is not finite";
        return result;
      }
      point = point + translation;
    }
    for (std::size_t point = 1; point < points.size(); ++point) {
      if (Length(points[point] - points[point - 1]) <=
          kMinimumSegmentLength) {
        result.error = "grind rail contains a degenerate segment";
        return result;
      }
    }
    segment_count += points.size() - 1;
    rail_points.push_back(std::move(points));
  }
  if (segment_count > std::numeric_limits<std::uint32_t>::max()) {
    result.error = "grind segment count exceeds the native format";
    return result;
  }

  const std::size_t rail_bytes =
      map.grind_rails.size() * kRailSize;
  if (segment_count >
      (std::numeric_limits<std::size_t>::max() -
       kHeaderSize - rail_bytes) /
          kSegmentSize) {
    result.error = "grind spline blob size overflow";
    return result;
  }
  const std::size_t segment_table = kHeaderSize + rail_bytes;
  const std::size_t total_size =
      segment_table + segment_count * kSegmentSize;
  if (total_size > std::numeric_limits<std::uint32_t>::max()) {
    result.error = "grind spline blob exceeds 32-bit address space";
    return result;
  }

  std::vector<std::uint8_t> bytes(total_size, 0);
  WriteU32(bytes, 0,
           static_cast<std::uint32_t>(map.grind_rails.size()));
  WriteU32(bytes, 4, static_cast<std::uint32_t>(segment_count));
  WriteU32(bytes, 8, static_cast<std::uint32_t>(kHeaderSize));
  WriteU32(bytes, 12, static_cast<std::uint32_t>(segment_table));

  std::size_t global_segment = 0;
  for (std::size_t rail_index = 0;
       rail_index < map.grind_rails.size(); ++rail_index) {
    const GrindRail& rail = map.grind_rails[rail_index];
    const std::vector<Vec3>& points = rail_points[rail_index];
    const std::size_t rail_offset =
        kHeaderSize + rail_index * kRailSize;
    const bool native = !rail.native_segments.empty();
    const std::size_t rail_segment_count =
        native ? rail.native_segments.size() : points.size() - 1;

    WriteU64(
        bytes, rail_offset,
        native ? rail.retail_spline_id : HashRail(rail));
    WriteU64(
        bytes, rail_offset + 8,
        native ? rail.retail_type_signature : kSplineTypeSignature);
    WriteU32(
        bytes, rail_offset + 16,
        native ? rail.retail_flags : 0);
    WriteU32(
        bytes, rail_offset + 20,
        static_cast<std::uint32_t>(
            segment_table + global_segment * kSegmentSize));
    WriteU32(
        bytes, rail_offset + 24,
        static_cast<std::uint32_t>(
            segment_table +
            (global_segment + rail_segment_count - 1) * kSegmentSize));
    WriteU32(
        bytes, rail_offset + 28,
        native ? rail.retail_trailing_word : 0);

    for (std::size_t local_segment = 0;
         local_segment < rail_segment_count; ++local_segment) {
      const std::size_t segment_index =
          global_segment + local_segment;
      const std::size_t segment_offset =
          segment_table + segment_index * kSegmentSize;
      if (native) {
        WriteNativeSegment(
            bytes, segment_offset,
            rail.native_segments[local_segment], translation);
      } else {
        const Vec3 start = points[local_segment];
        const Vec3 end = points[local_segment + 1];
        const Vec3 delta = end - start;
        const Vec3 cubic_a{
            -2.0f * delta.x,
            -2.0f * delta.y,
            -2.0f * delta.z};
        const Vec3 cubic_b{
            3.0f * delta.x,
            3.0f * delta.y,
            3.0f * delta.z};
        const Vec3 minimum{
            std::min(start.x, end.x),
            std::min(start.y, end.y),
            std::min(start.z, end.z)};
        const Vec3 maximum{
            std::max(start.x, end.x),
            std::max(start.y, end.y),
            std::max(start.z, end.z)};

        // Retail straight grind segments use the cubic smoothstep
        // D + 3*delta*t^2 - 2*delta*t^3. The remaining coefficient
        // and auxiliary fields stay zero in the native payload.
        WriteVec4(bytes, segment_offset, cubic_a, 0.0f);
        WriteVec4(bytes, segment_offset + 16, cubic_b, 0.0f);
        WriteVec4(bytes, segment_offset + 48, start, 1.0f);
        WriteVec4(bytes, segment_offset + 80, minimum, 0.0f);
        WriteVec4(bytes, segment_offset + 96, maximum, 0.0f);
      }
      WriteU32(bytes, segment_offset + 120,
               static_cast<std::uint32_t>(rail_offset));
      if (local_segment > 0) {
        WriteU32(
            bytes, segment_offset + 124,
            static_cast<std::uint32_t>(
                segment_offset - kSegmentSize));
      }
      if (local_segment + 1 < rail_segment_count) {
        WriteU32(
            bytes, segment_offset + 128,
            static_cast<std::uint32_t>(
                segment_offset + kSegmentSize));
      }
    }
    global_segment += rail_segment_count;
  }

  result.ok = true;
  result.blob.bytes = std::move(bytes);
  result.blob.rail_count =
      static_cast<std::uint32_t>(map.grind_rails.size());
  result.blob.segment_count =
      static_cast<std::uint32_t>(segment_count);
  return result;
}

bool FixupGrindSplineDataForGuest(
    std::vector<std::uint8_t>& bytes,
    std::uint32_t guest_base) {
  if (bytes.size() < kHeaderSize || guest_base == 0) {
    return false;
  }
  const std::uint32_t rail_count = ReadU32(bytes, 0);
  const std::uint32_t segment_count = ReadU32(bytes, 4);
  const std::uint32_t rail_table = ReadU32(bytes, 8);
  const std::uint32_t segment_table = ReadU32(bytes, 12);
  const std::size_t expected_segment_table =
      kHeaderSize + static_cast<std::size_t>(rail_count) * kRailSize;
  const std::size_t expected_size =
      expected_segment_table +
      static_cast<std::size_t>(segment_count) * kSegmentSize;
  if (rail_count == 0 || segment_count == 0 ||
      rail_table != kHeaderSize ||
      segment_table != expected_segment_table ||
      expected_size != bytes.size()) {
    return false;
  }

  for (std::uint32_t rail = 0; rail < rail_count; ++rail) {
    const std::size_t offset =
        kHeaderSize + static_cast<std::size_t>(rail) * kRailSize;
    if (!AddGuestBase(bytes, offset + 20, guest_base, false) ||
        !AddGuestBase(bytes, offset + 24, guest_base, false)) {
      return false;
    }
  }
  for (std::uint32_t segment = 0; segment < segment_count; ++segment) {
    const std::size_t offset =
        expected_segment_table +
        static_cast<std::size_t>(segment) * kSegmentSize;
    if (!AddGuestBase(bytes, offset + 120, guest_base, false) ||
        !AddGuestBase(bytes, offset + 124, guest_base, true) ||
        !AddGuestBase(bytes, offset + 128, guest_base, true)) {
      return false;
    }
  }
  if (!AddGuestBase(bytes, 8, guest_base, false) ||
      !AddGuestBase(bytes, 12, guest_base, false)) {
    return false;
  }
  return true;
}

}  // namespace skate::world
