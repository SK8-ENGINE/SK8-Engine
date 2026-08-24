#include "skate/world/render_world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace skate::world {
namespace {

constexpr float kAreaEpsilon = 1.0e-10f;

struct CellKey {
  std::int32_t x = 0;
  std::int32_t z = 0;

  bool operator<(const CellKey& other) const {
    return std::tie(x, z) < std::tie(other.x, other.z);
  }
};

using MaterialGeometry = std::map<MaterialId, std::vector<RenderVertex>>;
using CellGeometry = std::map<CellKey, MaterialGeometry>;

RenderVertex Interpolate(const RenderVertex& a,
                         const RenderVertex& b,
                         float amount) {
  RenderVertex result;
  result.position = a.position + (b.position - a.position) * amount;
  result.normal = Normalize(a.normal + (b.normal - a.normal) * amount);
  result.uv = {
      a.uv.x + (b.uv.x - a.uv.x) * amount,
      a.uv.y + (b.uv.y - a.uv.y) * amount,
  };
  result.material = a.material;
  result.lightmap_uv = {
      a.lightmap_uv.x +
          (b.lightmap_uv.x - a.lightmap_uv.x) * amount,
      a.lightmap_uv.y +
          (b.lightmap_uv.y - a.lightmap_uv.y) * amount,
  };
  result.decal_uv = {
      a.decal_uv.x + (b.decal_uv.x - a.decal_uv.x) * amount,
      a.decal_uv.y + (b.decal_uv.y - a.decal_uv.y) * amount,
  };
  result.tangent_binormal = Normalize(
      a.tangent_binormal +
      (b.tangent_binormal - a.tangent_binormal) * amount);
  result.tangent_handedness =
      std::abs(a.tangent_handedness) >=
              std::abs(b.tangent_handedness)
          ? a.tangent_handedness
          : b.tangent_handedness;
  return result;
}

std::vector<RenderVertex> ClipAxis(
    const std::vector<RenderVertex>& input,
    int axis,
    float boundary,
    bool keep_greater) {
  std::vector<RenderVertex> output;
  if (input.empty()) {
    return output;
  }

  const auto coordinate = [axis](const RenderVertex& vertex) {
    return axis == 0 ? vertex.position.x : vertex.position.z;
  };
  const auto inside = [keep_greater, boundary](float value) {
    return keep_greater ? value >= boundary : value <= boundary;
  };

  RenderVertex previous = input.back();
  float previous_value = coordinate(previous);
  bool previous_inside = inside(previous_value);
  for (const RenderVertex& current : input) {
    const float current_value = coordinate(current);
    const bool current_inside = inside(current_value);
    if (current_inside != previous_inside) {
      const float denominator = current_value - previous_value;
      float amount = 0.0f;
      if (std::abs(denominator) > std::numeric_limits<float>::epsilon()) {
        amount = std::clamp(
            (boundary - previous_value) / denominator, 0.0f, 1.0f);
      }
      RenderVertex intersection =
          Interpolate(previous, current, amount);
      if (axis == 0) {
        intersection.position.x = boundary;
      } else {
        intersection.position.z = boundary;
      }
      output.push_back(intersection);
    }
    if (current_inside) {
      output.push_back(current);
    }
    previous = current;
    previous_value = current_value;
    previous_inside = current_inside;
  }
  return output;
}

std::vector<RenderVertex> ClipToCell(
    std::vector<RenderVertex> polygon,
    float x_min,
    float x_max,
    float z_min,
    float z_max) {
  polygon = ClipAxis(polygon, 0, x_min, true);
  polygon = ClipAxis(polygon, 0, x_max, false);
  polygon = ClipAxis(polygon, 2, z_min, true);
  polygon = ClipAxis(polygon, 2, z_max, false);
  return polygon;
}

void ExpandBounds(RenderChunk& chunk, Vec3 position) {
  if (chunk.vertices.empty()) {
    chunk.bounds_min = position;
    chunk.bounds_max = position;
    return;
  }
  chunk.bounds_min.x = std::min(chunk.bounds_min.x, position.x);
  chunk.bounds_min.y = std::min(chunk.bounds_min.y, position.y);
  chunk.bounds_min.z = std::min(chunk.bounds_min.z, position.z);
  chunk.bounds_max.x = std::max(chunk.bounds_max.x, position.x);
  chunk.bounds_max.y = std::max(chunk.bounds_max.y, position.y);
  chunk.bounds_max.z = std::max(chunk.bounds_max.z, position.z);
}

void AppendTriangle(RenderChunk& chunk,
                    MaterialId material,
                    const RenderVertex& a,
                    const RenderVertex& b,
                    const RenderVertex& c) {
  if (chunk.batches.empty() ||
      chunk.batches.back().material != material) {
    chunk.batches.push_back(
        {material, static_cast<std::uint32_t>(chunk.indices.size()), 0});
  }
  const std::uint32_t first =
      static_cast<std::uint32_t>(chunk.vertices.size());
  for (const RenderVertex& vertex : {a, b, c}) {
    ExpandBounds(chunk, vertex.position);
    chunk.vertices.push_back(vertex);
  }
  chunk.indices.insert(chunk.indices.end(),
                       {first, first + 1, first + 2});
  chunk.batches.back().index_count += 3;
}

}  // namespace

RenderWorld BuildRenderWorld(
    const MapDefinition& definition,
    const RenderWorldBuildOptions& options) {
  if (!std::isfinite(options.chunk_size) ||
      options.chunk_size <= 0.0f) {
    throw std::invalid_argument("render chunk size must be positive");
  }
  if (options.maximum_vertices_per_chunk < 3 ||
      options.maximum_vertices_per_chunk >
          std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("render chunk vertex limit is invalid");
  }
  if (options.maximum_cells_per_source_triangle == 0) {
    throw std::invalid_argument(
        "render source-triangle cell limit must be positive");
  }
  if (definition.render_mesh.indices.size() % 3 != 0) {
    throw std::invalid_argument(
        "render mesh indices must contain complete triangles");
  }

  RenderWorld world;
  world.chunk_size = options.chunk_size;
  world.source_triangle_count =
      definition.render_mesh.indices.size() / 3;
  CellGeometry cells;

  for (std::size_t index = 0;
       index < definition.render_mesh.indices.size();
       index += 3) {
    RenderVertex triangle[3];
    for (std::size_t corner = 0; corner < 3; ++corner) {
      const std::uint32_t source_index =
          definition.render_mesh.indices[index + corner];
      if (source_index >= definition.render_mesh.vertices.size()) {
        throw std::invalid_argument("render mesh index is out of range");
      }
      triangle[corner] =
          definition.render_mesh.vertices[source_index];
    }
    const MaterialId material = triangle[0].material;
    if (material == 0 || triangle[1].material != material ||
        triangle[2].material != material) {
      throw std::invalid_argument(
          "render triangle has zero or mixed material IDs");
    }

    float x_min = triangle[0].position.x;
    float x_max = triangle[0].position.x;
    float z_min = triangle[0].position.z;
    float z_max = triangle[0].position.z;
    for (std::size_t corner = 1; corner < 3; ++corner) {
      x_min = std::min(x_min, triangle[corner].position.x);
      x_max = std::max(x_max, triangle[corner].position.x);
      z_min = std::min(z_min, triangle[corner].position.z);
      z_max = std::max(z_max, triangle[corner].position.z);
    }

    const std::int32_t cell_x_min = static_cast<std::int32_t>(
        std::floor(x_min / options.chunk_size));
    const std::int32_t cell_x_max = static_cast<std::int32_t>(
        std::floor(x_max / options.chunk_size));
    const std::int32_t cell_z_min = static_cast<std::int32_t>(
        std::floor(z_min / options.chunk_size));
    const std::int32_t cell_z_max = static_cast<std::int32_t>(
        std::floor(z_max / options.chunk_size));
    const std::uint64_t cell_count =
        std::uint64_t(std::int64_t(cell_x_max) - cell_x_min + 1) *
        std::uint64_t(std::int64_t(cell_z_max) - cell_z_min + 1);
    if (cell_count > options.maximum_cells_per_source_triangle) {
      throw std::runtime_error(
          "one render triangle spans too many world chunks");
    }

    for (std::int32_t cell_x = cell_x_min;
         cell_x <= cell_x_max; ++cell_x) {
      for (std::int32_t cell_z = cell_z_min;
           cell_z <= cell_z_max; ++cell_z) {
        const float cell_min_x =
            static_cast<float>(cell_x) * options.chunk_size;
        const float cell_min_z =
            static_cast<float>(cell_z) * options.chunk_size;
        std::vector<RenderVertex> polygon = ClipToCell(
            {triangle[0], triangle[1], triangle[2]},
            cell_min_x, cell_min_x + options.chunk_size,
            cell_min_z, cell_min_z + options.chunk_size);
        if (polygon.size() < 3) {
          continue;
        }
        auto& output = cells[{cell_x, cell_z}][material];
        for (std::size_t corner = 1;
             corner + 1 < polygon.size(); ++corner) {
          const RenderVertex& a = polygon[0];
          const RenderVertex& b = polygon[corner];
          const RenderVertex& c = polygon[corner + 1];
          if (LengthSquared(Cross(b.position - a.position,
                                  c.position - a.position)) <=
              kAreaEpsilon) {
            continue;
          }
          output.insert(output.end(), {a, b, c});
          ++world.output_triangle_count;
        }
      }
    }
  }

  for (const auto& [cell, materials] : cells) {
    std::uint32_t part = 0;
    RenderChunk chunk;
    chunk.cell_x = cell.x;
    chunk.cell_z = cell.z;
    chunk.part = part;
    for (const auto& [material, vertices] : materials) {
      for (std::size_t index = 0; index < vertices.size(); index += 3) {
        if (!chunk.vertices.empty() &&
            chunk.vertices.size() + 3 >
                options.maximum_vertices_per_chunk) {
          world.chunks.push_back(std::move(chunk));
          chunk = {};
          chunk.cell_x = cell.x;
          chunk.cell_z = cell.z;
          chunk.part = ++part;
        }
        AppendTriangle(chunk, material, vertices[index],
                       vertices[index + 1], vertices[index + 2]);
      }
    }
    if (!chunk.vertices.empty()) {
      world.chunks.push_back(std::move(chunk));
    }
  }
  return world;
}

}  // namespace skate::world
