#include "skate3_multiplayer_render_cache.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using skate3::multiplayer::AnimationTrack;
using skate3::multiplayer::render_cache::FindAnimationTrack;
using skate3::multiplayer::render_cache::FindWeightedPaletteRows;
using skate3::multiplayer::render_cache::
    kInvalidAnimationTrackIndex;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

void SetPackedVertexInfluences(
    std::vector<float>& vertices, std::size_t vertex,
    std::uint32_t weights, std::uint32_t indices) {
  const std::size_t base = vertex * 14;
  std::memcpy(
      vertices.data() + base + 7, &weights,
      sizeof(weights));
  std::memcpy(
      vertices.data() + base + 8, &indices,
      sizeof(indices));
}

AnimationTrack Track(
    std::uint32_t key, std::size_t bones = 1) {
  AnimationTrack track;
  track.mesh_key = key;
  track.bone_rows.resize(bones * 12, 1.0f);
  return track;
}

void TestWeightedRowsAreDiscoveredOncePerTopology() {
  std::vector<float> vertices(2 * 14);
  SetPackedVertexInfluences(
      vertices, 0, 0x000080FFu, 0x00000401u);
  SetPackedVertexInfluences(
      vertices, 1, 0x40400080u, 0x00070104u);

  const auto rows = FindWeightedPaletteRows(vertices, 5);
  Expect(rows.count == 3,
         "weighted palette discovery lost a valid row");
  Expect(rows.used[1] && rows.used[4] && rows.used[0],
         "weighted palette discovery returned the wrong rows");
  Expect(rows.invalid_influences == 1,
         "out-of-range weighted influence was not counted");
}

void TestCachedTrackOrdinalAndLayoutFallback() {
  constexpr std::uint32_t kWanted = 0x12345678u;
  std::vector<AnimationTrack> tracks{
      Track(0x10u), Track(kWanted, 2), Track(0x20u)};
  std::size_t cached = kInvalidAnimationTrackIndex;
  bool cache_hit = true;

  const AnimationTrack* first =
      FindAnimationTrack(tracks, kWanted, cached, &cache_hit);
  Expect(first == &tracks[1] && cached == 1 && !cache_hit,
         "initial track scan did not cache the resolved ordinal");

  const AnimationTrack* second =
      FindAnimationTrack(tracks, kWanted, cached, &cache_hit);
  Expect(second == &tracks[1] && cache_hit,
         "stable track layout did not use the cached ordinal");

  std::swap(tracks[0], tracks[1]);
  const AnimationTrack* moved =
      FindAnimationTrack(tracks, kWanted, cached, &cache_hit);
  Expect(moved == &tracks[0] && cached == 0 && !cache_hit,
         "changed track layout did not fall back to a safe scan");
}

void TestInvalidAndMissingTracksDoNotStayCached() {
  constexpr std::uint32_t kWanted = 0x87654321u;
  std::vector<AnimationTrack> tracks{Track(kWanted)};
  tracks.front().bone_rows.resize(11);
  std::size_t cached = 0;

  Expect(FindAnimationTrack(tracks, kWanted, cached) == nullptr,
         "malformed animation track was accepted");
  Expect(cached == kInvalidAnimationTrackIndex,
         "missing track left a stale cached ordinal");

  tracks.push_back(Track(kWanted, 3));
  const AnimationTrack* recovered =
      FindAnimationTrack(tracks, kWanted, cached);
  Expect(recovered == &tracks[1] && cached == 1,
         "valid replacement track did not recover after a miss");
}

}  // namespace

int main() {
  TestWeightedRowsAreDiscoveredOncePerTopology();
  TestCachedTrackOrdinalAndLayoutFallback();
  TestInvalidAndMissingTracksDoNotStayCached();

  if (g_failures != 0) {
    std::cerr << g_failures
              << " multiplayer render-cache test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer render-cache tests passed\n";
  return 0;
}
