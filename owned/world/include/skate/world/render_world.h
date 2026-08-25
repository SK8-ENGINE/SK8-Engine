#pragma once

#include "skate/world/world_map.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace skate::world {

struct RenderBatch {
  MaterialId material = 0;
  std::uint32_t first_index = 0;
  std::uint32_t index_count = 0;
  // Exact opposite-wound twins prove that the source supplied explicit
  // front/back faces. Rendering both without culling makes their distinct
  // UVs, normals or lightmaps fight at the same depth.
  bool cull_backfaces = false;
};

struct RenderChunk {
  std::int32_t cell_x = 0;
  std::int32_t cell_z = 0;
  std::uint32_t part = 0;
  Vec3 bounds_min;
  Vec3 bounds_max;
  std::vector<RenderVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<RenderBatch> batches;
};

struct RenderWorld {
  float chunk_size = 0.0f;
  std::size_t source_triangle_count = 0;
  std::size_t output_triangle_count = 0;
  std::size_t backface_culled_material_count = 0;
  std::size_t presentation_surface_count = 0;
  std::vector<RenderChunk> chunks;
};

struct RenderWorldBuildOptions {
  float chunk_size = 64.0f;
  std::size_t maximum_vertices_per_chunk = 65535;
  std::size_t maximum_cells_per_source_triangle = 4096;
  // Optional coarse progress callback for very large authored worlds.
  // Invoked at most once per million source triangles and once at completion.
  std::function<void(std::size_t, std::size_t)> progress;
};

// Compiles arbitrary authored triangles into bounded spatial chunks. Source
// triangles crossing cell boundaries are clipped, not merely assigned by
// their centre, so a large floor cannot silently defeat frustum culling.
RenderWorld BuildRenderWorld(
    const MapDefinition& definition,
    const RenderWorldBuildOptions& options = {});

}  // namespace skate::world
