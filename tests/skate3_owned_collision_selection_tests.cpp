#include "skate3_owned_collision_selection.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

namespace {

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  constexpr std::size_t kResourceCount = 64;
  std::vector<std::uint32_t> ranked(kResourceCount);
  std::iota(ranked.begin(), ranked.end(), 0u);

  // Model movement into a new sector while the old implementation still has
  // all 32 slots occupied by resources within its top-48 retention window.
  std::vector<std::uint8_t> previously_selected(kResourceCount, 0);
  std::fill(previously_selected.begin() + 16,
            previously_selected.begin() + 48, 1);

  const std::vector<std::uint8_t> selected =
      skate3::native_collision::BuildOwnedCollisionSelection(
          ranked, previously_selected, 32, 24, 16);

  bool ok = true;
  for (std::size_t rank = 0; rank < 24; ++rank) {
    ok &= Expect(selected[rank] != 0,
                 "a guaranteed nearest resource was displaced");
  }
  ok &= Expect(
      std::count(selected.begin(), selected.end(), std::uint8_t{1}) == 32,
      "selection did not fill the active collision budget");

  const std::vector<std::uint8_t> small_selection =
      skate3::native_collision::BuildOwnedCollisionSelection(
          std::span<const std::uint32_t>(ranked).first(12),
          std::span<const std::uint8_t>(previously_selected).first(12),
          32, 24, 16);
  ok &= Expect(
      std::count(small_selection.begin(), small_selection.end(),
                 std::uint8_t{1}) == 12,
      "maps smaller than the active budget must keep every resource");
  return ok ? 0 : 1;
}
