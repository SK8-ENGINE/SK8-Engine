#pragma once

#include "skate/world/world_map.h"

#include <cstdint>
#include <filesystem>
#include <span>
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

// Reassigns prefab-local break groups above every group already present in
// the destination map. This keeps separately spawned copies independent
// while preserving grouping between roots inside one prefab instance.
void RemapSkateObjectBreakGroups(SkateObjectAsset &asset,
                                 const MapDefinition &destination);

// Loads a .skateobj file using the shared SKATE package decoder, then enforces
// the object-profile restrictions.
SkateObjectAsset LoadSkateObjectPackage(const std::filesystem::path &path);
SkateObjectAsset LoadSkateObjectPackage(std::span<const std::uint8_t> bytes);

// Writes one portable SKATE14/MOBJ3 prefab package atomically. Geometry and
// collision remain local to each root object while origins and grind rails
// are relative to the package pivot, matching LoadSkateObjectPackage().
void SaveSkateObjectPackage(const std::filesystem::path &path,
                            const SkateObjectAsset &asset);

} // namespace skate::world
