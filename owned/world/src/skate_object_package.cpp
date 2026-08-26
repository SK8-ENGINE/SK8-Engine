#include "skate/world/skate_object_package.h"

#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace skate::world {

SkateObjectAsset ExtractSkateObjectAsset(MapDefinition package) {
  if (package.editable_objects.size() != 1) {
    throw std::runtime_error(
        "SKATEOBJ must contain exactly one prefab root object");
  }
  if (!package.npc_routes.empty() || !package.kinematic_boxes.empty() ||
      !package.hinged_doors.empty() || !package.water_basins.empty() ||
      !package.raytraced_mirrors.empty() ||
      !package.raytraced_puddles.empty() ||
      !package.moving_light_orbs.empty()) {
    throw std::runtime_error(
        "SKATEOBJ v1 contains unsupported map-level components");
  }

  MapObject& source = package.editable_objects.front();
  if (source.source_first_index != 0 ||
      source.source_index_count != package.render_mesh.indices.size() ||
      source.source_first_collision_triangle != 0 ||
      source.source_collision_triangle_count !=
          package.collision_triangles.size()) {
    throw std::runtime_error(
        "SKATEOBJ prefab root must own all render and collision geometry");
  }

  std::vector<std::uint32_t> rail_indices =
      source.grind_rail_indices;
  std::sort(rail_indices.begin(), rail_indices.end());
  std::vector<std::uint32_t> expected_rails(
      package.grind_rails.size());
  std::iota(expected_rails.begin(), expected_rails.end(), 0u);
  if (rail_indices != expected_rails) {
    throw std::runtime_error(
        "SKATEOBJ prefab root must own every grind spline");
  }
  for (const GrindRail& rail : package.grind_rails) {
    if (!rail.native_segments.empty()) {
      throw std::runtime_error(
          "SKATEOBJ v1 supports authored grind points, not retail native "
          "spline payloads");
    }
  }

  SkateObjectAsset asset;
  asset.name = package.name;
  asset.materials = std::move(package.materials);
  asset.textures = std::move(package.textures);
  asset.object = std::move(source);
  asset.grind_rails = std::move(package.grind_rails);

  const Vec3 authored_origin = asset.object.origin;
  asset.object.origin = {};
  asset.object.source_first_index = 0;
  asset.object.source_index_count =
      static_cast<std::uint32_t>(asset.object.render_mesh.indices.size());
  asset.object.source_first_collision_triangle = 0;
  asset.object.source_collision_triangle_count =
      static_cast<std::uint32_t>(
          asset.object.collision_triangles.size());
  asset.object.grind_rail_indices.resize(asset.grind_rails.size());
  std::iota(asset.object.grind_rail_indices.begin(),
            asset.object.grind_rail_indices.end(), 0u);
  for (GrindRail& rail : asset.grind_rails) {
    for (Vec3& point : rail.points) {
      point = point - authored_origin;
    }
  }
  return asset;
}

SkateObjectAsset LoadSkateObjectPackage(
    const std::filesystem::path& path) {
  if (path.extension() != ".skateobj") {
    throw std::runtime_error(
        "spawnable object package must use the .skateobj extension");
  }
  return ExtractSkateObjectAsset(LoadOwnedMapPackage(path));
}

}  // namespace skate::world
