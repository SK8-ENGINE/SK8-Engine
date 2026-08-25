#pragma once

#include "skate/world/world_map.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skate::world {

// A spawnable prefab extracted from the constrained .skateobj profile of the
// SKATE12 package. Geometry, collision, and authored grind points are all in
// the prefab root's local space.
struct SkateObjectAsset {
  std::string name;
  std::vector<SurfaceMaterial> materials;
  std::vector<ImageTexture> textures;
  MapObject object;
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
