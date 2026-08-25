#pragma once

#include "skate/world/world_map.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate::world {

// RenderWare collision surface IDs pack the audio, physics, and pattern
// channels into the 16-bit value stored on every triangle unit.
constexpr std::uint16_t EncodeRwSurfaceId(std::uint8_t audio,
                                          std::uint8_t physics,
                                          std::uint8_t pattern) {
  return static_cast<std::uint16_t>(
      (audio & 0x7fu) | ((physics & 0x1fu) << 7u) |
      ((pattern & 0x0fu) << 12u));
}

struct RwCollisionBuildOptions {
  // Vertices whose components quantize to the same cell are welded. Welding
  // is required for triangle adjacency and stable edge contacts.
  float weld_epsilon = 0.001f;
  float granularity = 0.001f;
  Vec3 translation;
  std::uint16_t default_surface_id = 0;
  std::unordered_map<MaterialId, std::uint16_t> material_surface_ids;
};

struct RwCollisionMeshBlob {
  // Big-endian RenderWare ClusteredMesh object followed by its KD-tree,
  // cluster table, and cluster data. This is the serialized form: fields
  // that the asset loader normally fixes up are offsets until
  // FixupRwCollisionMeshForGuest is called.
  std::vector<std::uint8_t> bytes;

  std::uint32_t triangle_count = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t cluster_count = 0;
  std::uint32_t maximum_cluster_vertex_count = 0;
  Vec3 bounds_min;
  Vec3 bounds_max;
};

struct RwCollisionBuildResult {
  bool ok = false;
  std::string error;
  RwCollisionMeshBlob mesh;
};

// Builds an rw::collision::ClusteredMesh with an uncompressed unit stream.
// KD leaves are partitioned into native clusters, keeping every cluster below
// the format's 255-vertex limit while preserving one authoritative query tree.
// The representation is native collision data, not a host-side contact shim.
RwCollisionBuildResult BuildRwCollisionMesh(
    const MapDefinition& map,
    const RwCollisionBuildOptions& options = {});

// Validates and adopts an untouched serialized retail ClusteredMesh. This is
// used to distinguish extraction errors from behavior introduced by
// rebuilding retail triangles into a different KD/cluster layout.
RwCollisionBuildResult LoadSerializedRwCollisionMesh(
    std::span<const std::uint8_t> bytes);

// Applies a rigid world-space translation without rebuilding a retail mesh.
// KD topology, cluster boundaries, unit flags, surfaces, group IDs, edge
// metadata, and compressed vertex representation are retained. Translation is
// snapped to the mesh granularity so compressed integer vertices remain exact;
// the snapped value is returned through applied_translation when requested.
//
// This must be called on the serialized form before guest pointer fixup.
bool TranslateSerializedRwCollisionMesh(
    RwCollisionMeshBlob& mesh,
    Vec3 requested_translation,
    Vec3* applied_translation = nullptr,
    std::string* error = nullptr);

// Applies the same mixed pointer/offset contract as Skate's RenderWare asset
// loader. The top-level KD-tree and cluster-table addresses, plus the KD
// branch-record address, become absolute guest pointers. Cluster-table
// elements deliberately remain mesh-relative because native query code adds
// the ClusteredMesh base when resolving each cluster.
//
// The operation is one-way and rejects malformed or already-fixed blobs.
bool FixupRwCollisionMeshForGuest(std::span<std::uint8_t> bytes,
                                  std::uint32_t guest_address);

}  // namespace skate::world
