#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <zlib.h>

namespace {

using skate::world::CollisionTriangle;
using skate::world::ImageTexture;
using skate::world::MapDefinition;
using skate::world::MaterialId;
using skate::world::RenderVertex;
using skate::world::SurfaceMaterial;
using skate::world::TextureId;
using skate::world::Vec3;

constexpr std::uint32_t kStorageRaw = 0;
constexpr std::uint32_t kStorageDeflate = 1;

using OrientedTriangleKey =
    std::array<std::array<std::uint32_t, 3>, 3>;

OrientedTriangleKey OrientedPositionKey(
    Vec3 a, Vec3 b, Vec3 c) {
  const auto point = [](Vec3 value) {
    return std::array<std::uint32_t, 3>{
        std::bit_cast<std::uint32_t>(value.x),
        std::bit_cast<std::uint32_t>(value.y),
        std::bit_cast<std::uint32_t>(value.z)};
  };
  const std::array<std::uint32_t, 3> pa = point(a);
  const std::array<std::uint32_t, 3> pb = point(b);
  const std::array<std::uint32_t, 3> pc = point(c);
  return std::min({
      OrientedTriangleKey{pa, pb, pc},
      OrientedTriangleKey{pb, pc, pa},
      OrientedTriangleKey{pc, pa, pb},
  });
}

struct OrientedTriangleKeyHash {
  std::size_t operator()(
      const OrientedTriangleKey& key) const noexcept {
    std::size_t hash = 1469598103934665603ull;
    for (const auto& point : key) {
      for (const std::uint32_t value : point) {
        hash ^= value;
        hash *= 1099511628211ull;
      }
    }
    return hash;
  }
};

void WriteBytes(
    std::ofstream& stream, const void* data, std::size_t size) {
  stream.write(
      static_cast<const char*>(data),
      static_cast<std::streamsize>(size));
  if (!stream) {
    throw std::runtime_error("could not write output package");
  }
}

template <typename T>
void WriteScalar(std::ofstream& stream, const T& value) {
  WriteBytes(stream, &value, sizeof(value));
}

void WriteString(std::ofstream& stream, const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("string is too large for SKATE");
  }
  WriteScalar(stream, static_cast<std::uint32_t>(value.size()));
  WriteBytes(stream, value.data(), value.size());
}

void WriteVec2(
    std::ofstream& stream, const skate::world::Vec2& value) {
  WriteScalar(stream, value.x);
  WriteScalar(stream, value.y);
}

void WriteVec3(std::ofstream& stream, Vec3 value) {
  WriteScalar(stream, value.x);
  WriteScalar(stream, value.y);
  WriteScalar(stream, value.z);
}

void WriteStoredPayload(
    std::ofstream& stream,
    std::uint32_t method,
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("stored payload is too large for SKATE");
  }
  WriteScalar(stream, method);
  WriteScalar(stream, static_cast<std::uint32_t>(payload.size()));
  WriteBytes(stream, payload.data(), payload.size());
}

void WriteStoredBytes(
    std::ofstream& stream,
    const std::vector<std::uint8_t>& decoded) {
  if (decoded.empty()) {
    WriteStoredPayload(stream, kStorageRaw, decoded);
    return;
  }
  uLongf capacity = compressBound(
      static_cast<uLong>(decoded.size()));
  std::vector<std::uint8_t> compressed(capacity);
  const int status = compress2(
      compressed.data(), &capacity, decoded.data(),
      static_cast<uLong>(decoded.size()), 6);
  if (status != Z_OK) {
    throw std::runtime_error("could not DEFLATE SKATE payload");
  }
  compressed.resize(static_cast<std::size_t>(capacity));
  if (compressed.size() >= decoded.size()) {
    WriteStoredPayload(stream, kStorageRaw, decoded);
  } else {
    WriteStoredPayload(stream, kStorageDeflate, compressed);
  }
}

void WriteMaterial(
    std::ofstream& stream, const SurfaceMaterial& material) {
  WriteString(stream, material.name);
  WriteScalar(
      stream, static_cast<std::uint32_t>(material.flags));
  WriteScalar(stream, material.friction);
  WriteScalar(stream, material.restitution);
  WriteVec3(stream, material.display_color);
  WriteScalar(stream, material.roughness);
  WriteScalar(stream, material.emissive_intensity);
  WriteScalar(stream, material.albedo_texture);
  WriteScalar(stream, material.indirect_lightmap);
  WriteScalar(stream, material.baked_indirect_strength);
  WriteScalar(stream, material.normal_texture);
  WriteScalar(stream, material.orm_texture);
  WriteScalar(stream, material.emissive_texture);
  WriteScalar(
      stream, static_cast<std::uint32_t>(material.alpha_mode));
  WriteScalar(stream, material.alpha_cutoff);
  WriteScalar(
      stream, static_cast<std::uint32_t>(
                  material.skate_audio_surface));
  WriteScalar(
      stream, static_cast<std::uint32_t>(
                  material.skate_physics_surface));
  WriteScalar(
      stream, static_cast<std::uint32_t>(
                  material.skate_surface_pattern));
  WriteScalar(stream, material.presentation_depth_layer);
  WriteScalar(stream, material.retail.enabled ? 1u : 0u);
  if (!material.retail.enabled) {
    return;
  }
  WriteScalar(stream, material.retail.material_guid);
  WriteScalar(stream, material.retail.material_handle);
  WriteScalar(stream, material.retail.material_group_index);
  WriteString(stream, material.retail.shader_name);
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(material.retail.shader_family));
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(material.retail.render_flags));
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(
          material.retail.texture_bindings.size()));
  for (const auto& binding :
       material.retail.texture_bindings) {
    WriteString(stream, binding.semantic);
    WriteScalar(stream, binding.texture);
    WriteScalar(stream, binding.uv_set);
    WriteScalar(stream, binding.address_u);
    WriteScalar(stream, binding.address_v);
  }
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(
          material.retail.parameters.size()));
  for (const auto& parameter : material.retail.parameters) {
    WriteString(stream, parameter.name);
    WriteScalar(
        stream,
        static_cast<std::uint32_t>(parameter.values.size()));
    for (const std::string& value : parameter.values) {
      WriteString(stream, value);
    }
  }
  WriteString(stream, material.retail.source_metadata_json);
}

std::int8_t PackSnorm(float value) {
  return static_cast<std::int8_t>(std::lround(
      std::clamp(value, -1.0f, 1.0f) * 127.0f));
}

void Append(
    std::vector<std::uint8_t>& bytes,
    const void* data,
    std::size_t size) {
  const auto* first = static_cast<const std::uint8_t*>(data);
  bytes.insert(bytes.end(), first, first + size);
}

template <typename T>
void AppendScalar(std::vector<std::uint8_t>& bytes, const T& value) {
  Append(bytes, &value, sizeof(value));
}

std::vector<std::uint8_t> PackVertices(
    const std::vector<RenderVertex>& vertices) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(vertices.size() * 56u);
  for (const RenderVertex& vertex : vertices) {
    Append(bytes, &vertex.position, sizeof(float) * 3u);
    Append(bytes, &vertex.normal, sizeof(float) * 3u);
    Append(bytes, &vertex.uv, sizeof(float) * 2u);
    Append(bytes, &vertex.lightmap_uv, sizeof(float) * 2u);
    AppendScalar(bytes, vertex.material);
    Append(bytes, &vertex.decal_uv, sizeof(float) * 2u);
    const std::array<std::int8_t, 4> tangent{
        PackSnorm(vertex.tangent_binormal.x),
        PackSnorm(vertex.tangent_binormal.y),
        PackSnorm(vertex.tangent_binormal.z),
        PackSnorm(vertex.tangent_handedness)};
    Append(bytes, tangent.data(), tangent.size());
  }
  return bytes;
}

std::vector<std::uint8_t> PackIndices(
    const std::vector<std::uint32_t>& indices) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(indices.size() * sizeof(std::uint32_t));
  Append(
      bytes, indices.data(),
      indices.size() * sizeof(std::uint32_t));
  return bytes;
}

std::vector<std::uint8_t> PackCollision(
    const std::vector<CollisionTriangle>& triangles) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(triangles.size() * 48u);
  for (const CollisionTriangle& triangle : triangles) {
    Append(bytes, &triangle.a, sizeof(float) * 3u);
    Append(bytes, &triangle.b, sizeof(float) * 3u);
    Append(bytes, &triangle.c, sizeof(float) * 3u);
    AppendScalar(bytes, triangle.surface);
    AppendScalar(bytes, triangle.material);
    Append(
        bytes, triangle.native_edge_codes.data(),
        triangle.native_edge_codes.size());
    const std::uint8_t has_codes =
        triangle.has_native_edge_codes ? 1u : 0u;
    AppendScalar(bytes, has_codes);
  }
  return bytes;
}

void SaveDiagnosticPackage(
    const std::filesystem::path& output,
    const MapDefinition& map,
    const std::unordered_set<TextureId>& used_textures) {
  std::ofstream stream(
      output, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error(
        "could not create output package: " + output.string());
  }
  constexpr std::array<char, 8> magic{
      'S', 'K', 'A', 'T', 'E', '1', '3', '\0'};
  constexpr std::uint32_t endian = 0x12345678u;
  WriteBytes(stream, magic.data(), magic.size());
  WriteScalar(stream, endian);
  WriteString(stream, map.name);
  WriteVec3(stream, map.spawn.position);
  WriteScalar(stream, map.spawn.heading_radians);
  WriteVec3(stream, map.sky.zenith_color);
  WriteVec3(stream, map.sky.horizon_color);
  WriteVec3(stream, map.sky.nadir_color);
  const auto& cycle = map.day_night_cycle;
  WriteScalar(stream, cycle.duration_seconds);
  WriteScalar(stream, cycle.start_time_hours);
  WriteScalar(stream, cycle.orbit_azimuth_radians);
  WriteScalar(stream, cycle.end_time_hours);
  WriteScalar(stream, cycle.ping_pong ? 1.0f : 0.0f);
  WriteVec3(stream, cycle.twilight_zenith);
  WriteVec3(stream, cycle.twilight_horizon);
  WriteVec3(stream, cycle.twilight_nadir);
  WriteVec3(stream, cycle.night_zenith);
  WriteVec3(stream, cycle.night_horizon);
  WriteVec3(stream, cycle.night_nadir);
  WriteVec3(stream, cycle.sun_color);
  WriteVec3(stream, cycle.moon_color);
  WriteScalar(stream, cycle.sun_intensity);
  WriteScalar(stream, cycle.moon_intensity);
  WriteScalar(stream, cycle.day_ambient);
  WriteScalar(stream, cycle.night_ambient);
  WriteVec3(stream, cycle.sky_tint);

  const std::array<std::uint32_t, 9> counts{
      static_cast<std::uint32_t>(map.materials.size()),
      static_cast<std::uint32_t>(map.textures.size()),
      static_cast<std::uint32_t>(map.render_mesh.vertices.size()),
      static_cast<std::uint32_t>(map.render_mesh.indices.size()),
      static_cast<std::uint32_t>(map.collision_triangles.size()),
      0u, 0u, 0u, 0u};
  for (const std::uint32_t count : counts) {
    WriteScalar(stream, count);
  }
  for (const SurfaceMaterial& material : map.materials) {
    WriteMaterial(stream, material);
  }
  for (const ImageTexture& texture : map.textures) {
    WriteString(stream, texture.name);
    if (!used_textures.contains(texture.id)) {
      WriteScalar(stream, 0u);
      WriteScalar(stream, 0u);
      WriteScalar(
          stream,
          static_cast<std::uint32_t>(texture.color_space));
      WriteStoredPayload(stream, kStorageRaw, {});
      continue;
    }
    WriteScalar(stream, texture.width);
    WriteScalar(stream, texture.height);
    WriteScalar(
        stream,
        static_cast<std::uint32_t>(texture.color_space));
    if (!texture.stored_rgba8.empty()) {
      WriteStoredPayload(
          stream, texture.stored_rgba8_method,
          texture.stored_rgba8);
    } else {
      WriteStoredBytes(stream, texture.rgba8);
    }
  }
  WriteStoredBytes(
      stream, PackVertices(map.render_mesh.vertices));
  WriteStoredBytes(
      stream, PackIndices(map.render_mesh.indices));
  WriteStoredBytes(
      stream, PackCollision(map.collision_triangles));
  WriteScalar(stream, 0u);  // No extensions in the diagnostic crop.
}

float DistanceSquaredXZ(Vec3 point, Vec3 center) {
  const float x = point.x - center.x;
  const float z = point.z - center.z;
  return x * x + z * z;
}

float EdgeLengthSquared(Vec3 a, Vec3 b) {
  const float x = a.x - b.x;
  const float y = a.y - b.y;
  const float z = a.z - b.z;
  return x * x + y * y + z * z;
}

bool KeepTriangle(
    Vec3 a, Vec3 b, Vec3 c, Vec3 center, float radius) {
  const Vec3 centroid{
      (a.x + b.x + c.x) / 3.0f,
      (a.y + b.y + c.y) / 3.0f,
      (a.z + b.z + c.z) / 3.0f};
  // A test zone is a local volume, not an infinite vertical cylinder.
  // Omitting this band retained every floor and facade stacked above the
  // spawn footprint, which left millions of irrelevant city triangles.
  if (centroid.y < center.y - 16.0f ||
      centroid.y > center.y + 24.0f) {
    return false;
  }
  if (DistanceSquaredXZ(centroid, center) > radius * radius) {
    return false;
  }
  const float maximum_edge = radius * 2.0f;
  const float maximum_edge_squared = maximum_edge * maximum_edge;
  return EdgeLengthSquared(a, b) <= maximum_edge_squared &&
         EdgeLengthSquared(b, c) <= maximum_edge_squared &&
         EdgeLengthSquared(c, a) <= maximum_edge_squared;
}

void AddMaterialTextures(
    const SurfaceMaterial& material,
    std::unordered_set<TextureId>& textures) {
  const std::array<TextureId, 5> generic{
      material.albedo_texture,
      material.indirect_lightmap,
      material.normal_texture,
      material.orm_texture,
      material.emissive_texture};
  for (const TextureId texture : generic) {
    if (texture != 0) {
      textures.insert(texture);
    }
  }
  for (const auto& binding :
       material.retail.texture_bindings) {
    if (binding.texture != 0) {
      textures.insert(binding.texture);
    }
  }
}

std::uint64_t Megabytes(std::uintmax_t bytes) {
  return static_cast<std::uint64_t>(
      (bytes + 1024u * 1024u - 1u) / (1024u * 1024u));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr
        << "usage: skate_owned_map_crop <input.skate> <output.skate> "
           "<radius_metres> [--ground-collision-only|--flat-collision] "
           "[--max-visual-triangles <count>] "
           "[--auto-spawn] "
           "[--center-x <metres> --center-y <metres> "
           "--center-z <metres>]\n";
    return 2;
  }
  try {
    const auto started = std::chrono::steady_clock::now();
    const std::filesystem::path input = argv[1];
    const std::filesystem::path output = argv[2];
    const float radius = std::stof(argv[3]);
    bool ground_collision_only = false;
    bool flat_collision = false;
    bool auto_spawn = false;
    std::size_t maximum_visual_triangles =
        std::numeric_limits<std::size_t>::max();
    std::optional<float> center_x;
    std::optional<float> center_y;
    std::optional<float> center_z;
    for (int argument = 4; argument < argc; ++argument) {
      const std::string_view option = argv[argument];
      if (option == "--ground-collision-only") {
        ground_collision_only = true;
      } else if (option == "--flat-collision") {
        flat_collision = true;
      } else if (option == "--auto-spawn") {
        auto_spawn = true;
      } else if (
          option == "--max-visual-triangles" &&
          argument + 1 < argc) {
        maximum_visual_triangles =
            std::stoull(argv[++argument]);
      } else if (option == "--center-x" && argument + 1 < argc) {
        center_x = std::stof(argv[++argument]);
      } else if (option == "--center-y" && argument + 1 < argc) {
        center_y = std::stof(argv[++argument]);
      } else if (option == "--center-z" && argument + 1 < argc) {
        center_z = std::stof(argv[++argument]);
      } else {
        throw std::runtime_error(
            "unknown crop option: " + std::string(option));
      }
    }
    if (ground_collision_only && flat_collision) {
      throw std::runtime_error(
          "collision crop modes are mutually exclusive");
    }
    const bool any_center =
        center_x.has_value() || center_y.has_value() ||
        center_z.has_value();
    const bool complete_center =
        center_x.has_value() && center_y.has_value() &&
        center_z.has_value();
    if (any_center && !complete_center) {
      throw std::runtime_error(
          "all three center coordinates must be supplied together");
    }
    if (!std::isfinite(radius) || radius <= 1.0f) {
      throw std::runtime_error("crop radius must exceed one metre");
    }

    std::cout
        << "CROP 0% loading " << input.string() << '\n';
    MapDefinition source =
        skate::world::LoadOwnedMapPackage(input);
    const auto loaded = std::chrono::steady_clock::now();
    const Vec3 center = complete_center
                            ? Vec3{*center_x, *center_y, *center_z}
                            : source.spawn.position;
    std::cout
        << "CROP 20% loaded; center=" << center.x << "," << center.y
        << "," << center.z << " radius=" << radius << "m\n";

    MapDefinition cropped;
    cropped.name =
        source.name.ends_with(" Test Zone")
            ? source.name
            : source.name + " Test Zone";
    cropped.spawn = source.spawn;
    cropped.spawn.position = center;
    cropped.sky = source.sky;
    cropped.sun = source.sun;
    cropped.day_night_cycle = source.day_night_cycle;
    cropped.weather = source.weather;

    struct VisualCandidate {
      float distance_squared = 0.0f;
      std::size_t source_index = 0;
    };
    std::vector<VisualCandidate> visual_candidates;
    visual_candidates.reserve(std::min<std::size_t>(
        source.render_mesh.indices.size() / 3u,
        maximum_visual_triangles ==
                std::numeric_limits<std::size_t>::max()
            ? 4u * 1024u * 1024u
            : maximum_visual_triangles * 2u));
    std::unordered_set<MaterialId> used_materials;
    std::unordered_set<
        OrientedTriangleKey, OrientedTriangleKeyHash>
        oriented_visual_triangles;
    oriented_visual_triangles.reserve(
        source.render_mesh.indices.size() / 3u);
    std::size_t duplicate_visual_triangles = 0;
    const auto& source_indices = source.render_mesh.indices;
    for (std::size_t index = 0;
         index + 2 < source_indices.size(); index += 3) {
      const std::array<std::uint32_t, 3> old_indices{
          source_indices[index],
          source_indices[index + 1],
          source_indices[index + 2]};
      const RenderVertex& a =
          source.render_mesh.vertices[old_indices[0]];
      const RenderVertex& b =
          source.render_mesh.vertices[old_indices[1]];
      const RenderVertex& c =
          source.render_mesh.vertices[old_indices[2]];
      if (!KeepTriangle(
              a.position, b.position, c.position, center, radius)) {
        continue;
      }
      if (!oriented_visual_triangles
               .insert(OrientedPositionKey(
                   a.position, b.position, c.position))
               .second) {
        ++duplicate_visual_triangles;
        continue;
      }
      const Vec3 centroid{
          (a.position.x + b.position.x + c.position.x) / 3.0f,
          (a.position.y + b.position.y + c.position.y) / 3.0f,
          (a.position.z + b.position.z + c.position.z) / 3.0f};
      visual_candidates.push_back(
          {DistanceSquaredXZ(centroid, center), index});
    }
    if (visual_candidates.size() > maximum_visual_triangles) {
      std::nth_element(
          visual_candidates.begin(),
          visual_candidates.begin() +
              static_cast<std::ptrdiff_t>(
                  maximum_visual_triangles),
          visual_candidates.end(),
          [](const VisualCandidate& left,
             const VisualCandidate& right) {
            if (left.distance_squared != right.distance_squared) {
              return left.distance_squared < right.distance_squared;
            }
            return left.source_index < right.source_index;
          });
      visual_candidates.resize(maximum_visual_triangles);
    }
    std::sort(
        visual_candidates.begin(), visual_candidates.end(),
        [](const VisualCandidate& left,
           const VisualCandidate& right) {
          return left.source_index < right.source_index;
        });

    std::unordered_map<std::uint32_t, std::uint32_t> vertex_remap;
    vertex_remap.reserve(visual_candidates.size() * 2u);
    for (const VisualCandidate& candidate : visual_candidates) {
      const std::size_t index = candidate.source_index;
      const std::array<std::uint32_t, 3> old_indices{
          source_indices[index],
          source_indices[index + 1],
          source_indices[index + 2]};
      for (const std::uint32_t old_index : old_indices) {
        auto [entry, inserted] = vertex_remap.emplace(
            old_index,
            static_cast<std::uint32_t>(
                cropped.render_mesh.vertices.size()));
        if (inserted) {
          const RenderVertex& vertex =
              source.render_mesh.vertices[old_index];
          cropped.render_mesh.vertices.push_back(vertex);
          used_materials.insert(vertex.material);
        }
        cropped.render_mesh.indices.push_back(entry->second);
      }
    }
    std::cout
        << "CROP 55% visuals selected: "
        << cropped.render_mesh.indices.size() / 3u
        << " triangles, " << cropped.render_mesh.vertices.size()
        << " vertices (same-wound duplicates removed "
        << duplicate_visual_triangles << ")\n";

    std::size_t collision_rejected_by_ground_filter = 0;
    if (flat_collision) {
      const float floor_y = center.y - 5.0f;
      const float extent = radius;
      const MaterialId material =
          used_materials.empty() ? 1u : *used_materials.begin();
      const std::uint32_t surface = 1u;
      cropped.collision_triangles = {
          CollisionTriangle{
              {center.x - extent, floor_y, center.z - extent},
              {center.x + extent, floor_y, center.z + extent},
              {center.x + extent, floor_y, center.z - extent},
              {0.0f, 1.0f, 0.0f}, surface, material},
          CollisionTriangle{
              {center.x - extent, floor_y, center.z - extent},
              {center.x - extent, floor_y, center.z + extent},
              {center.x + extent, floor_y, center.z + extent},
              {0.0f, 1.0f, 0.0f}, surface, material},
      };
      used_materials.insert(material);
    } else {
      for (const CollisionTriangle& triangle :
           source.collision_triangles) {
        if (!KeepTriangle(
                triangle.a, triangle.b, triangle.c, center, radius)) {
          continue;
        }
        const float centroid_y =
            (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0f;
        if (ground_collision_only &&
            (triangle.normal.y < 0.15f ||
             centroid_y < center.y - 16.0f ||
             centroid_y > center.y + 24.0f)) {
          ++collision_rejected_by_ground_filter;
          continue;
        }
        cropped.collision_triangles.push_back(triangle);
        used_materials.insert(triangle.material);
      }
    }
    if (cropped.render_mesh.indices.empty()) {
      throw std::runtime_error(
          "crop contained no visual triangles");
    }
    if (cropped.collision_triangles.empty()) {
      throw std::runtime_error(
          "crop contained no usable collision triangles");
    }
    if (auto_spawn) {
      const CollisionTriangle* selected = nullptr;
      float selected_score = -1.0f;
      float selected_area = 0.0f;
      Vec3 selected_centroid{};
      for (const CollisionTriangle& triangle :
           cropped.collision_triangles) {
        if (triangle.normal.y < 0.75f) {
          continue;
        }
        const float cross_xz =
            (triangle.b.x - triangle.a.x) *
                (triangle.c.z - triangle.a.z) -
            (triangle.b.z - triangle.a.z) *
                (triangle.c.x - triangle.a.x);
        const float area = std::abs(cross_xz) * 0.5f;
        if (area < 1.0f) {
          continue;
        }
        const Vec3 centroid{
            (triangle.a.x + triangle.b.x + triangle.c.x) / 3.0f,
            (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0f,
            (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0f};
        const float distance =
            std::sqrt(DistanceSquaredXZ(centroid, center));
        if (distance > radius * 0.75f) {
          continue;
        }
        // Prefer a broad, nearly horizontal surface near the centre while
        // capping the area contribution so a giant terrain/LOD triangle
        // cannot dominate every ordinary road or pavement candidate.
        const float score =
            std::min(area, 50.0f) * triangle.normal.y /
            (1.0f + distance * 0.04f);
        if (score > selected_score) {
          selected = &triangle;
          selected_score = score;
          selected_area = area;
          selected_centroid = centroid;
        }
      }
      if (selected == nullptr) {
        throw std::runtime_error(
            "no broad upward-facing surface was suitable for auto spawn");
      }
      cropped.spawn.position = {
          selected_centroid.x,
          selected_centroid.y + 2.25f,
          selected_centroid.z};
      std::cout
          << "CROP safe spawn: ground=("
          << selected_centroid.x << "," << selected_centroid.y << ","
          << selected_centroid.z << ") spawn=("
          << cropped.spawn.position.x << ","
          << cropped.spawn.position.y << ","
          << cropped.spawn.position.z << ") area=" << selected_area
          << " normal_y=" << selected->normal.y
          << " material=" << selected->material << '\n';
    }
    std::cout
        << "CROP 75% collision selected: "
        << cropped.collision_triangles.size()
        << " triangles (ground-filter rejected "
        << collision_rejected_by_ground_filter << ")\n";

    std::unordered_set<TextureId> used_textures;
    for (const MaterialId material_id : used_materials) {
      if (material_id == 0 ||
          material_id > source.materials.size()) {
        throw std::runtime_error(
            "selected geometry references an invalid material");
      }
      AddMaterialTextures(
          source.materials[material_id - 1], used_textures);
    }
    cropped.materials = std::move(source.materials);
    cropped.textures = std::move(source.textures);
    std::cout
        << "CROP 82% retaining " << used_materials.size()
        << " materials' " << used_textures.size()
        << " referenced textures\n";

    SaveDiagnosticPackage(output, cropped, used_textures);
    const auto finished = std::chrono::steady_clock::now();
    const double load_seconds =
        std::chrono::duration<double>(loaded - started).count();
    const double total_seconds =
        std::chrono::duration<double>(finished - started).count();
    std::cout
        << "CROP 100% wrote " << output.string()
        << " (" << Megabytes(std::filesystem::file_size(output))
        << " MiB) load_seconds=" << load_seconds
        << " total_seconds=" << total_seconds << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CROP_FAIL " << error.what() << '\n';
    return 1;
  }
}
