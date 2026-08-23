#pragma once

#include <cstdint>

namespace skate3::multiplayer::capture {

inline constexpr float kFallbackBoardRadiusSquared = 16.0f;

// A verified presentation entity is the authoritative ownership signal and
// remains valid when a bail separates the skater from the board. Proximity is
// only a fallback while no verified presentation entity is available.
[[nodiscard]] constexpr bool LocalPresentationPieceOwned(
    std::uint32_t selected_entity, std::uint32_t item_entity,
    bool finite_distance, float distance_squared,
    bool detached_board_piece) {
  if (item_entity == 0 || !finite_distance ||
      distance_squared < 0.0f) {
    return false;
  }
  if (selected_entity != 0) {
    return item_entity == selected_entity;
  }
  return distance_squared <= kFallbackBoardRadiusSquared ||
         detached_board_piece;
}

}  // namespace skate3::multiplayer::capture
