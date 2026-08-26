#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skate3::native_collision {

inline std::vector<std::uint8_t> BuildOwnedCollisionSelection(
    std::span<const std::uint32_t> ranked_indices,
    std::span<const std::uint8_t> currently_selected,
    std::size_t active_limit,
    std::size_t guaranteed_limit,
    std::size_t retention_extra) {
  const std::size_t count = currently_selected.size();
  std::vector<std::uint8_t> desired(count, 0);
  if (ranked_indices.size() != count || count == 0 || active_limit == 0) {
    return desired;
  }

  const std::size_t desired_count = std::min(count, active_limit);
  const std::size_t guaranteed_count =
      std::min(desired_count, guaranteed_limit);
  std::size_t selected = 0;

  // Hysteresis must never displace the resources closest to the player.
  // Reserve the guaranteed prefix before considering previously active
  // resources, then spend only the remaining slots on retention.
  for (std::size_t rank = 0; rank < guaranteed_count; ++rank) {
    const std::uint32_t index = ranked_indices[rank];
    if (index < count && desired[index] == 0) {
      desired[index] = 1;
      ++selected;
    }
  }

  const std::size_t retention_rank =
      std::min(count, desired_count + retention_extra);
  for (std::size_t rank = guaranteed_count;
       rank < retention_rank && selected < desired_count; ++rank) {
    const std::uint32_t index = ranked_indices[rank];
    if (index < count && desired[index] == 0 &&
        currently_selected[index] != 0) {
      desired[index] = 1;
      ++selected;
    }
  }

  for (std::size_t rank = 0;
       rank < count && selected < desired_count; ++rank) {
    const std::uint32_t index = ranked_indices[rank];
    if (index < count && desired[index] == 0) {
      desired[index] = 1;
      ++selected;
    }
  }
  return desired;
}

}  // namespace skate3::native_collision
