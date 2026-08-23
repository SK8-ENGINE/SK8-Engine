#include "skate/world/grind_spline.h"
#include "skate/world/owned_map_package.h"
#include "skate/world/render_world.h"
#include "skate/world/rw_collision_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 ||
      (argc == 3 && std::string_view(argv[2]) != "--compile-world")) {
    std::cerr
        << "usage: skate_owned_map_validate <package.skate> "
           "[--compile-world]\n";
    return 2;
  }
  try {
    const skate::world::MapDefinition map =
        skate::world::LoadOwnedMapPackage(std::filesystem::path(argv[1]));
    skate::world::Vec3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    skate::world::Vec3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (const skate::world::RenderVertex& vertex :
         map.render_mesh.vertices) {
      minimum.x = std::min(minimum.x, vertex.position.x);
      minimum.y = std::min(minimum.y, vertex.position.y);
      minimum.z = std::min(minimum.z, vertex.position.z);
      maximum.x = std::max(maximum.x, vertex.position.x);
      maximum.y = std::max(maximum.y, vertex.position.y);
      maximum.z = std::max(maximum.z, vertex.position.z);
    }
    std::uint64_t texture_bytes = 0;
    for (const skate::world::ImageTexture& texture : map.textures) {
      texture_bytes += texture.rgba8.size();
    }
    std::array<std::uint64_t, 3> alpha_modes{};
    for (const skate::world::SurfaceMaterial& material : map.materials) {
      const std::size_t alpha_mode =
          static_cast<std::size_t>(material.alpha_mode);
      if (alpha_mode >= alpha_modes.size()) {
        std::cerr << "SKATE_PACKAGE_FAIL invalid material alpha mode\n";
        return 1;
      }
      ++alpha_modes[alpha_mode];
    }
    std::cout
        << "SKATE_PACKAGE_OK"
        << " name=" << map.name
        << " materials=" << map.materials.size()
        << " alpha_opaque=" << alpha_modes[0]
        << " alpha_mask=" << alpha_modes[1]
        << " alpha_blend=" << alpha_modes[2]
        << " textures=" << map.textures.size()
        << " texture_rgba8_bytes=" << texture_bytes
        << " vertices=" << map.render_mesh.vertices.size()
        << " indices=" << map.render_mesh.indices.size()
        << " triangles=" << map.render_mesh.indices.size() / 3
        << " collision=" << map.collision_triangles.size()
        << " rails=" << map.grind_rails.size()
        << " doors=" << map.hinged_doors.size()
        << " lights=" << map.moving_light_orbs.size()
        << " npc_routes=" << map.npc_routes.size()
        << " bounds_min=" << minimum.x << "," << minimum.y << ","
        << minimum.z
        << " bounds_max=" << maximum.x << "," << maximum.y << ","
        << maximum.z
        << '\n';
    if (argc == 3) {
      {
        const skate::world::RenderWorld render_world =
            skate::world::BuildRenderWorld(map);
        std::cout
            << "SKATE_RENDER_WORLD_OK"
            << " source_triangles=" << render_world.source_triangle_count
            << " output_triangles=" << render_world.output_triangle_count
            << " chunks=" << render_world.chunks.size()
            << '\n';
      }

      if (!map.grind_rails.empty()) {
        const skate::world::GrindSplineBuildResult grind =
            skate::world::BuildGrindSplineData(map, {});
        if (!grind.ok || grind.blob.bytes.empty()) {
          std::cerr
              << "SKATE_GRIND_WORLD_FAIL"
              << " error=" << grind.error
              << '\n';
          return 1;
        }
        std::cout
            << "SKATE_GRIND_WORLD_OK"
            << " rails=" << grind.blob.rail_count
            << " segments=" << grind.blob.segment_count
            << " bytes=" << grind.blob.bytes.size()
            << '\n';
      }

      skate::world::RwCollisionBuildOptions options;
      options.default_surface_id =
          skate::world::EncodeRwSurfaceId(3, 1, 0);
      for (const skate::world::SurfaceMaterial& material : map.materials) {
        options.material_surface_ids.emplace(
            material.id,
            skate::world::EncodeRwSurfaceId(
                material.skate_audio_surface,
                material.skate_physics_surface,
                material.skate_surface_pattern));
      }
      skate::world::RwCollisionBuildResult unified =
          skate::world::BuildRwCollisionMesh(map, options);
      if (unified.ok && !unified.mesh.bytes.empty()) {
        std::cout
            << "SKATE_COLLISION_WORLD_OK"
            << " mode=continuous"
            << " triangles=" << unified.mesh.triangle_count
            << " vertices=" << unified.mesh.vertex_count
            << " clusters=" << unified.mesh.cluster_count
            << " bytes=" << unified.mesh.bytes.size()
            << '\n';
      } else {
        constexpr float cell_size = 256.0f;
        using Cell = std::pair<std::int32_t, std::int32_t>;
        std::map<Cell, std::vector<skate::world::CollisionTriangle>> cells;
        for (const skate::world::CollisionTriangle& triangle :
             map.collision_triangles) {
          const float center_x =
              (triangle.a.x + triangle.b.x + triangle.c.x) / 3.0f;
          const float center_z =
              (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0f;
          cells[{
              static_cast<std::int32_t>(std::floor(center_x / cell_size)),
              static_cast<std::int32_t>(std::floor(center_z / cell_size)),
          }].push_back(triangle);
        }
        std::uint64_t total_triangles = 0;
        std::uint64_t total_vertices = 0;
        std::uint64_t total_clusters = 0;
        std::uint64_t total_bytes = 0;
        for (auto& [cell, triangles] : cells) {
          skate::world::MapDefinition chunk;
          chunk.name =
              "validate_collision_" + std::to_string(cell.first) + "_" +
              std::to_string(cell.second);
          chunk.collision_triangles = std::move(triangles);
          skate::world::RwCollisionBuildResult build =
              skate::world::BuildRwCollisionMesh(chunk, options);
          if (!build.ok || build.mesh.bytes.empty()) {
            std::cerr
                << "SKATE_COLLISION_WORLD_FAIL"
                << " cell=" << cell.first << "," << cell.second
                << " error=" << build.error
                << '\n';
            return 1;
          }
          total_triangles += build.mesh.triangle_count;
          total_vertices += build.mesh.vertex_count;
          total_clusters += build.mesh.cluster_count;
          total_bytes += build.mesh.bytes.size();
        }
        if (total_triangles != map.collision_triangles.size()) {
          std::cerr
              << "SKATE_COLLISION_WORLD_FAIL triangle count changed\n";
          return 1;
        }
        std::cout
            << "SKATE_COLLISION_WORLD_OK"
            << " mode=spatial"
            << " cell_size=" << cell_size
            << " chunks=" << cells.size()
            << " triangles=" << total_triangles
            << " vertices=" << total_vertices
            << " clusters=" << total_clusters
            << " bytes=" << total_bytes
            << " continuous_error=" << unified.error
            << '\n';
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SKATE_PACKAGE_FAIL " << error.what() << '\n';
    return 1;
  }
}
