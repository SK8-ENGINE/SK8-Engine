#pragma once

#include "skate3_multiplayer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace skate3::multiplayer::render_cache {

inline constexpr std::size_t kInvalidAnimationTrackIndex =
    std::numeric_limits<std::size_t>::max();

struct WeightedPaletteRows {
  std::array<bool, 256> used{};
  std::size_t count = 0;
  std::size_t invalid_influences = 0;
};

// Discovers the palette rows referenced by the renderer's packed
// float14 character-vertex layout. This is invariant for an installed mesh
// and should be cached with the appearance rather than repeated per frame.
inline WeightedPaletteRows FindWeightedPaletteRows(
    std::span<const float> vertices,
    std::size_t palette_count) {
  WeightedPaletteRows result;
  for (std::size_t vertex = 0;
       vertex + 14 <= vertices.size(); vertex += 14) {
    std::uint32_t packed_weights = 0;
    std::uint32_t packed_indices = 0;
    std::memcpy(
        &packed_weights, vertices.data() + vertex + 7,
        sizeof(packed_weights));
    std::memcpy(
        &packed_indices, vertices.data() + vertex + 8,
        sizeof(packed_indices));
    for (std::size_t influence = 0; influence < 4;
         ++influence) {
      const std::uint8_t weight =
          static_cast<std::uint8_t>(
              packed_weights >> (influence * 8));
      if (weight == 0) {
        continue;
      }
      const std::uint8_t palette_row =
          static_cast<std::uint8_t>(
              packed_indices >> (influence * 8));
      if (palette_row >= palette_count) {
        ++result.invalid_influences;
        continue;
      }
      if (!result.used[palette_row]) {
        result.used[palette_row] = true;
        ++result.count;
      }
    }
  }
  return result;
}

inline bool ValidAnimationTrack(
    const AnimationTrack& track, std::uint32_t mesh_key) {
  return track.mesh_key == mesh_key &&
         !track.bone_rows.empty() &&
         track.bone_rows.size() % 12 == 0;
}

// Track order is stable for normal snapshots, so validate and use the cached
// ordinal first. A linear fallback preserves correctness when an appearance
// or compatibility sender changes the track layout.
inline const AnimationTrack* FindAnimationTrack(
    const std::vector<AnimationTrack>& tracks,
    std::uint32_t mesh_key, std::size_t& cached_index,
    bool* cache_hit = nullptr) {
  if (cache_hit != nullptr) {
    *cache_hit = false;
  }
  if (cached_index < tracks.size() &&
      ValidAnimationTrack(tracks[cached_index], mesh_key)) {
    if (cache_hit != nullptr) {
      *cache_hit = true;
    }
    return &tracks[cached_index];
  }
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    if (ValidAnimationTrack(tracks[index], mesh_key)) {
      cached_index = index;
      return &tracks[index];
    }
  }
  cached_index = kInvalidAnimationTrackIndex;
  return nullptr;
}

}  // namespace skate3::multiplayer::render_cache
