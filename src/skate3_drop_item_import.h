#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skate3::drop_item_import {

struct TextureBinding {
  std::string channel;
  std::uint64_t texture_id = 0;
};

struct Material {
  std::uint64_t id = 0;
  std::string type;
  std::vector<TextureBinding> textures;
};

struct Item {
  std::string category;
  std::string name;
  std::uint64_t model_id = 0;
  std::vector<std::uint64_t> material_ids;
  bool dynamic = false;
};

struct Recipe {
  std::unordered_map<std::uint64_t, Material> materials;
  std::vector<Item> items;
};

// Exposed for focused parser tests. Categories and item/material membership
// come directly from the retail recipe; no category names are synthesized.
bool ParseRecipe(
    std::string_view xml, bool dynamic,
    Recipe& recipe, std::string& error);

struct Progress {
  std::size_t completed = 0;
  std::size_t total = 0;
  std::size_t written = 0;
  std::size_t reused = 0;
  std::size_t unsupported = 0;
  std::string message;
};

struct Result : Progress {
  std::vector<std::string> errors;
};

using ProgressCallback = std::function<void(const Progress&)>;

Result ImportDefaults(
    const std::filesystem::path& game_data_root,
    const std::filesystem::path& object_library_root,
    const ProgressCallback& progress = {});

}  // namespace skate3::drop_item_import
