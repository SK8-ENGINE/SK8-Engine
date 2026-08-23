#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
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
constexpr std::uint32_t kEndianMarker = 0x12345678u;
constexpr std::uint32_t kStorageRaw = 0;
constexpr std::uint32_t kStorageDeflate = 1;
constexpr std::uint32_t kMaximumCount = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaximumTextureDimension = 8192u;
constexpr std::uint32_t kMaximumTextureBytes =
    kMaximumTextureDimension * kMaximumTextureDimension * 4u;
constexpr std::uint32_t kMaximumStringBytes = 64u * 1024u;
constexpr std::uint64_t kMaximumPackageBytes = 2ull * 1024ull * 1024ull * 1024ull;
constexpr float kAreaEpsilon = 1.0e-10f;

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

  void Bytes(void* destination, std::size_t size) {
    if (size > bytes_.size() - offset_) {
      throw std::runtime_error("SKATE package is truncated");
    }
    std::memcpy(destination, bytes_.data() + offset_, size);
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

std::vector<std::uint8_t> ReadStoredBytes(
    Reader& reader,
    std::size_t expected_size,
    const char* label) {
  const std::uint32_t method = reader.Scalar<std::uint32_t>();
  const std::uint32_t stored_size = reader.Scalar<std::uint32_t>();
  if (stored_size > kMaximumPackageBytes) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " stored size is invalid");
  }
  std::vector<std::uint8_t> stored = reader.ByteVector(stored_size);
  if (method == kStorageRaw) {
    if (stored.size() != expected_size) {
      throw std::runtime_error(
          std::string("SKATE ") + label + " raw size is invalid");
    }
    return stored;
  }
  if (method != kStorageDeflate) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " storage method is unsupported");
  }
  if (expected_size > std::numeric_limits<uLongf>::max()) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " decoded size is invalid");
  }
  std::vector<std::uint8_t> decoded(expected_size);
  uLongf decoded_size = static_cast<uLongf>(expected_size);
  const int result = uncompress(
      decoded.data(),
      &decoded_size,
      stored.data(),
      static_cast<uLong>(stored.size()));
  if (result != Z_OK || decoded_size != expected_size) {
    throw std::runtime_error(
        std::string("SKATE ") + label + " DEFLATE payload is invalid");
  }
  return decoded;
}

void ReadRenderVertices(
    Reader& reader,
    std::uint32_t count,
    std::vector<RenderVertex>& destination) {
  destination.reserve(destination.size() + count);
  for (std::uint32_t index = 0; index < count; ++index) {
    RenderVertex vertex;
    vertex.position = reader.Vector3();
    vertex.normal = reader.Vector3();
    vertex.uv = reader.Vector2();
    vertex.lightmap_uv = reader.Vector2();
    vertex.material = reader.Scalar<MaterialId>();
    destination.push_back(vertex);
  }
}

void ReadIndices(
    Reader& reader,
    std::uint32_t count,
    std::vector<std::uint32_t>& destination) {
  destination.reserve(destination.size() + count);
  for (std::uint32_t index = 0; index < count; ++index) {
    destination.push_back(reader.Scalar<std::uint32_t>());
  }
}

void ReadCollisionTriangles(
    Reader& reader,
    std::uint32_t count,
    std::vector<CollisionTriangle>& destination) {
  destination.reserve(destination.size() + count);
  for (std::uint32_t index = 0; index < count; ++index) {
    CollisionTriangle triangle;
    triangle.a = reader.Vector3();
    triangle.b = reader.Vector3();
    triangle.c = reader.Vector3();
    triangle.surface = reader.Scalar<SurfaceId>();
    triangle.material = reader.Scalar<MaterialId>();
    destination.push_back(triangle);
  }
}

bool Finite(Vec2 value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

void RequireCount(std::uint32_t count, const char* label) {
  if (count > kMaximumCount) {
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

const ImageTexture* FindTexture(const MapDefinition& map, TextureId id) {
  const auto found = std::find_if(
      map.textures.begin(), map.textures.end(),
      [id](const ImageTexture& texture) { return texture.id == id; });
  return found == map.textures.end() ? nullptr : &*found;
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
        material.skate_physics_surface > 12 ||
        material.skate_surface_pattern > 15 ||
        (material.albedo_texture != 0 &&
         FindTexture(map, material.albedo_texture) == nullptr) ||
        (material.indirect_lightmap != 0 &&
         FindTexture(map, material.indirect_lightmap) == nullptr) ||
        (material.normal_texture != 0 &&
         FindTexture(map, material.normal_texture) == nullptr) ||
        (material.orm_texture != 0 &&
         FindTexture(map, material.orm_texture) == nullptr) ||
        (material.emissive_texture != 0 &&
         FindTexture(map, material.emissive_texture) == nullptr)) {
      throw std::runtime_error("SKATE material table is invalid");
    }
  }

  std::unordered_set<TextureId> texture_ids;
  for (const ImageTexture& texture : map.textures) {
    const std::uint64_t expected =
        std::uint64_t(texture.width) * texture.height * 4u;
    if (texture.id == 0 || texture.name.empty() ||
        !texture_ids.insert(texture.id).second ||
        texture.width == 0 || texture.height == 0 ||
        texture.width > kMaximumTextureDimension ||
        texture.height > kMaximumTextureDimension ||
        expected != texture.rgba8.size()) {
      throw std::runtime_error("SKATE embedded texture is invalid");
    }
  }

  if (map.render_mesh.indices.size() % 3 != 0) {
    throw std::runtime_error(
        "SKATE render indices do not form complete triangles");
  }
  for (const RenderVertex& vertex : map.render_mesh.vertices) {
    if (!Finite(vertex.position) || !Finite(vertex.normal) ||
        !Finite(vertex.uv) || !Finite(vertex.lightmap_uv) ||
        FindMaterial(map, vertex.material) == nullptr) {
      throw std::runtime_error("SKATE render vertex is invalid");
    }
  }
  for (std::uint32_t index : map.render_mesh.indices) {
    if (index >= map.render_mesh.vertices.size()) {
      throw std::runtime_error("SKATE render index is out of range");
    }
  }
  for (CollisionTriangle& triangle : map.collision_triangles) {
    const Vec3 cross = Cross(triangle.b - triangle.a,
                             triangle.c - triangle.a);
    if (!Finite(triangle.a) || !Finite(triangle.b) ||
        !Finite(triangle.c) ||
        LengthSquared(cross) <= kAreaEpsilon ||
        triangle.surface == 0 ||
        FindMaterial(map, triangle.material) == nullptr) {
      throw std::runtime_error("SKATE collision triangle is invalid");
    }
    triangle.normal = Normalize(cross);
  }
  for (const GrindRail& rail : map.grind_rails) {
    if (rail.id == 0 || rail.name.empty() || rail.points.size() < 2 ||
        !std::all_of(
            rail.points.begin(), rail.points.end(),
            [](Vec3 point) { return Finite(point); })) {
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
          FindMaterial(map, vertex.material) == nullptr) {
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
          FindMaterial(map, triangle.material) == nullptr) {
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
  const bool skate_magic =
      std::memcmp(magic.data(), "SKATE", 5) == 0 &&
      std::isdigit(static_cast<unsigned char>(magic[5])) &&
      std::isdigit(static_cast<unsigned char>(magic[6])) &&
      magic[7] == '\0';
  const int package_version =
      skate_magic ? (magic[5] - '0') * 10 + (magic[6] - '0') : 0;
  if (skate_magic && package_version > 9) {
    throw std::runtime_error(
        "SKATE v" + std::to_string(package_version) +
        " requires a newer Custom Engine Layer release");
  }
  if ((!version_1 && !version_2 && !version_3 && !version_4 &&
       !version_5 && !version_6 && !version_7 && !version_8 &&
       !version_9) ||
      reader.Scalar<std::uint32_t>() != kEndianMarker) {
    throw std::runtime_error(
        "file is not a supported little-endian SKATE v1-v9 package");
  }

  MapDefinition map;
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
  RequireCount(vertex_count, "vertex");
  RequireCount(index_count, "index");
  RequireCount(collision_count, "collision");
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
    if (texture.width == 0 || texture.height == 0 ||
        texture.width > kMaximumTextureDimension ||
        texture.height > kMaximumTextureDimension ||
        expected_byte_count > kMaximumTextureBytes) {
      throw std::runtime_error("SKATE embedded texture is invalid");
    }
    if (version_9) {
      texture.rgba8 = ReadStoredBytes(
          reader,
          static_cast<std::size_t>(expected_byte_count),
          "embedded texture");
    } else {
      const std::uint32_t byte_count = reader.Scalar<std::uint32_t>();
      if (byte_count != expected_byte_count) {
        throw std::runtime_error("SKATE embedded texture is invalid");
      }
      texture.rgba8 = reader.ByteVector(byte_count);
    }
    map.textures.push_back(std::move(texture));
  }

  if (version_9) {
    Reader vertex_reader(ReadStoredBytes(
        reader,
        std::size_t(vertex_count) * sizeof(float) * 10u +
            std::size_t(vertex_count) * sizeof(std::uint32_t),
        "visual vertex block"));
    ReadRenderVertices(
        vertex_reader, vertex_count, map.render_mesh.vertices);
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
            (sizeof(float) * 9u + sizeof(std::uint32_t) * 2u),
        "collision block"));
    ReadCollisionTriangles(
        collision_reader, collision_count, map.collision_triangles);
    collision_reader.RequireEnd();
  } else {
    ReadRenderVertices(reader, vertex_count, map.render_mesh.vertices);
    ReadIndices(reader, index_count, map.render_mesh.indices);
    ReadCollisionTriangles(
        reader, collision_count, map.collision_triangles);
  }

  map.grind_rails.reserve(rail_count);
  for (std::uint32_t index = 0; index < rail_count; ++index) {
    GrindRail rail;
    rail.id = index + 1;
    rail.name = reader.String();
    rail.closed = reader.Scalar<std::uint32_t>() != 0;
    const std::uint32_t point_count = reader.Scalar<std::uint32_t>();
    RequireCount(point_count, "grind point");
    rail.points.reserve(point_count);
    for (std::uint32_t point = 0; point < point_count; ++point) {
      rail.points.push_back(reader.Vector3());
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
      door.render_mesh.vertices.push_back(vertex);
    }
    door.render_mesh.indices.reserve(door_index_count);
    for (std::uint32_t door_index = 0;
         door_index < door_index_count; ++door_index) {
      door.render_mesh.indices.push_back(
          reader.Scalar<std::uint32_t>());
    }
    door.collision_triangles.reserve(door_collision_count);
    for (std::uint32_t triangle_index = 0;
         triangle_index < door_collision_count; ++triangle_index) {
      CollisionTriangle triangle;
      triangle.a = reader.Vector3();
      triangle.b = reader.Vector3();
      triangle.c = reader.Vector3();
      triangle.surface = reader.Scalar<SurfaceId>();
      triangle.material = reader.Scalar<MaterialId>();
      door.collision_triangles.push_back(triangle);
    }
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

  reader.RequireEnd();
  Validate(map);
  return map;
}

}  // namespace skate::world
