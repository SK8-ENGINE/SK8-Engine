#pragma once

#include "skate/world/world_map.h"

#include <cstdint>
#include <string>
#include <vector>

namespace skate::world {

// Relocatable Pegasus tSplineData used by Skate's native GrindData runtime.
// The serialized form stores blob-relative offsets; the recomp adapter fixes
// those offsets to persistent guest addresses immediately before registration.
struct GrindSplineBlob {
  std::vector<std::uint8_t> bytes;
  std::uint32_t rail_count = 0;
  std::uint32_t segment_count = 0;
};

struct GrindSplineBuildResult {
  bool ok = false;
  std::string error;
  GrindSplineBlob blob;
};

GrindSplineBuildResult BuildGrindSplineData(
    const MapDefinition& map,
    Vec3 translation = {});

// Applies the asset loader's pointer relocation contract in place. Returns
// false for malformed blobs, overflow, or an already-fixed blob.
bool FixupGrindSplineDataForGuest(
    std::vector<std::uint8_t>& bytes,
    std::uint32_t guest_base);

}  // namespace skate::world
