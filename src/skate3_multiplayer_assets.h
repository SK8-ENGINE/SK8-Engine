#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace skate3::multiplayer_assets {

// Renderer-ready vanilla bind mesh resolved from the locally installed
// Create-a-Skater catalogue. No retail presentation entity is involved: the
// custom renderer consumes this geometry and the normal replicated canonical
// skeleton.
struct BindMesh {
  std::uint64_t asset_id = 0;
  std::filesystem::path source_path;
  std::vector<float> vertices;
  std::vector<std::uint16_t> indices;
  std::vector<std::uint16_t> palette_to_canonical;
  float bbox_min[3] = {};
  float bbox_max[3] = {};
};

// Resolves a live ROPA mesh by immutable topology. ROPA rewrites positions
// every simulation frame, but its vertex/index layout remains the layout of
// the vanilla RX2 bind mesh. Character family 2 maps to OuterTorso and family
// 4/5 maps to Hair.
bool ResolveRopaBindMesh(
    std::uint8_t character_family,
    std::uint32_t vertex_count,
    std::uint32_t index_count,
    std::uint64_t topology_hash,
    BindMesh& output);

}  // namespace skate3::multiplayer_assets
