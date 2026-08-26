#include "skate/world/skate_object_package.h"

#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace skate::world {
namespace {

bool IsSkateObjectPath(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return extension == ".skateobj";
}

void TranslateNativeSegment(
    NativeGrindSegment& segment, Vec3 translation) {
  for (const std::size_t word : {12u, 20u, 24u}) {
    segment.words[word] = std::bit_cast<std::uint32_t>(
        std::bit_cast<float>(segment.words[word]) + translation.x);
    segment.words[word + 1] = std::bit_cast<std::uint32_t>(
        std::bit_cast<float>(segment.words[word + 1]) + translation.y);
    segment.words[word + 2] = std::bit_cast<std::uint32_t>(
        std::bit_cast<float>(segment.words[word + 2]) + translation.z);
  }
}

}  // namespace

SkateObjectAsset ExtractSkateObjectAsset(MapDefinition package) {
  const bool version_2 =
      package.package_version == 14 &&
      package.map_object_schema_version == 3;
  if (package.package_version == 14 && !version_2) {
    throw std::runtime_error(
        "SKATEOBJ v2 requires SKATE14 with MOBJ schema 3");
  }
  if (package.editable_objects.empty() ||
      (!version_2 && package.editable_objects.size() != 1)) {
    throw std::runtime_error(
        version_2
            ? "SKATEOBJ v2 must contain at least one prefab root object"
            : "SKATEOBJ v1 must contain exactly one prefab root object");
  }
  if (!package.npc_routes.empty() || !package.kinematic_boxes.empty() ||
      !package.hinged_doors.empty() || !package.water_basins.empty() ||
      !package.raytraced_mirrors.empty() ||
      !package.raytraced_puddles.empty() ||
      !package.moving_light_orbs.empty()) {
    throw std::runtime_error(
        "SKATEOBJ contains unsupported map-level components");
  }

  std::vector<bool> claimed_indices(
      package.render_mesh.indices.size(), false);
  std::vector<bool> claimed_collision(
      package.collision_triangles.size(), false);
  std::vector<bool> claimed_rails(package.grind_rails.size(), false);
  for (const MapObject& source : package.editable_objects) {
    const std::size_t index_begin = source.source_first_index;
    const std::size_t index_count = source.source_index_count;
    const std::size_t collision_begin =
        source.source_first_collision_triangle;
    const std::size_t collision_count =
        source.source_collision_triangle_count;
    if (index_begin > claimed_indices.size() ||
        index_count > claimed_indices.size() - index_begin ||
        collision_begin > claimed_collision.size() ||
        collision_count > claimed_collision.size() - collision_begin) {
      throw std::runtime_error(
          "SKATEOBJ root ownership range is invalid");
    }
    std::fill(
        claimed_indices.begin() +
            static_cast<std::ptrdiff_t>(index_begin),
        claimed_indices.begin() +
            static_cast<std::ptrdiff_t>(index_begin + index_count),
        true);
    std::fill(
        claimed_collision.begin() +
            static_cast<std::ptrdiff_t>(collision_begin),
        claimed_collision.begin() +
            static_cast<std::ptrdiff_t>(collision_begin + collision_count),
        true);
    for (const std::uint32_t rail_index :
         source.grind_rail_indices) {
      if (rail_index >= claimed_rails.size() ||
          claimed_rails[rail_index]) {
        throw std::runtime_error(
            "SKATEOBJ grind ownership is invalid");
      }
      claimed_rails[rail_index] = true;
    }
  }
  if (std::find(claimed_indices.begin(), claimed_indices.end(), false) !=
          claimed_indices.end() ||
      std::find(
          claimed_collision.begin(), claimed_collision.end(), false) !=
          claimed_collision.end() ||
      std::find(claimed_rails.begin(), claimed_rails.end(), false) !=
          claimed_rails.end()) {
    throw std::runtime_error(
        "SKATEOBJ roots must collectively own all render, collision, and "
        "grind records");
  }
  SkateObjectAsset asset;
  asset.format_version = version_2 ? 2u : 1u;
  asset.name = package.name;
  asset.materials = std::move(package.materials);
  asset.textures = std::move(package.textures);
  asset.objects = std::move(package.editable_objects);
  asset.grind_rails = std::move(package.grind_rails);

  // v1's root origin was its implicit pivot. v2 uses the package spawn
  // marker, allowing each root to retain an independent relative origin.
  const Vec3 authored_pivot =
      version_2 ? package.spawn.position : asset.objects.front().origin;
  for (MapObject& object : asset.objects) {
    object.origin = object.origin - authored_pivot;
    object.source_first_index = 0;
    object.source_index_count =
        static_cast<std::uint32_t>(object.render_mesh.indices.size());
    object.source_first_collision_triangle = 0;
    object.source_collision_triangle_count =
        static_cast<std::uint32_t>(
            object.collision_triangles.size());
  }
  for (GrindRail& rail : asset.grind_rails) {
    for (Vec3& point : rail.points) {
      point = point - authored_pivot;
    }
    for (NativeGrindSegment& segment : rail.native_segments) {
      TranslateNativeSegment(
          segment,
          {-authored_pivot.x, -authored_pivot.y, -authored_pivot.z});
    }
  }
  return asset;
}

void RemapSkateObjectBreakGroups(
    SkateObjectAsset& asset, const MapDefinition& destination) {
  std::uint32_t next_group = 0;
  for (const MapObject& object : destination.editable_objects) {
    next_group = std::max(next_group, object.physics.break_group);
  }

  std::unordered_map<std::uint32_t, std::uint32_t> remapped;
  for (MapObject& object : asset.objects) {
    const std::uint32_t source_group = object.physics.break_group;
    if (source_group == 0) {
      continue;
    }
    const auto found = remapped.find(source_group);
    if (found != remapped.end()) {
      object.physics.break_group = found->second;
      continue;
    }
    if (next_group == std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error(
          "SKATEOBJ break-group namespace is exhausted");
    }
    const std::uint32_t instance_group = ++next_group;
    remapped.emplace(source_group, instance_group);
    object.physics.break_group = instance_group;
  }
}

SkateObjectAsset LoadSkateObjectPackage(
    const std::filesystem::path& path) {
  if (!IsSkateObjectPath(path)) {
    throw std::runtime_error(
        "spawnable object package must use the .skateobj extension");
  }
  return ExtractSkateObjectAsset(LoadOwnedMapPackage(path));
}

}  // namespace skate::world
