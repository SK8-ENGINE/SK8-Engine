#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
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

enum class TextureFormat : std::uint8_t {
  kBc1,
  kBc3,
};

// One locally installed texture referenced by a Create-a-Skater recipe.
// Payload bytes are linear host-order BC blocks ready for a native-RHI
// upload; retail texture data never crosses the multiplayer transport.
struct RecipeTexture {
  std::uint64_t texture_id = 0;
  TextureFormat format = TextureFormat::kBc1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;
  std::vector<std::uint8_t> bytes;
};

struct RecipePiece {
  std::string category;
  std::uint64_t asset_id = 0;
  std::uint64_t model_id = 0;
  std::uint64_t material_id = 0;
  BindMesh mesh;
  std::uint64_t diffuse_texture = 0;
  std::uint64_t normal_texture = 0;
  std::uint64_t alpha_texture = 0;
  std::uint64_t specular_texture = 0;
};

struct RecipeAppearance {
  std::uint8_t gender = 0;
  std::size_t structural_bytes = 0;
  std::vector<RecipePiece> pieces;
  std::unordered_map<std::uint64_t, RecipeTexture> textures;
};

// Strictly validates a live `cas_db` recipe and resolves its high-detail
// model/texture IDs against the receiver's own extracted retail catalogue.
// The recipe is network-safe metadata only; this routine never accepts paths
// from a peer.
bool ResolveRecipeAppearance(
    const std::vector<std::uint8_t>& recipe,
    bool load_textures,
    RecipeAppearance& output);

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
