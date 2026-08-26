#include "skate/world/skate_object_package.h"

#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <zlib.h>

namespace skate::world {
namespace {

constexpr std::uint32_t kStorageRaw = 0;
constexpr std::uint32_t kStorageDeflate = 1;

bool IsSkateObjectPath(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return extension == ".skateobj";
}

void WriteBytes(
    std::ostream& stream, const void* data, std::size_t size) {
  stream.write(
      static_cast<const char*>(data),
      static_cast<std::streamsize>(size));
  if (!stream) {
    throw std::runtime_error("could not write SKATEOBJ package");
  }
}

template <typename T>
void WriteScalar(std::ostream& stream, const T& value) {
  WriteBytes(stream, &value, sizeof(value));
}

void WriteString(std::ostream& stream, const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("SKATEOBJ string exceeds the format limit");
  }
  WriteScalar(stream, static_cast<std::uint32_t>(value.size()));
  WriteBytes(stream, value.data(), value.size());
}

void WriteVec3(std::ostream& stream, Vec3 value) {
  WriteScalar(stream, value.x);
  WriteScalar(stream, value.y);
  WriteScalar(stream, value.z);
}

void WriteStoredPayload(
    std::ostream& stream, std::uint32_t method,
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(
        "SKATEOBJ stored payload exceeds the format limit");
  }
  WriteScalar(stream, method);
  WriteScalar(stream, static_cast<std::uint32_t>(payload.size()));
  WriteBytes(stream, payload.data(), payload.size());
}

void WriteStoredBytes(
    std::ostream& stream, const std::vector<std::uint8_t>& decoded) {
  if (decoded.empty()) {
    WriteStoredPayload(stream, kStorageRaw, decoded);
    return;
  }
  if (decoded.size() > std::numeric_limits<uLong>::max()) {
    throw std::runtime_error(
        "SKATEOBJ payload exceeds the compressor limit");
  }
  uLongf capacity = compressBound(static_cast<uLong>(decoded.size()));
  std::vector<std::uint8_t> compressed(capacity);
  const int status = compress2(
      compressed.data(), &capacity, decoded.data(),
      static_cast<uLong>(decoded.size()), 6);
  if (status != Z_OK) {
    throw std::runtime_error("could not compress SKATEOBJ payload");
  }
  compressed.resize(static_cast<std::size_t>(capacity));
  if (compressed.size() < decoded.size()) {
    WriteStoredPayload(stream, kStorageDeflate, compressed);
  } else {
    WriteStoredPayload(stream, kStorageRaw, decoded);
  }
}

void Append(
    std::vector<std::uint8_t>& bytes, const void* data,
    std::size_t size) {
  const auto* first = static_cast<const std::uint8_t*>(data);
  bytes.insert(bytes.end(), first, first + size);
}

template <typename T>
void AppendScalar(std::vector<std::uint8_t>& bytes, const T& value) {
  Append(bytes, &value, sizeof(value));
}

void AppendString(
    std::vector<std::uint8_t>& bytes, const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("SKATEOBJ string exceeds the format limit");
  }
  const auto size = static_cast<std::uint32_t>(value.size());
  AppendScalar(bytes, size);
  Append(bytes, value.data(), value.size());
}

void AppendVec3(std::vector<std::uint8_t>& bytes, Vec3 value) {
  AppendScalar(bytes, value.x);
  AppendScalar(bytes, value.y);
  AppendScalar(bytes, value.z);
}

std::int8_t PackSnorm(float value) {
  return static_cast<std::int8_t>(std::lround(
      std::clamp(value, -1.0f, 1.0f) * 127.0f));
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

void WriteMaterial(
    std::ostream& stream, const SurfaceMaterial& material) {
  WriteString(stream, material.name);
  WriteScalar(stream, static_cast<std::uint32_t>(material.flags));
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
      stream,
      static_cast<std::uint32_t>(material.skate_audio_surface));
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(material.skate_physics_surface));
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(material.skate_surface_pattern));
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
  for (const RetailTextureBinding& binding :
       material.retail.texture_bindings) {
    WriteString(stream, binding.semantic);
    WriteScalar(stream, binding.texture);
    WriteScalar(stream, binding.uv_set);
    WriteScalar(stream, binding.address_u);
    WriteScalar(stream, binding.address_v);
  }
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(material.retail.parameters.size()));
  for (const RetailMaterialParameter& parameter :
       material.retail.parameters) {
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

std::vector<std::uint8_t> BuildMapObjectPayload(
    const SkateObjectAsset& asset,
    const std::vector<std::uint32_t>& first_indices,
    const std::vector<std::uint32_t>& index_counts,
    const std::vector<std::uint32_t>& first_collision,
    const std::vector<std::uint32_t>& collision_counts) {
  std::vector<std::uint8_t> payload;
  AppendScalar(
      payload, static_cast<std::uint32_t>(asset.objects.size()));
  for (std::size_t index = 0; index < asset.objects.size(); ++index) {
    const MapObject& object = asset.objects[index];
    AppendScalar(payload, static_cast<MapObjectId>(index + 1));
    AppendString(payload, object.name);
    AppendVec3(payload, object.origin);
    AppendScalar(payload, first_indices[index]);
    AppendScalar(payload, index_counts[index]);
    AppendScalar(payload, first_collision[index]);
    AppendScalar(payload, collision_counts[index]);
    AppendScalar(
        payload,
        static_cast<std::uint32_t>(
            object.grind_rail_indices.size()));
    for (const std::uint32_t rail : object.grind_rail_indices) {
      AppendScalar(payload, rail);
    }
    AppendScalar(
        payload,
        static_cast<std::uint32_t>(object.physics.type));
    AppendScalar(
        payload,
        static_cast<std::uint32_t>(object.physics.shape));
    AppendScalar(payload, object.physics.density);
    AppendScalar(payload, object.physics.friction);
    AppendScalar(payload, object.physics.restitution);
    AppendScalar(payload, object.physics.linear_damping);
    AppendScalar(payload, object.physics.angular_damping);
    AppendScalar(payload, object.physics.gravity_scale);
    AppendScalar(payload, object.physics.enable_sleep ? 1u : 0u);
    AppendScalar(payload, object.physics.initially_awake ? 1u : 0u);
  }
  return payload;
}

void WritePackage(
    const std::filesystem::path& path,
    const SkateObjectAsset& asset) {
  if (asset.name.empty() || asset.objects.empty()) {
    throw std::runtime_error(
        "SKATEOBJ requires a name and at least one root object");
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error(
        "could not create SKATEOBJ package: " + path.string());
  }

  std::vector<RenderVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<CollisionTriangle> collision;
  std::vector<std::uint32_t> first_indices;
  std::vector<std::uint32_t> index_counts;
  std::vector<std::uint32_t> first_collision;
  std::vector<std::uint32_t> collision_counts;
  first_indices.reserve(asset.objects.size());
  index_counts.reserve(asset.objects.size());
  first_collision.reserve(asset.objects.size());
  collision_counts.reserve(asset.objects.size());
  for (const MapObject& object : asset.objects) {
    if (object.render_mesh.vertices.empty() ||
        object.render_mesh.indices.empty() ||
        object.render_mesh.indices.size() % 3u != 0) {
      throw std::runtime_error(
          "SKATEOBJ root has invalid render geometry");
    }
    const std::uint32_t vertex_base =
        static_cast<std::uint32_t>(vertices.size());
    first_indices.push_back(
        static_cast<std::uint32_t>(indices.size()));
    index_counts.push_back(
        static_cast<std::uint32_t>(
            object.render_mesh.indices.size()));
    for (RenderVertex vertex : object.render_mesh.vertices) {
      vertex.position = vertex.position + object.origin;
      vertices.push_back(vertex);
    }
    for (const std::uint32_t source : object.render_mesh.indices) {
      if (source >= object.render_mesh.vertices.size()) {
        throw std::runtime_error(
            "SKATEOBJ root index is out of range");
      }
      indices.push_back(vertex_base + source);
    }
    first_collision.push_back(
        static_cast<std::uint32_t>(collision.size()));
    collision_counts.push_back(
        static_cast<std::uint32_t>(
            object.collision_triangles.size()));
    for (CollisionTriangle triangle : object.collision_triangles) {
      triangle.a = triangle.a + object.origin;
      triangle.b = triangle.b + object.origin;
      triangle.c = triangle.c + object.origin;
      collision.push_back(triangle);
    }
  }

  constexpr std::array<char, 8> magic{
      'S', 'K', 'A', 'T', 'E', '1', '4', '\0'};
  constexpr std::uint32_t endian = 0x12345678u;
  WriteBytes(stream, magic.data(), magic.size());
  WriteScalar(stream, endian);
  WriteString(stream, asset.name);
  WriteVec3(stream, {});
  WriteScalar(stream, 0.0f);
  WriteVec3(stream, {0.08f, 0.20f, 0.42f});
  WriteVec3(stream, {0.52f, 0.72f, 0.90f});
  WriteVec3(stream, {0.12f, 0.15f, 0.20f});
  const DayNightCycleDefinition cycle;
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
      static_cast<std::uint32_t>(asset.materials.size()),
      static_cast<std::uint32_t>(asset.textures.size()),
      static_cast<std::uint32_t>(vertices.size()),
      static_cast<std::uint32_t>(indices.size()),
      static_cast<std::uint32_t>(collision.size()),
      static_cast<std::uint32_t>(asset.grind_rails.size()),
      0u, 0u, 0u};
  for (const std::uint32_t count : counts) {
    WriteScalar(stream, count);
  }
  for (const SurfaceMaterial& material : asset.materials) {
    WriteMaterial(stream, material);
  }
  for (const ImageTexture& texture : asset.textures) {
    WriteString(stream, texture.name);
    WriteScalar(stream, texture.width);
    WriteScalar(stream, texture.height);
    WriteScalar(
        stream, static_cast<std::uint32_t>(texture.color_space));
    if (!texture.stored_rgba8.empty()) {
      WriteStoredPayload(
          stream, texture.stored_rgba8_method,
          texture.stored_rgba8);
    } else {
      WriteStoredBytes(stream, texture.rgba8);
    }
  }
  WriteStoredBytes(stream, PackVertices(vertices));
  WriteStoredBytes(stream, PackIndices(indices));
  WriteStoredBytes(stream, PackCollision(collision));
  for (const GrindRail& rail : asset.grind_rails) {
    WriteString(stream, rail.name);
    WriteScalar(stream, rail.closed ? 1u : 0u);
    if (rail.native_segments.empty()) {
      WriteScalar(stream, 0u);
      WriteScalar(
          stream, static_cast<std::uint32_t>(rail.points.size()));
      for (const Vec3 point : rail.points) {
        WriteVec3(stream, point);
      }
    } else {
      WriteScalar(stream, 1u);
      WriteScalar(stream, rail.retail_spline_id);
      WriteScalar(stream, rail.retail_type_signature);
      WriteScalar(stream, rail.retail_flags);
      WriteScalar(stream, rail.retail_trailing_word);
      WriteScalar(
          stream,
          static_cast<std::uint32_t>(
              rail.native_segments.size()));
      for (const NativeGrindSegment& segment :
           rail.native_segments) {
        for (const std::uint32_t word : segment.words) {
          WriteScalar(stream, word);
        }
      }
    }
  }
  const std::vector<std::uint8_t> object_payload =
      BuildMapObjectPayload(
          asset, first_indices, index_counts, first_collision,
          collision_counts);
  WriteScalar(stream, 1u);
  constexpr std::array<char, 4> object_tag{'M', 'O', 'B', 'J'};
  WriteBytes(stream, object_tag.data(), object_tag.size());
  WriteScalar(stream, 3u);
  WriteScalar(
      stream,
      static_cast<std::uint32_t>(object_payload.size()));
  WriteStoredBytes(stream, object_payload);
  stream.close();
  if (!stream) {
    throw std::runtime_error("could not finalize SKATEOBJ package");
  }
}

}  // namespace

void SaveSkateObjectPackage(
    const std::filesystem::path& path,
    const SkateObjectAsset& asset) {
  if (!IsSkateObjectPath(path)) {
    throw std::runtime_error(
        "spawnable object package must use the .skateobj extension");
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    throw std::runtime_error(
        "could not create SKATEOBJ category folder: " +
        error.message());
  }
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  std::filesystem::remove(temporary, error);
  error.clear();
  try {
    WritePackage(temporary, asset);
    // Validate the exact bytes before they become visible to the editor.
    (void)ExtractSkateObjectAsset(
        LoadOwnedMapPackage(temporary));
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
      throw std::runtime_error(
          "could not publish SKATEOBJ package: " + error.message());
    }
  } catch (...) {
    std::filesystem::remove(temporary, error);
    throw;
  }
}

}  // namespace skate::world
