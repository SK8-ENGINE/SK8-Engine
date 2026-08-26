#include "skate/world/box3d_physics.h"
#include "skate/world/grind_spline.h"
#include "skate/world/owned_map_package.h"
#include "skate/world/render_world.h"
#include "skate/world/rw_collision_mesh.h"
#include "skate/world/skate_object_package.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
  bool compile_world = false;
  bool object_profile = false;
  std::optional<std::filesystem::path> collision_output;
  for (int argument = 2; argument < argc; ++argument) {
    const std::string_view option = argv[argument];
    if (option == "--compile-world") {
      compile_world = true;
    } else if (option == "--object-profile") {
      object_profile = true;
    } else if (option == "--collision-output" &&
               argument + 1 < argc) {
      collision_output = std::filesystem::path(argv[++argument]);
      compile_world = true;
    } else {
      std::cerr
          << "usage: skate_owned_map_validate <package.skate> "
             "[--compile-world] [--collision-output <mesh.bin>]\n";
      return 2;
    }
  }
  if (argc < 2) {
    std::cerr
        << "usage: skate_owned_map_validate <package.skate> "
           "[--compile-world] [--collision-output <mesh.bin>]\n";
    return 2;
  }
  try {
    const skate::world::MapDefinition map =
        skate::world::LoadOwnedMapPackage(std::filesystem::path(argv[1]));
    if (object_profile) {
      skate::world::SkateObjectAsset object =
          skate::world::LoadSkateObjectPackage(
              std::filesystem::path(argv[1]));
      std::cout << "SKATEOBJ_PROFILE_OK"
                << " name=" << object.name
                << " version=" << object.format_version
                << " roots=" << object.objects.size()
                << " rails=" << object.grind_rails.size()
                << '\n';
      for (const skate::world::MapObject& root : object.objects) {
        std::cout << "SKATEOBJ_ROOT"
                  << " name=" << root.name
                  << " vertices=" << root.render_mesh.vertices.size()
                  << " collision=" << root.collision_triangles.size()
                  << " physics="
                  << static_cast<std::uint32_t>(root.physics.type)
                  << " shape="
                  << static_cast<std::uint32_t>(root.physics.shape)
                  << '\n';
      }
      for (const skate::world::GrindRail& rail : object.grind_rails) {
        std::cout << "SKATEOBJ_GRIND"
                  << " name=" << rail.name
                  << " points=" << rail.points.size();
        if (!rail.points.empty()) {
          const skate::world::Vec3& first = rail.points.front();
          const skate::world::Vec3& last = rail.points.back();
          std::cout << " first=" << first.x << "," << first.y << ","
                    << first.z
                    << " last=" << last.x << "," << last.y << ","
                    << last.z;
        }
        std::cout << '\n';
      }
      const bool has_physics = std::any_of(
          map.editable_objects.begin(), map.editable_objects.end(),
          [](const skate::world::MapObject& root) {
            return root.physics.type !=
                   skate::world::ObjectPhysicsType::Disabled;
          });
      if (has_physics) {
        skate::world::OwnedPhysicsWorld physics;
        physics.Load(map);
        const skate::world::PhysicsTelemetry telemetry =
            physics.Telemetry();
        std::cout << "SKATEOBJ_PHYSICS_OK"
                  << " static=" << telemetry.static_body_count
                  << " dynamic=" << telemetry.dynamic_body_count
                  << " generation=" << telemetry.world_generation
                  << '\n';
      }
    }
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
      std::string texture_error;
      if (!skate::world::DecodeOwnedMapTexture(
              texture, &texture_error)) {
        std::cerr
            << "SKATE_PACKAGE_FAIL texture " << texture.id << " '"
            << texture.name << "' could not be decoded: "
            << texture_error << '\n';
        return 1;
      }
      texture_bytes += texture.rgba8.size();
    }
    std::array<std::uint64_t, 3> alpha_modes{};
    std::array<std::uint64_t, 4> depth_layers{};
    for (const skate::world::SurfaceMaterial& material : map.materials) {
      const std::size_t alpha_mode =
          static_cast<std::size_t>(material.alpha_mode);
      if (alpha_mode >= alpha_modes.size()) {
        std::cerr << "SKATE_PACKAGE_FAIL invalid material alpha mode\n";
        return 1;
      }
      ++alpha_modes[alpha_mode];
      if (material.presentation_depth_layer >= depth_layers.size()) {
        std::cerr << "SKATE_PACKAGE_FAIL invalid material depth layer\n";
        return EXIT_FAILURE;
      }
      ++depth_layers[material.presentation_depth_layer];
    }
    std::cout
        << "SKATE_PACKAGE_OK"
        << " name=" << map.name
        << " materials=" << map.materials.size()
        << " alpha_opaque=" << alpha_modes[0]
        << " alpha_mask=" << alpha_modes[1]
        << " alpha_blend=" << alpha_modes[2]
        << " depth_base=" << depth_layers[0]
        << " depth_cutout=" << depth_layers[1]
        << " depth_overlay=" << depth_layers[2]
        << " depth_blend=" << depth_layers[3]
        << " textures=" << map.textures.size()
        << " texture_rgba8_bytes=" << texture_bytes
        << " vertices=" << map.render_mesh.vertices.size()
        << " indices=" << map.render_mesh.indices.size()
        << " triangles=" << map.render_mesh.indices.size() / 3
        << " collision=" << map.collision_triangles.size()
        << " editable_objects=" << map.editable_objects.size()
        << " rails=" << map.grind_rails.size()
        << " doors=" << map.hinged_doors.size()
        << " lights=" << map.moving_light_orbs.size()
        << " npc_routes=" << map.npc_routes.size()
        << " bounds_min=" << minimum.x << "," << minimum.y << ","
        << minimum.z
        << " bounds_max=" << maximum.x << "," << maximum.y << ","
        << maximum.z
        << '\n';
    if (!map.retail_collision_resource_names.empty()) {
      if (!skate::world::HasRetailCollisionIdentity(map)) {
        std::cerr << "SKATE_RETAIL_COLLISION_IDENTITY_FAIL invalid_table\n";
        return 1;
      }
      std::size_t collision_objects = 0;
      std::size_t maximum_resources = 0;
      for (const skate::world::MapObject &object : map.editable_objects) {
        if (object.source_collision_triangle_count == 0) {
          continue;
        }
        const std::vector<std::uint16_t> resources =
            skate::world::RetailCollisionResourcesForObject(map, object);
        if (resources.empty()) {
          std::cerr << "SKATE_RETAIL_COLLISION_IDENTITY_FAIL object="
                    << object.name << '\n';
          return 1;
        }
        ++collision_objects;
        maximum_resources = std::max(maximum_resources, resources.size());
      }
      std::cout << "SKATE_RETAIL_COLLISION_IDENTITY_OK"
                << " resources=" << map.retail_collision_resource_names.size()
                << " associations=" << map.retail_collision_associations.size()
                << " collision_objects=" << collision_objects
                << " max_resources_per_object=" << maximum_resources << '\n';
    }
    if (compile_world) {
      {
        const skate::world::RenderWorld render_world =
            skate::world::BuildRenderWorld(map);
        std::cout
            << "SKATE_RENDER_WORLD_OK"
            << " source_triangles=" << render_world.source_triangle_count
            << " output_triangles=" << render_world.output_triangle_count
            << " backface_culled_materials="
            << render_world.backface_culled_material_count
            << " presentation_surfaces="
            << render_world.presentation_surface_count
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
        if (collision_output) {
          std::ofstream output(
              *collision_output, std::ios::binary | std::ios::trunc);
          output.write(
              reinterpret_cast<const char*>(unified.mesh.bytes.data()),
              static_cast<std::streamsize>(unified.mesh.bytes.size()));
          if (!output) {
            std::cerr
                << "SKATE_COLLISION_WORLD_FAIL"
                << " error=unable to write collision output\n";
            return 1;
          }
        }
        std::cout
            << "SKATE_COLLISION_WORLD_OK"
            << " mode=continuous"
            << " triangles=" << unified.mesh.triangle_count
            << " vertices=" << unified.mesh.vertex_count
            << " clusters=" << unified.mesh.cluster_count
            << " bytes=" << unified.mesh.bytes.size()
            << '\n';
      } else {
        if (collision_output) {
          std::cerr
              << "SKATE_COLLISION_WORLD_FAIL"
              << " error=continuous collision required for binary output\n";
          return 1;
        }
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
