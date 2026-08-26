#pragma once

#include "skate/world/world_map.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skate::world {

// A spawnable prefab extracted from the constrained .skateobj profile of the
// supported SKATE package. Geometry, collision, and authored grind points are
// in prefab-pivot space. v1 has one root; v2 may contain multiple independent
// roots such as a Box3D cube stack.
struct SkateObjectAsset {
  std::uint32_t format_version = 1;
  std::string name;
  std::vector<SurfaceMaterial> materials;
  std::vector<ImageTexture> textures;
  std::vector<MapObject> objects;
  std::vector<GrindRail> grind_rails;
};

// Validates and extracts one prefab from an already decoded package. Kept
// separate from file I/O so exporter/profile tests can exercise the contract.
SkateObjectAsset ExtractSkateObjectAsset(MapDefinition package);

// Loads a .skateobj file using the shared SKATE package decoder, then enforces
// the object-profile restrictions.
SkateObjectAsset LoadSkateObjectPackage(
    const std::filesystem::path& path);

}  // namespace skate::world
