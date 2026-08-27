#pragma once

#include "skate/world/world_map.h"

#include <filesystem>
#include <string>

namespace skate::world {

inline constexpr int kLatestSupportedOwnedMapPackageVersion = 14;

constexpr bool IsSupportedOwnedMapPackageVersion(int version) {
  return version >= 1 && version <= kLatestSupportedOwnedMapPackageVersion;
}

// Reads the original SKATE package emitted by tools/blender_owned_map.
// Packages are renderer-neutral: one file contains visual triangles with
// UV0/UV1, embedded RGBA8 textures, independent collision triangles, grind
// paths, and spawn/environment metadata.
MapDefinition LoadOwnedMapPackage(const std::filesystem::path& path);

// Losslessly expands a package-backed texture on demand. The encoded payload
// remains available so renderers can release the large RGBA allocation after
// uploading it and recreate it later if their GPU cache evicts the texture.
bool DecodeOwnedMapTexture(
    const ImageTexture& texture,
    std::string* error = nullptr);
void ReleaseOwnedMapTexturePixels(const ImageTexture& texture);

}  // namespace skate::world
