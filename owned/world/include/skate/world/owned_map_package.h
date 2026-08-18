#pragma once

#include "skate/world/world_map.h"

#include <filesystem>

namespace skate::world {

// Reads the original SKATE package emitted by tools/blender_owned_map.
// Packages are renderer-neutral: one file contains visual triangles with
// UV0/UV1, embedded RGBA8 textures, independent collision triangles, grind
// paths, and spawn/environment metadata.
MapDefinition LoadOwnedMapPackage(const std::filesystem::path& path);

}  // namespace skate::world
