#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zlib.h>

namespace skate::world {
namespace {

constexpr std::array<char, 8> kMagicV1 = {
    'S', 'K', 'A', 'T', 'E', '0', '1', '\0'};
constexpr std::array<char, 8> kMagicV2 = {
    'S', 'K', 'A', 'T', 'E', '0', '2', '\0'};
constexpr std::array<char, 8> kMagicV3 = {
    'S', 'K', 'A', 'T', 'E', '0', '3', '\0'};
constexpr std::array<char, 8> kMagicV4 = {
    'S', 'K', 'A', 'T', 'E', '0', '4', '\0'};
constexpr std::array<char, 8> kMagicV5 = {
    'S', 'K', 'A', 'T', 'E', '0', '5', '\0'};
constexpr std::array<char, 8> kMagicV6 = {
    'S', 'K', 'A', 'T', 'E', '0', '6', '\0'};
constexpr std::array<char, 8> kMagicV7 = {
    'S', 'K', 'A', 'T', 'E', '0', '7', '\0'};
constexpr std::array<char, 8> kMagicV8 = {
    'S', 'K', 'A', 'T', 'E', '0', '8', '\0'};
constexpr std::array<char, 8> kMagicV9 = {
    'S', 'K', 'A', 'T', 'E', '0', '9', '\0'};
constexpr std::array<char, 8> kMagicV10 = {
    'S', 'K', 'A', 'T', 'E', '1', '0', '\0'};
constexpr std::array<char, 8> kMagicV11 = {
    'S', 'K', 'A', 'T', 'E', '1', '1', '\0'};
constexpr std::array<char, 8> kMagicV12 = {
    'S', 'K', 'A', 'T', 'E', '1', '2', '\0'};
constexpr std::array<char, 8> kMagicV13 = {
    'S', 'K', 'A', 'T', 'E', '1', '3', '\0'};
constexpr std::array<char, 8> kMagicV14 = {
    'S', 'K', 'A', 'T', 'E', '1', '4', '\0'};
constexpr std::uint32_t kEndianMarker = 0x12345678u;
constexpr std::uint32_t kStorageRaw = 0;
constexpr std::uint32_t kStorageDeflate = 1;
constexpr std::uint32_t kMaximumCount = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaximumGeometryCount = 64u * 1024u * 1024u;
constexpr std::uint32_t kMaximumIndexCount = 128u * 1024u * 1024u;
constexpr std::uint32_t kMaximumTextureDimension = 8192u;
constexpr std::uint32_t kMaximumTextureBytes =
    kMaximumTextureDimension * kMaximumTextureDimension * 4u;
constexpr std::uint32_t kMaximumStringBytes = 64u * 1024u;
constexpr std::uint32_t kMaximumMetadataBytes = 128u * 1024u * 1024u;
constexpr std::uint64_t kMaximumPackageBytes = 4ull * 1024ull * 1024ull * 1024ull;
constexpr float kAreaEpsilon = 1.0e-10f;

std::uint32_t InferPresentationDepthLayer(
    const SurfaceMaterial& material) {
  if (material.alpha_mode == SurfaceMaterial::AlphaMode::Blend) {
    return 3;
  }
  std::string lower_name = material.name;
  std::transform(
      lower_name.begin(), lower_name.end(), lower_name.begin(),
      [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
      });
  constexpr std::array<const char*, 13> kOverlayNames = {
      "sign", "poster", "billboard", "adbord", "advert",
      "banner", "logo", "decal", "graffiti", "sticker",
      "plaque", "letter", "neon"};
  for (const char* token : kOverlayNames) {
    if (lower_name.find(token) != std::string::npos) {
      return 2;
    }
  }
  return material.alpha_mode == SurfaceMaterial::AlphaMode::Mask ? 1 : 0;
}

class Reader {
 public:
  explicit Reader(std::vector<std::uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  template <typename T>
  T Scalar() {
    static_assert(std::is_trivially_copyable_v<T>);
    T result{};
    Bytes(&result, sizeof(result));
    return result;
  }

  Vec2 Vector2() {
    return {Scalar<float>(), Scalar<float>()};
  }

  Vec3 Vector3() {
    return {Scalar<float>(), Scalar<float>(), Scalar<float>()};
  }

  std::string String() {
    const std::uint32_t size = Scalar<std::uint32_t>();
    if (size > kMaximumStringBytes) {
      throw std::runtime_error("SKATE string exceeds the format limit");
    }
    std::string result(size, '\0');
    if (size != 0) {
      Bytes(result.data(), size);
    }
    return result;
  }

  std::vector<std::uint8_t> ByteVector(std::size_t size) {
    std::vector<std::uint8_t> result(size);
    if (size != 0) {
      Bytes(result.data(), size);
    }
    return result;
  }

  std::span<const std::uint8_t> View(std::size_t size) {
    if (size > bytes_.size() - offset_) {
      throw std::runtime_error("SKATE package is truncated");
    }
    const std::span<const std::uint8_t> result(
        bytes_.data() + offset_, size);
    offset_ += size;
    return result;
  }

  void Bytes(void* destination, std::size_t size) {
    if (size > bytes_.size() - offset_) {
      throw std::runtime_error("SKATE package is truncated");
    }
    std::memcpy(destination, bytes_.data() + offset_, size);
    offset_ += size;
  }

  void Skip(std::size_t size) {
    if (size > bytes_.size() - offset_) {
      throw std::runtime_error("SKATE package is truncated");
    }
    offset_ += size;
  }

  void RequireEnd() const {
    if (offset_ != bytes_.size()) {
      throw std::runtime_error("SKATE package has trailing bytes");
    }
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

struct StoredPayload {
  std::uint32_t method = kStorageRaw;
  std::size_t expected_size = 0;
  std::vector<std::uint8_t> bytes;
};

StoredPayload ReadStoredPayload(
    Reader& reader,
    std::size_t expected_size,
    const char* label) {
  const std::uint32_t method = reader.Scalar<std::uint32_t>();
  const std::uint32_t stored_size = reader.Scalar<std::uint32_t>();
  StoredPayload payload{
      method, expected_size, reader.ByteVector(stored_size)};
  if (method == kStorageRaw) {
    if (payload.bytes.size() != expected_size) {
      throw std::runtime_error(
          std::string("SKATE ") + label + " raw size is invalid");
    }
    return payload;
  }
  if (method != kStorageDeflate) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " storage method is unsupported");
  }
  return payload;
}

std::vector<std::uint8_t> DecodeStoredPayload(
    StoredPayload payload,
    const char* label) {
  if (payload.method == kStorageRaw) {
    return std::move(payload.bytes);
  }
  const std::size_t expected_size = payload.expected_size;
  if (expected_size > std::numeric_limits<uLongf>::max()) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " decoded size is invalid");
  }
  std::vector<std::uint8_t> decoded(expected_size);
  uLongf decoded_size = static_cast<uLongf>(expected_size);
  const int result = uncompress(
      decoded.data(),
      &decoded_size,
      payload.bytes.data(),
      static_cast<uLong>(payload.bytes.size()));
  if (result != Z_OK || decoded_size != expected_size) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " DEFLATE payload is invalid");
  }
  return decoded;
}

std::vector<std::uint8_t> ReadStoredBytes(
    Reader& reader,
    std::size_t expected_size,
    const char* label) {
  return DecodeStoredPayload(
      ReadStoredPayload(reader, expected_size, label), label);
}

void SkipStoredBytes(Reader& reader,
                     std::size_t expected_size,
                     const char* label) {
  const std::uint32_t method = reader.Scalar<std::uint32_t>();
  const std::uint32_t stored_size = reader.Scalar<std::uint32_t>();
  if (method == kStorageRaw && stored_size != expected_size) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " raw size is invalid");
  }
  if (method != kStorageRaw && method != kStorageDeflate) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " storage method is unsupported");
  }
  reader.Skip(stored_size);
}

float DecodeSnorm8(std::uint32_t packed, std::uint32_t shift) {
  const auto value = static_cast<std::int8_t>(
      static_cast<std::uint8_t>((packed >> shift) & 0xFFu));
  return std::clamp(static_cast<float>(value) / 127.0f, -1.0f, 1.0f);
}

template <typename Function>
void ParallelRanges(std::size_t count, Function function) {
  if (count == 0) {
    return;
  }
  const std::size_t worker_count = std::min<std::size_t>(
      count,
      std::min<std::size_t>(
          std::max(1u, std::thread::hardware_concurrency()), 8u));
  if (worker_count == 1 || count < 65536) {
    function(0, count);
    return;
  }
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    const std::size_t first = count * worker / worker_count;
    const std::size_t last = count * (worker + 1) / worker_count;
    workers.emplace_back(function, first, last);
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
}

struct PackedRenderVertexV9 {
  float position[3];
  float normal[3];
  float uv[2];
  float lightmap_uv[2];
  std::uint32_t material;
};
static_assert(sizeof(PackedRenderVertexV9) == 44);

struct PackedRenderVertexV12 {
  PackedRenderVertexV9 base;
  float decal_uv[2];
  std::uint32_t packed_tangent;
};
static_assert(sizeof(PackedRenderVertexV12) == 56);

struct PackedCollisionTriangleV9 {
  float vertices[9];
  std::uint32_t surface;
  std::uint32_t material;
};
static_assert(sizeof(PackedCollisionTriangleV9) == 44);

struct PackedCollisionTriangleV11 {
  PackedCollisionTriangleV9 base;
  std::uint8_t edge_codes[3];
  std::uint8_t has_edge_codes;
};
static_assert(sizeof(PackedCollisionTriangleV11) == 48);

void ReadPackedTangentFrame(Reader& reader, RenderVertex& vertex) {
  const std::uint32_t packed = reader.Scalar<std::uint32_t>();
  vertex.tangent_binormal = {
      DecodeSnorm8(packed, 0u),
      DecodeSnorm8(packed, 8u),
      DecodeSnorm8(packed, 16u),
  };
  vertex.tangent_handedness = DecodeSnorm8(packed, 24u);
}

void ReadRenderVertices(
    Reader& reader,
    std::uint32_t count,
    std::vector<RenderVertex>& destination,
    bool has_retail_channels = false) {
  const std::size_t first_destination = destination.size();
  destination.resize(first_destination + count);
  if (has_retail_channels) {
    const std::span<const std::uint8_t> bytes = reader.View(
        std::size_t(count) * sizeof(PackedRenderVertexV12));
    ParallelRanges(count, [&](std::size_t first, std::size_t last) {
      for (std::size_t index = first; index < last; ++index) {
        PackedRenderVertexV12 source;
        std::memcpy(
            &source,
            bytes.data() + index * sizeof(source),
            sizeof(source));
        RenderVertex& vertex = destination[first_destination + index];
        vertex.position = {
            source.base.position[0], source.base.position[1],
            source.base.position[2]};
        vertex.normal = {
            source.base.normal[0], source.base.normal[1],
            source.base.normal[2]};
        vertex.uv = {source.base.uv[0], source.base.uv[1]};
        vertex.material = source.base.material;
        vertex.lightmap_uv = {
            source.base.lightmap_uv[0], source.base.lightmap_uv[1]};
        vertex.decal_uv = {
            source.decal_uv[0], source.decal_uv[1]};
        vertex.tangent_binormal = {
            DecodeSnorm8(source.packed_tangent, 0u),
            DecodeSnorm8(source.packed_tangent, 8u),
            DecodeSnorm8(source.packed_tangent, 16u)};
        vertex.tangent_handedness =
            DecodeSnorm8(source.packed_tangent, 24u);
      }
    });
    return;
  }

  const std::span<const std::uint8_t> bytes =
      reader.View(std::size_t(count) * sizeof(PackedRenderVertexV9));
  ParallelRanges(count, [&](std::size_t first, std::size_t last) {
    for (std::size_t index = first; index < last; ++index) {
      PackedRenderVertexV9 source;
      std::memcpy(
          &source,
          bytes.data() + index * sizeof(source),
          sizeof(source));
      RenderVertex& vertex = destination[first_destination + index];
      vertex.position = {
          source.position[0], source.position[1], source.position[2]};
      vertex.normal = {
          source.normal[0], source.normal[1], source.normal[2]};
      vertex.uv = {source.uv[0], source.uv[1]};
      vertex.material = source.material;
      vertex.lightmap_uv = {
          source.lightmap_uv[0], source.lightmap_uv[1]};
      vertex.decal_uv = vertex.uv;
    }
  });
}

void ReadIndices(
    Reader& reader,
    std::uint32_t count,
    std::vector<std::uint32_t>& destination) {
  const std::size_t first_destination = destination.size();
  destination.resize(first_destination + count);
  reader.Bytes(
      destination.data() + first_destination,
      std::size_t(count) * sizeof(std::uint32_t));
}

void ReadCollisionTriangles(
    Reader& reader,
    std::uint32_t count,
    std::vector<CollisionTriangle>& destination,
    bool has_native_edge_codes = false) {
  const std::size_t first_destination = destination.size();
  destination.resize(first_destination + count);
  if (has_native_edge_codes) {
    const std::span<const std::uint8_t> bytes = reader.View(
        std::size_t(count) * sizeof(PackedCollisionTriangleV11));
    ParallelRanges(count, [&](std::size_t first, std::size_t last) {
      for (std::size_t index = first; index < last; ++index) {
        PackedCollisionTriangleV11 source;
        std::memcpy(
            &source,
            bytes.data() + index * sizeof(source),
            sizeof(source));
        CollisionTriangle& triangle =
            destination[first_destination + index];
        triangle.a = {
            source.base.vertices[0], source.base.vertices[1],
            source.base.vertices[2]};
        triangle.b = {
            source.base.vertices[3], source.base.vertices[4],
            source.base.vertices[5]};
        triangle.c = {
            source.base.vertices[6], source.base.vertices[7],
            source.base.vertices[8]};
        triangle.surface = source.base.surface;
        triangle.material = source.base.material;
        std::copy(
            std::begin(source.edge_codes),
            std::end(source.edge_codes),
            triangle.native_edge_codes.begin());
        triangle.has_native_edge_codes =
            source.has_edge_codes != 0;
      }
    });
    return;
  }

  const std::span<const std::uint8_t> bytes = reader.View(
      std::size_t(count) * sizeof(PackedCollisionTriangleV9));
  ParallelRanges(count, [&](std::size_t first, std::size_t last) {
    for (std::size_t index = first; index < last; ++index) {
      PackedCollisionTriangleV9 source;
      std::memcpy(
          &source,
          bytes.data() + index * sizeof(source),
          sizeof(source));
      CollisionTriangle& triangle =
          destination[first_destination + index];
      triangle.a = {
          source.vertices[0], source.vertices[1], source.vertices[2]};
      triangle.b = {
          source.vertices[3], source.vertices[4], source.vertices[5]};
      triangle.c = {
          source.vertices[6], source.vertices[7], source.vertices[8]};
      triangle.surface = source.surface;
      triangle.material = source.material;
    }
  });
}

bool Finite(Vec2 value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

void RequireCount(std::uint32_t count,
                  const char* label,
                  std::uint32_t maximum = kMaximumCount) {
  if (count > maximum) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " count exceeds the format limit");
  }
}

const SurfaceMaterial* FindMaterial(const MapDefinition& map,
                                    MaterialId id) {
  const auto found = std::find_if(
      map.materials.begin(), map.materials.end(),
      [id](const SurfaceMaterial& material) {
        return material.id == id;
      });
  return found == map.materials.end() ? nullptr : &*found;
}

void ReadMapObjects(std::vector<std::uint8_t> payload,
                    std::uint32_t schema, MapDefinition& map) {
  if (schema < 1 || schema > 3) {
    throw std::runtime_error(
        "SKATE MOBJ extension uses an unsupported schema");
  }
  Reader reader(std::move(payload));
  const std::uint32_t object_count = reader.Scalar<std::uint32_t>();
  RequireCount(object_count, "map object");
  std::unordered_set<MapObjectId> ids;
  std::unordered_set<std::string> names;
  std::vector<bool> claimed_indices(map.render_mesh.indices.size(), false);
  std::vector<bool> claimed_collision(
      map.collision_triangles.size(), false);
  std::vector<bool> claimed_grinds(map.grind_rails.size(), false);
  map.editable_objects.reserve(object_count);

  for (std::uint32_t object_index = 0;
       object_index < object_count; ++object_index) {
    MapObject object;
    object.id = reader.Scalar<MapObjectId>();
    object.name = reader.String();
    object.origin = reader.Vector3();
    object.source_first_index = reader.Scalar<std::uint32_t>();
    object.source_index_count = reader.Scalar<std::uint32_t>();
    object.source_first_collision_triangle =
        reader.Scalar<std::uint32_t>();
    object.source_collision_triangle_count =
        reader.Scalar<std::uint32_t>();
    if (schema >= 2) {
      const std::uint32_t grind_count =
          reader.Scalar<std::uint32_t>();
      RequireCount(grind_count, "map object grind rail");
      object.grind_rail_indices.reserve(grind_count);
      for (std::uint32_t grind = 0; grind < grind_count; ++grind) {
        const std::uint32_t rail_index =
            reader.Scalar<std::uint32_t>();
        if (rail_index >= map.grind_rails.size() ||
            claimed_grinds[rail_index]) {
          throw std::runtime_error(
              "SKATE map object grind association is invalid");
        }
        claimed_grinds[rail_index] = true;
        object.grind_rail_indices.push_back(rail_index);
      }
    }
    if (schema >= 3) {
      const std::uint32_t physics_type =
          reader.Scalar<std::uint32_t>();
      const std::uint32_t collision_shape =
          reader.Scalar<std::uint32_t>();
      if (physics_type >
              static_cast<std::uint32_t>(ObjectPhysicsType::Dynamic) ||
          collision_shape >
              static_cast<std::uint32_t>(
                  ObjectCollisionShape::ConvexHull)) {
        throw std::runtime_error(
            "SKATE map object physics enum is invalid");
      }
      object.physics.type =
          static_cast<ObjectPhysicsType>(physics_type);
      object.physics.shape =
          static_cast<ObjectCollisionShape>(collision_shape);
      object.physics.density = reader.Scalar<float>();
      object.physics.friction = reader.Scalar<float>();
      object.physics.restitution = reader.Scalar<float>();
      object.physics.linear_damping = reader.Scalar<float>();
      object.physics.angular_damping = reader.Scalar<float>();
      object.physics.gravity_scale = reader.Scalar<float>();
      const std::uint32_t enable_sleep =
          reader.Scalar<std::uint32_t>();
      const std::uint32_t initially_awake =
          reader.Scalar<std::uint32_t>();
      if (enable_sleep > 1 || initially_awake > 1) {
        throw std::runtime_error(
            "SKATE map object physics boolean is invalid");
      }
      object.physics.enable_sleep = enable_sleep != 0;
      object.physics.initially_awake = initially_awake != 0;
    }

    const std::uint64_t index_end =
        static_cast<std::uint64_t>(object.source_first_index) +
        object.source_index_count;
    const std::uint64_t collision_end =
        static_cast<std::uint64_t>(
            object.source_first_collision_triangle) +
        object.source_collision_triangle_count;
    if (object.id == 0 || object.name.empty() ||
        !Finite(object.origin) ||
        object.source_index_count == 0 ||
        object.source_index_count % 3u != 0 ||
        index_end > map.render_mesh.indices.size() ||
        collision_end > map.collision_triangles.size() ||
        !ids.insert(object.id).second ||
        !names.insert(object.name).second) {
      throw std::runtime_error("SKATE map object record is invalid");
    }

    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    object.render_mesh.indices.reserve(object.source_index_count);
    for (std::uint64_t source_offset = object.source_first_index;
         source_offset < index_end; ++source_offset) {
      if (claimed_indices[source_offset]) {
        throw std::runtime_error(
            "SKATE map object render ranges overlap");
      }
      claimed_indices[source_offset] = true;
      const std::uint32_t source_index =
          map.render_mesh.indices[source_offset];
      if (source_index >= map.render_mesh.vertices.size()) {
        throw std::runtime_error(
            "SKATE map object references an invalid render vertex");
      }
      const auto [found, inserted] = remap.emplace(
          source_index,
          static_cast<std::uint32_t>(
              object.render_mesh.vertices.size()));
      if (inserted) {
        RenderVertex vertex = map.render_mesh.vertices[source_index];
        vertex.position = vertex.position - object.origin;
        object.render_mesh.vertices.push_back(vertex);
      }
      object.render_mesh.indices.push_back(found->second);
    }

    object.collision_triangles.reserve(
        object.source_collision_triangle_count);
    for (std::uint64_t source_offset =
             object.source_first_collision_triangle;
         source_offset < collision_end; ++source_offset) {
      if (claimed_collision[source_offset]) {
        throw std::runtime_error(
            "SKATE map object collision ranges overlap");
      }
      claimed_collision[source_offset] = true;
      CollisionTriangle triangle =
          map.collision_triangles[source_offset];
      triangle.a = triangle.a - object.origin;
      triangle.b = triangle.b - object.origin;
      triangle.c = triangle.c - object.origin;
      object.collision_triangles.push_back(triangle);
    }

    object.local_bounds_min =
        object.render_mesh.vertices.front().position;
    object.local_bounds_max = object.local_bounds_min;
    const auto include = [&](Vec3 point) {
      object.local_bounds_min.x =
          std::min(object.local_bounds_min.x, point.x);
      object.local_bounds_min.y =
          std::min(object.local_bounds_min.y, point.y);
      object.local_bounds_min.z =
          std::min(object.local_bounds_min.z, point.z);
      object.local_bounds_max.x =
          std::max(object.local_bounds_max.x, point.x);
      object.local_bounds_max.y =
          std::max(object.local_bounds_max.y, point.y);
      object.local_bounds_max.z =
          std::max(object.local_bounds_max.z, point.z);
    };
    for (const RenderVertex& vertex : object.render_mesh.vertices) {
      include(vertex.position);
    }
    for (const CollisionTriangle& triangle :
         object.collision_triangles) {
      include(triangle.a);
      include(triangle.b);
      include(triangle.c);
    }
    map.editable_objects.push_back(std::move(object));
  }
  reader.RequireEnd();
}

void ReadBreakGroups(std::vector<std::uint8_t> payload,
                     std::uint32_t schema, MapDefinition& map) {
  if (schema != 1) {
    throw std::runtime_error(
        "SKATE BGRP extension uses an unsupported schema");
  }
  if (map.editable_objects.empty()) {
    throw std::runtime_error(
        "SKATE BGRP extension requires map objects");
  }
  Reader reader(std::move(payload));
  const std::uint32_t count = reader.Scalar<std::uint32_t>();
  RequireCount(count, "break group object");
  std::unordered_set<MapObjectId> claimed;
  for (std::uint32_t index = 0; index < count; ++index) {
    const MapObjectId object_id = reader.Scalar<MapObjectId>();
    const auto found = std::find_if(
        map.editable_objects.begin(), map.editable_objects.end(),
        [object_id](const MapObject& object) {
          return object.id == object_id;
        });
    if (found == map.editable_objects.end() ||
        !claimed.insert(object_id).second) {
      throw std::runtime_error(
          "SKATE BGRP object reference is invalid");
    }
    found->physics.break_group = reader.Scalar<std::uint32_t>();
    found->physics.break_speed_threshold = reader.Scalar<float>();
    found->physics.break_impulse_scale = reader.Scalar<float>();
    found->physics.break_angular_impulse = reader.Scalar<float>();
    found->physics.break_gravity_scale = reader.Scalar<float>();
    if (found->physics.break_group == 0) {
      throw std::runtime_error(
          "SKATE BGRP group identifier must be nonzero");
    }
  }
  reader.RequireEnd();
}

void ReadRetailCollisionIdentity(std::vector<std::uint8_t> payload,
                                 std::uint32_t schema, MapDefinition &map) {
  if (schema != 1) {
    return;
  }
  Reader reader(std::move(payload));
  const std::uint32_t resource_count = reader.Scalar<std::uint32_t>();
  RequireCount(resource_count, "retail collision resource");
  if (resource_count == 0 ||
      resource_count > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error(
        "SKATE retail collision resource count is invalid");
  }
  map.retail_collision_resource_names.reserve(resource_count);
  for (std::uint32_t index = 0; index < resource_count; ++index) {
    std::string name = reader.String();
    if (name.empty()) {
      throw std::runtime_error(
          "SKATE retail collision resource name is invalid");
    }
    map.retail_collision_resource_names.push_back(std::move(name));
  }

  const std::uint32_t triangle_count = reader.Scalar<std::uint32_t>();
  if (triangle_count != map.collision_triangles.size()) {
    throw std::runtime_error(
        "SKATE retail collision identity triangle count is invalid");
  }
  const std::uint32_t association_count = reader.Scalar<std::uint32_t>();
  RequireCount(association_count, "retail collision association");
  if (association_count < triangle_count) {
    throw std::runtime_error("SKATE retail collision identity omits triangles");
  }
  map.retail_collision_associations.reserve(association_count);
  std::uint32_t previous_triangle = 0;
  std::uint16_t previous_resource = 0;
  bool first = true;
  for (std::uint32_t index = 0; index < association_count; ++index) {
    RetailCollisionAssociation association;
    association.triangle_index = reader.Scalar<std::uint32_t>();
    association.resource_index = reader.Scalar<std::uint16_t>();
    association.cluster_index = reader.Scalar<std::uint16_t>();
    association.group_id = reader.Scalar<std::uint32_t>();
    association.unit_flags = reader.Scalar<std::uint8_t>();
    const std::array<std::uint8_t, 3> reserved{reader.Scalar<std::uint8_t>(),
                                               reader.Scalar<std::uint8_t>(),
                                               reader.Scalar<std::uint8_t>()};
    if (association.triangle_index >= triangle_count ||
        association.resource_index >= resource_count ||
        reserved != std::array<std::uint8_t, 3>{} ||
        (!first && (association.triangle_index < previous_triangle ||
                    (association.triangle_index == previous_triangle &&
                     association.resource_index <= previous_resource)))) {
      throw std::runtime_error("SKATE retail collision association is invalid");
    }
    first = false;
    previous_triangle = association.triangle_index;
    previous_resource = association.resource_index;
    map.retail_collision_associations.push_back(association);
  }
  reader.RequireEnd();
}

void Validate(MapDefinition& map) {
  if (map.name.empty() || !Finite(map.spawn.position) ||
      !std::isfinite(map.spawn.heading_radians)) {
    throw std::runtime_error("SKATE map metadata is invalid");
  }
  const DayNightCycleDefinition& cycle = map.day_night_cycle;
  if (!cycle.enabled || !std::isfinite(cycle.duration_seconds) ||
      cycle.duration_seconds < 0.0f ||
      !std::isfinite(cycle.start_time_hours) ||
      cycle.start_time_hours < 0.0f ||
      cycle.start_time_hours >= 24.0f ||
      !std::isfinite(cycle.end_time_hours) ||
      cycle.end_time_hours < 0.0f ||
      cycle.end_time_hours >= 24.0f ||
      !std::isfinite(cycle.orbit_azimuth_radians) ||
      !Finite(cycle.day_zenith) || !Finite(cycle.day_horizon) ||
      !Finite(cycle.day_nadir) || !Finite(cycle.twilight_zenith) ||
      !Finite(cycle.twilight_horizon) || !Finite(cycle.twilight_nadir) ||
      !Finite(cycle.night_zenith) || !Finite(cycle.night_horizon) ||
      !Finite(cycle.night_nadir) || !Finite(cycle.sun_color) ||
      !Finite(cycle.moon_color) ||
      !std::isfinite(cycle.sun_intensity) ||
      cycle.sun_intensity < 0.0f ||
      !std::isfinite(cycle.moon_intensity) ||
      cycle.moon_intensity < 0.0f ||
      !std::isfinite(cycle.day_ambient) ||
      cycle.day_ambient < 0.0f || cycle.day_ambient > 1.0f ||
      !std::isfinite(cycle.night_ambient) ||
      cycle.night_ambient < 0.0f || cycle.night_ambient > 1.0f ||
      !Finite(cycle.sky_tint) ||
      cycle.sky_tint.x < 0.0f || cycle.sky_tint.x > 4.0f ||
      cycle.sky_tint.y < 0.0f || cycle.sky_tint.y > 4.0f ||
      cycle.sky_tint.z < 0.0f || cycle.sky_tint.z > 4.0f) {
    throw std::runtime_error("SKATE day/night metadata is invalid");
  }
  if (map.materials.empty() || map.render_mesh.vertices.empty() ||
      map.render_mesh.indices.empty() ||
      map.collision_triangles.empty()) {
    throw std::runtime_error(
        "SKATE requires materials, render geometry, and collision");
  }

  std::unordered_set<TextureId> texture_ids;
  for (const ImageTexture& texture : map.textures) {
    const std::uint64_t expected =
        std::uint64_t(texture.width) * texture.height * 4u;
    const bool empty_placeholder =
        texture.width == 0 && texture.height == 0 &&
        texture.rgba8.empty() && texture.stored_rgba8.empty();
    const bool decoded =
        texture.rgba8.size() == expected;
    const bool package_backed =
        texture.rgba8.empty() &&
        ((texture.stored_rgba8_method == kStorageRaw &&
          texture.stored_rgba8.size() == expected) ||
         (texture.stored_rgba8_method == kStorageDeflate &&
          (expected == 0 || !texture.stored_rgba8.empty())));
    if (texture.id == 0 || texture.name.empty() ||
        !texture_ids.insert(texture.id).second ||
        (!empty_placeholder &&
         (texture.width == 0 || texture.height == 0 ||
          texture.width > kMaximumTextureDimension ||
          texture.height > kMaximumTextureDimension)) ||
        (!empty_placeholder && !decoded && !package_backed)) {
      throw std::runtime_error("SKATE embedded texture is invalid");
    }
  }

  std::unordered_set<MaterialId> material_ids;
  for (const SurfaceMaterial& material : map.materials) {
    if (material.id == 0 || material.name.empty() ||
        !material_ids.insert(material.id).second ||
        !Finite(material.display_color) ||
        material.friction < 0.0f ||
        material.restitution < 0.0f ||
        material.roughness < 0.0f || material.roughness > 1.0f ||
        material.baked_indirect_strength < 0.0f ||
        material.alpha_cutoff < 0.0f || material.alpha_cutoff > 1.0f ||
        static_cast<std::uint32_t>(material.alpha_mode) >
            static_cast<std::uint32_t>(
                SurfaceMaterial::AlphaMode::Blend) ||
        material.skate_audio_surface > 93 ||
        material.skate_physics_surface > 13 ||
        material.skate_surface_pattern > 15 ||
        material.presentation_depth_layer > 3 ||
        (material.albedo_texture != 0 &&
         !texture_ids.contains(material.albedo_texture)) ||
        (material.indirect_lightmap != 0 &&
         !texture_ids.contains(material.indirect_lightmap)) ||
        (material.normal_texture != 0 &&
         !texture_ids.contains(material.normal_texture)) ||
        (material.orm_texture != 0 &&
         !texture_ids.contains(material.orm_texture)) ||
        (material.emissive_texture != 0 &&
         !texture_ids.contains(material.emissive_texture))) {
      throw std::runtime_error("SKATE material table is invalid");
    }
    if (material.retail.enabled) {
      const std::uint32_t family =
          static_cast<std::uint32_t>(
              material.retail.shader_family);
      const std::uint32_t flags =
          static_cast<std::uint32_t>(
              material.retail.render_flags);
      constexpr std::uint32_t kKnownRetailFlags =
          (1u << 7u) - 1u;
      if (material.retail.shader_name.empty() ||
          family > static_cast<std::uint32_t>(
                       RetailShaderFamily::Sky) ||
          (flags & ~kKnownRetailFlags) != 0) {
        throw std::runtime_error(
            "SKATE retail material metadata is invalid");
      }
      for (const RetailTextureBinding& binding :
           material.retail.texture_bindings) {
        if (binding.semantic.empty() || binding.texture == 0 ||
            !texture_ids.contains(binding.texture) ||
            binding.uv_set > 2 || binding.address_u > 2 ||
            binding.address_v > 2) {
          throw std::runtime_error(
              "SKATE retail texture binding is invalid");
        }
      }
      for (const RetailMaterialParameter& parameter :
           material.retail.parameters) {
        if (parameter.name.empty() || parameter.values.empty()) {
          throw std::runtime_error(
              "SKATE retail material parameter is invalid");
        }
      }
    }
  }

  if (map.render_mesh.indices.size() % 3 != 0) {
    throw std::runtime_error(
        "SKATE render indices do not form complete triangles");
  }
  std::atomic<bool> invalid_render_vertex{false};
  ParallelRanges(
      map.render_mesh.vertices.size(),
      [&](std::size_t first, std::size_t last) {
        for (std::size_t index = first;
             index < last &&
             !invalid_render_vertex.load(std::memory_order_relaxed);
             ++index) {
          const RenderVertex& vertex =
              map.render_mesh.vertices[index];
          if (!Finite(vertex.position) || !Finite(vertex.normal) ||
              !Finite(vertex.uv) || !Finite(vertex.lightmap_uv) ||
              !Finite(vertex.decal_uv) ||
              !Finite(vertex.tangent_binormal) ||
              !std::isfinite(vertex.tangent_handedness) ||
              !material_ids.contains(vertex.material)) {
            invalid_render_vertex.store(
                true, std::memory_order_relaxed);
          }
        }
      });
  if (invalid_render_vertex.load(std::memory_order_relaxed)) {
    throw std::runtime_error("SKATE render vertex is invalid");
  }

  std::atomic<bool> invalid_render_index{false};
  ParallelRanges(
      map.render_mesh.indices.size(),
      [&](std::size_t first, std::size_t last) {
        for (std::size_t position = first;
             position < last &&
             !invalid_render_index.load(std::memory_order_relaxed);
             ++position) {
          if (map.render_mesh.indices[position] >=
              map.render_mesh.vertices.size()) {
            invalid_render_index.store(
                true, std::memory_order_relaxed);
          }
        }
      });
  if (invalid_render_index.load(std::memory_order_relaxed)) {
    throw std::runtime_error("SKATE render index is out of range");
  }

  std::atomic<bool> invalid_collision{false};
  ParallelRanges(
      map.collision_triangles.size(),
      [&](std::size_t first, std::size_t last) {
        for (std::size_t index = first;
             index < last &&
             !invalid_collision.load(std::memory_order_relaxed);
             ++index) {
          CollisionTriangle& triangle =
              map.collision_triangles[index];
          const Vec3 cross = Cross(
              triangle.b - triangle.a, triangle.c - triangle.a);
          if (!Finite(triangle.a) || !Finite(triangle.b) ||
              !Finite(triangle.c) ||
              LengthSquared(cross) <= kAreaEpsilon ||
              triangle.surface == 0 ||
              !material_ids.contains(triangle.material)) {
            invalid_collision.store(
                true, std::memory_order_relaxed);
            continue;
          }
          triangle.normal = Normalize(cross);
        }
      });
  if (invalid_collision.load(std::memory_order_relaxed)) {
    throw std::runtime_error("SKATE collision triangle is invalid");
  }
  std::unordered_set<MapObjectId> object_ids;
  std::unordered_set<std::string> object_names;
  for (MapObject& object : map.editable_objects) {
    const std::uint32_t physics_type =
        static_cast<std::uint32_t>(object.physics.type);
    const std::uint32_t collision_shape =
        static_cast<std::uint32_t>(object.physics.shape);
    const bool valid_physics =
        physics_type <=
            static_cast<std::uint32_t>(ObjectPhysicsType::Dynamic) &&
        collision_shape <=
            static_cast<std::uint32_t>(
                ObjectCollisionShape::ConvexHull) &&
        std::isfinite(object.physics.density) &&
        object.physics.density > 0.0f &&
        object.physics.density <= 100000.0f &&
        std::isfinite(object.physics.friction) &&
        object.physics.friction >= 0.0f &&
        object.physics.friction <= 2.0f &&
        std::isfinite(object.physics.restitution) &&
        object.physics.restitution >= 0.0f &&
        object.physics.restitution <= 1.0f &&
        std::isfinite(object.physics.linear_damping) &&
        object.physics.linear_damping >= 0.0f &&
        object.physics.linear_damping <= 100.0f &&
        std::isfinite(object.physics.angular_damping) &&
        object.physics.angular_damping >= 0.0f &&
        object.physics.angular_damping <= 100.0f &&
        std::isfinite(object.physics.gravity_scale) &&
        object.physics.gravity_scale >= -10.0f &&
        object.physics.gravity_scale <= 10.0f &&
        std::isfinite(object.physics.break_speed_threshold) &&
        object.physics.break_speed_threshold >= 0.1f &&
        object.physics.break_speed_threshold <= 30.0f &&
        std::isfinite(object.physics.break_impulse_scale) &&
        object.physics.break_impulse_scale >= 0.0f &&
        object.physics.break_impulse_scale <= 10.0f &&
        std::isfinite(object.physics.break_angular_impulse) &&
        object.physics.break_angular_impulse >= 0.0f &&
        object.physics.break_angular_impulse <= 10.0f &&
        std::isfinite(object.physics.break_gravity_scale) &&
        object.physics.break_gravity_scale >= 0.0f &&
        object.physics.break_gravity_scale <= 4.0f &&
        (object.physics.break_group == 0 ||
         object.physics.type == ObjectPhysicsType::Dynamic);
    if (object.id == 0 || object.name.empty() ||
        !object_ids.insert(object.id).second ||
        !object_names.insert(object.name).second ||
        !Finite(object.origin) ||
        !Finite(object.local_bounds_min) ||
        !Finite(object.local_bounds_max) ||
        object.local_bounds_min.x > object.local_bounds_max.x ||
        object.local_bounds_min.y > object.local_bounds_max.y ||
        object.local_bounds_min.z > object.local_bounds_max.z ||
        object.render_mesh.vertices.empty() ||
        object.render_mesh.indices.empty() ||
        object.render_mesh.indices.size() % 3u != 0 ||
        !valid_physics) {
      throw std::runtime_error("SKATE editable map object is invalid");
    }
    for (const RenderVertex& vertex : object.render_mesh.vertices) {
      if (!Finite(vertex.position) || !Finite(vertex.normal) ||
          !Finite(vertex.uv) || !Finite(vertex.lightmap_uv) ||
          !Finite(vertex.decal_uv) ||
          !Finite(vertex.tangent_binormal) ||
          !std::isfinite(vertex.tangent_handedness) ||
          FindMaterial(map, vertex.material) == nullptr) {
        throw std::runtime_error(
            "SKATE editable map object vertex is invalid");
      }
    }
    for (std::uint32_t index : object.render_mesh.indices) {
      if (index >= object.render_mesh.vertices.size()) {
        throw std::runtime_error(
            "SKATE editable map object index is out of range");
      }
    }
    for (CollisionTriangle& triangle :
         object.collision_triangles) {
      const Vec3 cross =
          Cross(triangle.b - triangle.a, triangle.c - triangle.a);
      if (!Finite(triangle.a) || !Finite(triangle.b) ||
          !Finite(triangle.c) ||
          LengthSquared(cross) <= kAreaEpsilon ||
          triangle.surface == 0 ||
          FindMaterial(map, triangle.material) == nullptr) {
        throw std::runtime_error(
            "SKATE editable map object collision is invalid");
      }
      triangle.normal = Normalize(cross);
    }
  }
  for (const GrindRail& rail : map.grind_rails) {
    const bool native = !rail.native_segments.empty();
    const bool points_valid =
        rail.points.size() >= 2 &&
        std::all_of(
            rail.points.begin(), rail.points.end(),
            [](Vec3 point) { return Finite(point); });
    const bool native_valid =
        native && rail.points.empty() &&
        rail.retail_spline_id != 0 &&
        rail.retail_type_signature != 0 &&
        std::all_of(
            rail.native_segments.begin(), rail.native_segments.end(),
            [](const NativeGrindSegment& segment) {
              return std::all_of(
                  segment.words.begin(), segment.words.end(),
                  [](std::uint32_t word) {
                    return std::isfinite(std::bit_cast<float>(word));
                  });
            });
    if (rail.id == 0 || rail.name.empty() ||
        (!native && !points_valid) || (native && !native_valid)) {
      throw std::runtime_error("SKATE grind path is invalid");
    }
  }
  std::unordered_set<NpcRouteId> npc_route_ids;
  for (const NpcRoute& route : map.npc_routes) {
    if (route.id == 0 || route.name.empty() ||
        !npc_route_ids.insert(route.id).second ||
        route.points.size() < 2 || route.skater_count == 0 ||
        route.skater_count > 32 || !std::isfinite(route.speed) ||
        route.speed <= 0.0f || route.speed > 30.0f ||
        !std::isfinite(route.spawn_spacing) ||
        route.spawn_spacing < 0.0f || route.spawn_spacing > 100.0f ||
        !std::all_of(
            route.points.begin(), route.points.end(),
            [](Vec3 point) { return Finite(point); })) {
      throw std::runtime_error("SKATE NPC route is invalid");
    }
  }
  std::unordered_set<HingedDoorId> door_ids;
  for (HingedDoor& door : map.hinged_doors) {
    const float axis_dot = Dot(door.hinge_axis, door.closed_width_axis);
    const float depth_dot = Dot(door.hinge_axis, door.closed_depth_axis);
    const float width_depth_dot =
        Dot(door.closed_width_axis, door.closed_depth_axis);
    if (door.id == 0 || door.name.empty() ||
        !door_ids.insert(door.id).second ||
        !Finite(door.hinge_position) || !Finite(door.hinge_axis) ||
        !Finite(door.closed_width_axis) ||
        !Finite(door.closed_depth_axis) ||
        !Finite(door.local_min) || !Finite(door.local_max) ||
        std::abs(LengthSquared(door.hinge_axis) - 1.0f) > 1.0e-3f ||
        std::abs(LengthSquared(door.closed_width_axis) - 1.0f) > 1.0e-3f ||
        std::abs(LengthSquared(door.closed_depth_axis) - 1.0f) > 1.0e-3f ||
        std::abs(axis_dot) > 1.0e-3f ||
        std::abs(depth_dot) > 1.0e-3f ||
        std::abs(width_depth_dot) > 1.0e-3f ||
        door.local_min.x >= door.local_max.x ||
        door.local_min.y >= door.local_max.y ||
        door.local_min.z >= door.local_max.z ||
        !std::isfinite(door.minimum_angle_radians) ||
        !std::isfinite(door.maximum_angle_radians) ||
        !std::isfinite(door.initial_angle_radians) ||
        door.minimum_angle_radians >= door.maximum_angle_radians ||
        door.initial_angle_radians < door.minimum_angle_radians ||
        door.initial_angle_radians > door.maximum_angle_radians ||
        !std::isfinite(door.mass) || door.mass <= 0.0f ||
        !std::isfinite(door.angular_damping) ||
        door.angular_damping < 0.0f ||
        !std::isfinite(door.return_spring_strength) ||
        door.return_spring_strength < 0.0f ||
        !std::isfinite(door.maximum_angular_speed) ||
        door.maximum_angular_speed <= 0.0f ||
        !std::isfinite(door.contact_impulse_scale) ||
        door.contact_impulse_scale < 0.0f ||
        !std::isfinite(door.static_friction) ||
        door.static_friction < 0.0f ||
        !std::isfinite(door.restitution) ||
        door.restitution < 0.0f ||
        door.surface == 0 || door.render_mesh.vertices.empty() ||
        door.render_mesh.indices.empty() ||
        door.render_mesh.indices.size() % 3 != 0 ||
        door.collision_triangles.empty()) {
      throw std::runtime_error("SKATE hinged door is invalid");
    }
    for (const RenderVertex& vertex : door.render_mesh.vertices) {
      if (!Finite(vertex.position) || !Finite(vertex.normal) ||
          !Finite(vertex.uv) || !Finite(vertex.lightmap_uv) ||
          !Finite(vertex.decal_uv) ||
          !Finite(vertex.tangent_binormal) ||
          !std::isfinite(vertex.tangent_handedness) ||
          !material_ids.contains(vertex.material)) {
        throw std::runtime_error("SKATE hinged-door render vertex is invalid");
      }
    }
    for (std::uint32_t index : door.render_mesh.indices) {
      if (index >= door.render_mesh.vertices.size()) {
        throw std::runtime_error("SKATE hinged-door index is out of range");
      }
    }
    for (CollisionTriangle& triangle : door.collision_triangles) {
      const Vec3 cross = Cross(triangle.b - triangle.a,
                               triangle.c - triangle.a);
      if (!Finite(triangle.a) || !Finite(triangle.b) ||
          !Finite(triangle.c) ||
          LengthSquared(cross) <= kAreaEpsilon ||
          triangle.surface == 0 ||
          !material_ids.contains(triangle.material)) {
        throw std::runtime_error(
            "SKATE hinged-door collision triangle is invalid");
      }
      triangle.normal = Normalize(cross);
    }
  }
  std::unordered_set<MovingLightOrbId> light_ids;
  for (MovingLightOrb& light : map.moving_light_orbs) {
    if (light.id == 0 || light.name.empty() ||
        !light_ids.insert(light.id).second ||
        !Finite(light.orbit_center) || !Finite(light.direction) ||
        !Finite(light.color) ||
        static_cast<std::uint32_t>(light.type) >
            static_cast<std::uint32_t>(LocalLightType::Area) ||
        LengthSquared(light.direction) <= kAreaEpsilon ||
        !std::isfinite(light.source_radius) ||
        !std::isfinite(light.influence_radius) ||
        !std::isfinite(light.intensity) ||
        light.source_radius <= 0.0f ||
        light.influence_radius <= light.source_radius ||
        light.intensity <= 0.0f ||
        !std::isfinite(light.spot_inner_cosine) ||
        !std::isfinite(light.spot_outer_cosine) ||
        light.spot_inner_cosine < -1.0f ||
        light.spot_inner_cosine > 1.0f ||
        light.spot_outer_cosine < -1.0f ||
        light.spot_outer_cosine > light.spot_inner_cosine) {
      throw std::runtime_error("SKATE local light is invalid");
    }
    light.direction = Normalize(light.direction);
  }
}

}  // namespace

MapDefinition LoadOwnedMapPackage(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error(
        "could not open SKATE package: " + path.string());
  }
  const std::streamoff end = stream.tellg();
  if (end <= 0 || static_cast<std::uint64_t>(end) > kMaximumPackageBytes) {
    throw std::runtime_error("SKATE package size is invalid");
  }
  stream.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  stream.read(reinterpret_cast<char*>(bytes.data()), end);
  if (!stream) {
    throw std::runtime_error("could not read the complete SKATE package");
  }

  Reader reader(std::move(bytes));
  std::array<char, 8> magic{};
  reader.Bytes(magic.data(), magic.size());
  const bool version_1 = magic == kMagicV1;
  const bool version_2 = magic == kMagicV2;
  const bool version_3 = magic == kMagicV3;
  const bool version_4 = magic == kMagicV4;
  const bool version_5 = magic == kMagicV5;
  const bool version_6 = magic == kMagicV6;
  const bool version_7 = magic == kMagicV7;
  const bool version_8 = magic == kMagicV8;
  const bool version_9 = magic == kMagicV9;
  const bool version_10 = magic == kMagicV10;
  const bool version_11 = magic == kMagicV11;
  const bool version_12 = magic == kMagicV12;
  const bool version_13 = magic == kMagicV13;
  const bool version_14 = magic == kMagicV14;
  const bool version_10_or_newer =
      version_10 || version_11 || version_12 || version_13 ||
      version_14;
  const bool skate_magic =
      std::memcmp(magic.data(), "SKATE", 5) == 0 &&
      std::isdigit(static_cast<unsigned char>(magic[5])) &&
      std::isdigit(static_cast<unsigned char>(magic[6])) &&
      magic[7] == '\0';
  const int package_version =
      skate_magic ? (magic[5] - '0') * 10 + (magic[6] - '0') : 0;
  if (skate_magic && package_version > 14) {
    throw std::runtime_error(
        "SKATE v" + std::to_string(package_version) +
        " requires a newer Custom Engine Layer release");
  }
  if ((!version_1 && !version_2 && !version_3 && !version_4 && !version_5 &&
       !version_6 && !version_7 && !version_8 && !version_9 &&
       !version_10 && !version_11 && !version_12 && !version_13 &&
       !version_14) ||
      reader.Scalar<std::uint32_t>() != kEndianMarker) {
    throw std::runtime_error(
        "file is not a supported little-endian SKATE v1-v14 package");
  }

  MapDefinition map;
  map.package_version = static_cast<std::uint32_t>(package_version);
  map.name = reader.String();
  map.spawn.position = reader.Vector3();
  map.spawn.heading_radians = reader.Scalar<float>();

  map.sky.enabled = true;
  map.sky.zenith_color = reader.Vector3();
  map.sky.horizon_color = reader.Vector3();
  map.sky.nadir_color = reader.Vector3();
  map.day_night_cycle.day_zenith = map.sky.zenith_color;
  map.day_night_cycle.day_horizon = map.sky.horizon_color;
  map.day_night_cycle.day_nadir = map.sky.nadir_color;
  map.day_night_cycle.enabled = true;
  map.day_night_cycle.duration_seconds = reader.Scalar<float>();
  map.day_night_cycle.start_time_hours = reader.Scalar<float>();
  map.day_night_cycle.orbit_azimuth_radians = reader.Scalar<float>();
  if (package_version >= 3) {
    map.day_night_cycle.end_time_hours = reader.Scalar<float>();
    map.day_night_cycle.ping_pong = reader.Scalar<float>() > 0.5f;
  }
  if (package_version >= 6) {
    map.day_night_cycle.twilight_zenith = reader.Vector3();
    map.day_night_cycle.twilight_horizon = reader.Vector3();
    map.day_night_cycle.twilight_nadir = reader.Vector3();
    map.day_night_cycle.night_zenith = reader.Vector3();
    map.day_night_cycle.night_horizon = reader.Vector3();
    map.day_night_cycle.night_nadir = reader.Vector3();
    map.day_night_cycle.sun_color = reader.Vector3();
    map.day_night_cycle.moon_color = reader.Vector3();
    map.day_night_cycle.sun_intensity = reader.Scalar<float>();
    map.day_night_cycle.moon_intensity = reader.Scalar<float>();
    map.day_night_cycle.day_ambient = reader.Scalar<float>();
    map.day_night_cycle.night_ambient = reader.Scalar<float>();
    map.day_night_cycle.sky_tint = reader.Vector3();
  }

  const std::uint32_t material_count = reader.Scalar<std::uint32_t>();
  const std::uint32_t texture_count = reader.Scalar<std::uint32_t>();
  const std::uint32_t vertex_count = reader.Scalar<std::uint32_t>();
  const std::uint32_t index_count = reader.Scalar<std::uint32_t>();
  const std::uint32_t collision_count = reader.Scalar<std::uint32_t>();
  const std::uint32_t rail_count = reader.Scalar<std::uint32_t>();
  const std::uint32_t door_count =
      package_version >= 4 ? reader.Scalar<std::uint32_t>() : 0;
  const std::uint32_t local_light_count =
      package_version >= 7 ? reader.Scalar<std::uint32_t>() : 0;
  const std::uint32_t npc_route_count =
      package_version >= 8 ? reader.Scalar<std::uint32_t>() : 0;
  RequireCount(material_count, "material");
  RequireCount(texture_count, "texture");
  RequireCount(vertex_count, "vertex", kMaximumGeometryCount);
  RequireCount(index_count, "index", kMaximumIndexCount);
  RequireCount(collision_count, "collision", kMaximumGeometryCount);
  RequireCount(rail_count, "grind rail");
  RequireCount(door_count, "hinged door");
  RequireCount(local_light_count, "local light");
  RequireCount(npc_route_count, "NPC route");

  map.materials.reserve(material_count);
  for (std::uint32_t index = 0; index < material_count; ++index) {
    SurfaceMaterial material;
    material.id = index + 1;
    material.name = reader.String();
    material.flags =
        static_cast<SurfaceFlags>(reader.Scalar<std::uint32_t>());
    material.friction = reader.Scalar<float>();
    material.restitution = reader.Scalar<float>();
    material.display_color = reader.Vector3();
    material.roughness = reader.Scalar<float>();
    material.emissive_intensity = reader.Scalar<float>();
    material.albedo_texture = reader.Scalar<TextureId>();
    material.indirect_lightmap = reader.Scalar<TextureId>();
    material.baked_indirect_strength = reader.Scalar<float>();
    if (package_version >= 2) {
      material.normal_texture = reader.Scalar<TextureId>();
      material.orm_texture = reader.Scalar<TextureId>();
      material.emissive_texture = reader.Scalar<TextureId>();
      material.alpha_mode =
          static_cast<SurfaceMaterial::AlphaMode>(
              reader.Scalar<std::uint32_t>());
      material.alpha_cutoff = reader.Scalar<float>();
      material.skate_audio_surface =
          static_cast<std::uint8_t>(reader.Scalar<std::uint32_t>());
      material.skate_physics_surface =
          static_cast<std::uint8_t>(reader.Scalar<std::uint32_t>());
      material.skate_surface_pattern =
          static_cast<std::uint8_t>(reader.Scalar<std::uint32_t>());
    }
    if (package_version >= 13) {
      material.presentation_depth_layer =
          reader.Scalar<std::uint32_t>();
    } else {
      material.presentation_depth_layer =
          InferPresentationDepthLayer(material);
    }
    if (package_version >= 12) {
      material.retail.enabled =
          reader.Scalar<std::uint32_t>() != 0;
      if (material.retail.enabled) {
        material.retail.material_guid =
            reader.Scalar<std::uint64_t>();
        material.retail.material_handle =
            reader.Scalar<std::uint32_t>();
        material.retail.material_group_index =
            reader.Scalar<std::int32_t>();
        material.retail.shader_name = reader.String();
        material.retail.shader_family =
            static_cast<RetailShaderFamily>(
                reader.Scalar<std::uint32_t>());
        material.retail.render_flags =
            static_cast<RetailRenderFlags>(
                reader.Scalar<std::uint32_t>());
        const std::uint32_t binding_count =
            reader.Scalar<std::uint32_t>();
        RequireCount(binding_count, "retail texture binding");
        material.retail.texture_bindings.reserve(binding_count);
        for (std::uint32_t binding_index = 0;
             binding_index < binding_count; ++binding_index) {
          RetailTextureBinding binding;
          binding.semantic = reader.String();
          binding.texture = reader.Scalar<TextureId>();
          binding.uv_set = reader.Scalar<std::uint32_t>();
          binding.address_u = reader.Scalar<std::uint32_t>();
          binding.address_v = reader.Scalar<std::uint32_t>();
          material.retail.texture_bindings.push_back(
              std::move(binding));
        }
        const std::uint32_t parameter_count =
            reader.Scalar<std::uint32_t>();
        RequireCount(parameter_count, "retail material parameter");
        material.retail.parameters.reserve(parameter_count);
        for (std::uint32_t parameter_index = 0;
             parameter_index < parameter_count; ++parameter_index) {
          RetailMaterialParameter parameter;
          parameter.name = reader.String();
          const std::uint32_t value_count =
              reader.Scalar<std::uint32_t>();
          RequireCount(value_count, "retail material parameter value");
          parameter.values.reserve(value_count);
          for (std::uint32_t value_index = 0;
               value_index < value_count; ++value_index) {
            parameter.values.push_back(reader.String());
          }
          material.retail.parameters.push_back(
              std::move(parameter));
        }
        material.retail.source_metadata_json = reader.String();
      }
    }
    map.materials.push_back(std::move(material));
  }

  map.textures.reserve(texture_count);
  for (std::uint32_t index = 0; index < texture_count; ++index) {
    ImageTexture texture;
    texture.id = index + 1;
    texture.name = reader.String();
    texture.width = reader.Scalar<std::uint32_t>();
    texture.height = reader.Scalar<std::uint32_t>();
    texture.color_space =
        static_cast<TextureColorSpace>(reader.Scalar<std::uint32_t>());
    const std::uint64_t expected_byte_count =
        std::uint64_t(texture.width) * texture.height * 4u;
    const bool empty_placeholder =
        texture.width == 0 && texture.height == 0;
    if ((texture.width == 0) != (texture.height == 0) ||
        (!empty_placeholder &&
         (texture.width > kMaximumTextureDimension ||
          texture.height > kMaximumTextureDimension)) ||
        expected_byte_count > kMaximumTextureBytes) {
      throw std::runtime_error("SKATE embedded texture is invalid");
    }
    if (package_version >= 9) {
      StoredPayload payload = ReadStoredPayload(
          reader,
          static_cast<std::size_t>(expected_byte_count),
          "embedded texture");
      texture.stored_rgba8_method = payload.method;
      texture.stored_rgba8 = std::move(payload.bytes);
    } else {
      const std::uint32_t byte_count = reader.Scalar<std::uint32_t>();
      if (byte_count != expected_byte_count) {
        throw std::runtime_error("SKATE embedded texture is invalid");
      }
      texture.rgba8 = reader.ByteVector(byte_count);
    }
    map.textures.push_back(std::move(texture));
  }

  if (package_version >= 9) {
    Reader vertex_reader(ReadStoredBytes(
        reader,
        std::size_t(vertex_count) *
            (package_version >= 12
                 ? sizeof(float) * 12u + sizeof(std::uint32_t) * 2u
                 : sizeof(float) * 10u + sizeof(std::uint32_t)),
        "visual vertex block"));
    ReadRenderVertices(
        vertex_reader, vertex_count, map.render_mesh.vertices,
        package_version >= 12);
    vertex_reader.RequireEnd();

    Reader index_reader(ReadStoredBytes(
        reader,
        std::size_t(index_count) * sizeof(std::uint32_t),
        "visual index block"));
    ReadIndices(index_reader, index_count, map.render_mesh.indices);
    index_reader.RequireEnd();

    Reader collision_reader(ReadStoredBytes(
        reader,
        std::size_t(collision_count) *
            (sizeof(float) * 9u + sizeof(std::uint32_t) * 2u +
             (package_version >= 11 ? 4u : 0u)),
        "collision block"));
    ReadCollisionTriangles(
        collision_reader,
        collision_count,
        map.collision_triangles,
        package_version >= 11);
    collision_reader.RequireEnd();
  } else {
    ReadRenderVertices(
        reader, vertex_count, map.render_mesh.vertices,
        package_version >= 12);
    ReadIndices(reader, index_count, map.render_mesh.indices);
    ReadCollisionTriangles(
        reader,
        collision_count,
        map.collision_triangles,
        package_version >= 11);
  }

  map.grind_rails.reserve(rail_count);
  for (std::uint32_t index = 0; index < rail_count; ++index) {
    GrindRail rail;
    rail.id = index + 1;
    rail.name = reader.String();
    rail.closed = reader.Scalar<std::uint32_t>() != 0;
    const std::uint32_t representation =
        version_10_or_newer ? reader.Scalar<std::uint32_t>() : 0;
    if (representation == 0) {
      const std::uint32_t point_count = reader.Scalar<std::uint32_t>();
      RequireCount(point_count, "grind point");
      rail.points.reserve(point_count);
      for (std::uint32_t point = 0; point < point_count; ++point) {
        rail.points.push_back(reader.Vector3());
      }
    } else if (representation == 1 && version_10_or_newer) {
      rail.retail_spline_id = reader.Scalar<std::uint64_t>();
      rail.retail_type_signature = reader.Scalar<std::uint64_t>();
      rail.retail_flags = reader.Scalar<std::uint32_t>();
      rail.retail_trailing_word = reader.Scalar<std::uint32_t>();
      const std::uint32_t segment_count =
          reader.Scalar<std::uint32_t>();
      RequireCount(segment_count, "native grind segment");
      rail.native_segments.resize(segment_count);
      for (NativeGrindSegment& segment : rail.native_segments) {
        for (std::uint32_t& word : segment.words) {
          word = reader.Scalar<std::uint32_t>();
        }
      }
    } else {
      throw std::runtime_error(
          "SKATE grind representation is unsupported");
    }
    map.grind_rails.push_back(std::move(rail));
  }

  map.hinged_doors.reserve(door_count);
  for (std::uint32_t index = 0; index < door_count; ++index) {
    HingedDoor door;
    door.id = index + 1;
    door.name = reader.String();
    door.hinge_position = reader.Vector3();
    door.hinge_axis = reader.Vector3();
    door.closed_width_axis = reader.Vector3();
    door.closed_depth_axis = reader.Vector3();
    door.local_min = reader.Vector3();
    door.local_max = reader.Vector3();
    door.minimum_angle_radians = reader.Scalar<float>();
    door.maximum_angle_radians = reader.Scalar<float>();
    door.initial_angle_radians = reader.Scalar<float>();
    door.mass = reader.Scalar<float>();
    door.angular_damping = reader.Scalar<float>();
    if (package_version >= 5) {
      door.return_spring_strength = reader.Scalar<float>();
      door.maximum_angular_speed = reader.Scalar<float>();
      door.contact_impulse_scale = reader.Scalar<float>();
    }
    door.static_friction = reader.Scalar<float>();
    door.restitution = reader.Scalar<float>();
    door.surface = reader.Scalar<SurfaceId>();
    const std::uint32_t door_vertex_count =
        reader.Scalar<std::uint32_t>();
    const std::uint32_t door_index_count =
        reader.Scalar<std::uint32_t>();
    const std::uint32_t door_collision_count =
        reader.Scalar<std::uint32_t>();
    RequireCount(door_vertex_count, "hinged-door vertex");
    RequireCount(door_index_count, "hinged-door index");
    RequireCount(door_collision_count, "hinged-door collision");
    door.render_mesh.vertices.reserve(door_vertex_count);
    for (std::uint32_t vertex_index = 0;
         vertex_index < door_vertex_count; ++vertex_index) {
      RenderVertex vertex;
      vertex.position = reader.Vector3();
      vertex.normal = reader.Vector3();
      vertex.uv = reader.Vector2();
      vertex.lightmap_uv = reader.Vector2();
      vertex.material = reader.Scalar<MaterialId>();
      if (package_version >= 12) {
        vertex.decal_uv = reader.Vector2();
        ReadPackedTangentFrame(reader, vertex);
      } else {
        vertex.decal_uv = vertex.uv;
      }
      door.render_mesh.vertices.push_back(vertex);
    }
    door.render_mesh.indices.reserve(door_index_count);
    for (std::uint32_t door_index = 0;
         door_index < door_index_count; ++door_index) {
      door.render_mesh.indices.push_back(
          reader.Scalar<std::uint32_t>());
    }
    ReadCollisionTriangles(reader, door_collision_count,
                           door.collision_triangles, package_version >= 11);
    map.hinged_doors.push_back(std::move(door));
  }

  map.moving_light_orbs.reserve(local_light_count);
  for (std::uint32_t index = 0; index < local_light_count; ++index) {
    MovingLightOrb light;
    light.id = index + 1;
    light.name = reader.String();
    light.type =
        static_cast<LocalLightType>(reader.Scalar<std::uint32_t>());
    light.orbit_center = reader.Vector3();
    light.direction = reader.Vector3();
    light.color = reader.Vector3();
    light.intensity = reader.Scalar<float>();
    light.influence_radius = reader.Scalar<float>();
    light.source_radius = reader.Scalar<float>();
    light.spot_inner_cosine = reader.Scalar<float>();
    light.spot_outer_cosine = reader.Scalar<float>();
    light.orbit_axis_u = {};
    light.orbit_axis_v = {};
    light.period_seconds = 0.0f;
    light.phase_radians = 0.0f;
    light.visible_source = false;
    map.moving_light_orbs.push_back(std::move(light));
  }

  map.npc_routes.reserve(npc_route_count);
  for (std::uint32_t index = 0; index < npc_route_count; ++index) {
    NpcRoute route;
    route.id = index + 1;
    route.name = reader.String();
    route.closed = reader.Scalar<std::uint32_t>() != 0;
    route.skater_count = reader.Scalar<std::uint32_t>();
    route.speed = reader.Scalar<float>();
    route.spawn_spacing = reader.Scalar<float>();
    const std::uint32_t point_count = reader.Scalar<std::uint32_t>();
    RequireCount(point_count, "NPC route point");
    route.points.reserve(point_count);
    for (std::uint32_t point = 0; point < point_count; ++point) {
      route.points.push_back(reader.Vector3());
    }
    map.npc_routes.push_back(std::move(route));
  }

  if (package_version >= 12) {
    std::vector<std::uint8_t> break_group_payload;
    std::uint32_t break_group_schema = 0;
    bool break_group_seen = false;
    const std::uint32_t extension_count =
        reader.Scalar<std::uint32_t>();
    RequireCount(extension_count, "extension");
    for (std::uint32_t extension_index = 0;
         extension_index < extension_count; ++extension_index) {
      std::array<char, 4> tag{};
      reader.Bytes(tag.data(), tag.size());
      const std::uint32_t schema = reader.Scalar<std::uint32_t>();
      const std::uint32_t decoded_size =
          reader.Scalar<std::uint32_t>();
      if (decoded_size > kMaximumMetadataBytes) {
        // WMET is provenance for extraction and round-tripping, not data
        // needed by the runtime world. Very large combined retail maps may
        // legitimately carry more metadata than the runtime's allocation
        // safety limit, so retain the limit while safely ignoring only this
        // optional known extension.
        if (tag == std::array<char, 4>{'W', 'M', 'E', 'T'} &&
            schema == 1) {
          SkipStoredBytes(reader, decoded_size, "extension");
          continue;
        }
        throw std::runtime_error(
            "SKATE extension exceeds the metadata limit");
      }
      std::vector<std::uint8_t> payload = ReadStoredBytes(
          reader, decoded_size, "extension");
      if (tag == std::array<char, 4>{'W', 'M', 'E', 'T'} &&
          schema == 1) {
        map.retail_world_metadata_json.assign(
            reinterpret_cast<const char*>(payload.data()),
            payload.size());
      } else if (tag == std::array<char, 4>{'M', 'O', 'B', 'J'}) {
        if (map.map_object_schema_version != 0) {
          throw std::runtime_error(
              "SKATE package contains duplicate MOBJ extensions");
        }
        if (schema < 1 || schema > 3 ||
            (schema == 3 && package_version < 14)) {
          throw std::runtime_error(
              "SKATE MOBJ extension schema is incompatible with the package");
        }
        map.map_object_schema_version = schema;
        ReadMapObjects(std::move(payload), schema, map);
      } else if (tag == std::array<char, 4>{'R', 'C', 'I', 'D'}) {
        ReadRetailCollisionIdentity(std::move(payload), schema, map);
      } else if (tag == std::array<char, 4>{'B', 'G', 'R', 'P'}) {
        if (break_group_seen) {
          throw std::runtime_error(
              "SKATE package contains duplicate BGRP extensions");
        }
        break_group_seen = true;
        break_group_schema = schema;
        break_group_payload = std::move(payload);
      }
    }
    if (break_group_seen) {
      ReadBreakGroups(
          std::move(break_group_payload), break_group_schema, map);
    }
  }

  reader.RequireEnd();
  Validate(map);
  return map;
}

bool DecodeOwnedMapTexture(
    const ImageTexture& texture,
    std::string* error) {
  const std::uint64_t expected_u64 =
      std::uint64_t(texture.width) * texture.height * 4u;
  if (expected_u64 > std::numeric_limits<std::size_t>::max()) {
    if (error != nullptr) {
      *error = "decoded texture size exceeds the host address space";
    }
    return false;
  }
  const std::size_t expected =
      static_cast<std::size_t>(expected_u64);
  if (texture.rgba8.size() == expected) {
    return true;
  }
  if (!texture.rgba8.empty()) {
    if (error != nullptr) {
      *error = "decoded texture has the wrong byte count";
    }
    return false;
  }
  if (texture.stored_rgba8_method == kStorageRaw) {
    if (texture.stored_rgba8.size() != expected) {
      if (error != nullptr) {
        *error = "raw texture payload has the wrong byte count";
      }
      return false;
    }
    texture.rgba8 = texture.stored_rgba8;
    return true;
  }
  if (texture.stored_rgba8_method != kStorageDeflate) {
    if (error != nullptr) {
      *error = "texture storage method is unsupported";
    }
    return false;
  }
  if (expected == 0) {
    return texture.stored_rgba8.empty();
  }
  if (texture.stored_rgba8.empty() ||
      texture.stored_rgba8.size() >
          std::numeric_limits<uLong>::max() ||
      expected > std::numeric_limits<uLongf>::max()) {
    if (error != nullptr) {
      *error = "compressed texture payload is invalid";
    }
    return false;
  }
  std::vector<std::uint8_t> decoded(expected);
  uLongf decoded_size = static_cast<uLongf>(expected);
  const int status = uncompress(
      decoded.data(), &decoded_size,
      texture.stored_rgba8.data(),
      static_cast<uLong>(texture.stored_rgba8.size()));
  if (status != Z_OK || decoded_size != expected) {
    if (error != nullptr) {
      *error = "texture DEFLATE payload did not decode to the expected size";
    }
    return false;
  }
  texture.rgba8 = std::move(decoded);
  return true;
}

void ReleaseOwnedMapTexturePixels(const ImageTexture& texture) {
  // Legacy packages have no retained encoded payload, so their decoded
  // bytes remain authoritative. Version 9+ packages can recreate RGBA8
  // after a GPU-cache eviction.
  if (!texture.stored_rgba8.empty() ||
      (texture.width == 0 && texture.height == 0)) {
    std::vector<std::uint8_t>().swap(texture.rgba8);
  }
}

}  // namespace skate::world
