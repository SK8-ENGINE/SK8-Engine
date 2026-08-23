#pragma once

#include "skate/world/math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace skate::world {

using MaterialId = std::uint32_t;
using TextureId = std::uint32_t;
using SurfaceId = std::uint32_t;
using GrindRailId = std::uint32_t;
using NpcRouteId = std::uint32_t;
using KinematicObjectId = std::uint32_t;
using HingedDoorId = std::uint32_t;
using WaterBasinId = std::uint32_t;
using RaytracedMirrorId = std::uint32_t;
using RaytracedPuddleId = std::uint32_t;
using MovingLightOrbId = std::uint32_t;

enum class SurfaceFlags : std::uint32_t {
  None = 0,
  Skateable = 1u << 0,
  Grindable = 1u << 1,
  Wall = 1u << 2,
};

// Renderer-neutral authored surface treatment. The recomp adapter currently
// evaluates these as procedural texture families, while a future standalone
// renderer can resolve the same contract to image/PBR assets.
enum class MaterialPattern : std::uint32_t {
  Solid = 0,
  Concrete = 1,
  Asphalt = 2,
  Brick = 3,
  Metal = 4,
  Wood = 5,
  Tile = 6,
  Grass = 7,
  Painted = 8,
};

constexpr SurfaceFlags operator|(SurfaceFlags left, SurfaceFlags right) {
  return static_cast<SurfaceFlags>(
      static_cast<std::uint32_t>(left) |
      static_cast<std::uint32_t>(right));
}

constexpr bool HasFlag(SurfaceFlags value, SurfaceFlags flag) {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0;
}

struct SurfaceMaterial {
  MaterialId id = 0;
  std::string name;
  float friction = 0.8f;
  float restitution = 0.0f;
  SurfaceFlags flags = SurfaceFlags::Skateable;
  Vec3 display_color{0.5f, 0.5f, 0.5f};
  MaterialPattern pattern = MaterialPattern::Solid;
  float texture_scale = 1.0f;
  float roughness = 0.8f;
  float variation = 0.15f;
  float emissive_intensity = 0.0f;
  // Zero keeps the procedural material path. Imported maps reference
  // package-owned RGBA8 textures here; the renderer remains free to upload
  // or transcode them however its backend requires.
  TextureId albedo_texture = 0;
  TextureId indirect_lightmap = 0;
  // Optional modern material maps imported from Blender. ORM channels are
  // R=ambient occlusion, G=roughness, B=metallic.
  TextureId normal_texture = 0;
  TextureId orm_texture = 0;
  TextureId emissive_texture = 0;
  float baked_indirect_strength = 0.0f;
  enum class AlphaMode : std::uint8_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2,
  };
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;
  // Native Skate 3 RenderWare collision channels. These values deliberately
  // remain raw IDs so authored contacts can drive the retail wheel, foot,
  // grind, VFX, and special-surface behavior without host-side imitation.
  std::uint8_t skate_audio_surface = 3;    // Concrete_Polished
  std::uint8_t skate_physics_surface = 1;  // Smooth
  std::uint8_t skate_surface_pattern = 0;  // None
};

enum class TextureColorSpace : std::uint32_t {
  Linear = 0,
  Srgb = 1,
};

struct ImageTexture {
  TextureId id = 0;
  std::string name;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  TextureColorSpace color_space = TextureColorSpace::Linear;
  std::vector<std::uint8_t> rgba8;
};

struct SpawnPoint {
  Vec3 position;
  float heading_radians = 0.0f;
};

struct SkyDefinition {
  bool enabled = false;
  Vec3 zenith_color{0.08f, 0.20f, 0.42f};
  Vec3 horizon_color{0.52f, 0.72f, 0.90f};
  Vec3 nadir_color{0.12f, 0.15f, 0.20f};
};

struct DirectionalLightDefinition {
  Vec3 direction_to_light{0.35f, 0.85f, 0.38f};
  Vec3 color{1.0f, 0.93f, 0.82f};
  float intensity = 1.15f;
  float ambient = 0.28f;
};

// Renderer-neutral accelerated celestial cycle. One pure evaluation supplies
// the sky palette, visible sun/moon, and active directional light to every
// renderer. The short duration is an authored demo value, not a simulation
// timestep. A duration of zero freezes the world at start_time_hours.
struct DayNightCycleDefinition {
  bool enabled = false;
  // When enabled, duration_seconds is one complete out-and-back loop:
  // start_time_hours -> end_time_hours -> start_time_hours. The cosine
  // easing reaches zero velocity at both ends, avoiding a visible reversal.
  bool ping_pong = false;
  float duration_seconds = 96.0f;
  float start_time_hours = 9.0f;
  float end_time_hours = 17.0f;
  float orbit_azimuth_radians = 0.62f;
  Vec3 day_zenith{0.09f, 0.34f, 0.72f};
  Vec3 day_horizon{0.58f, 0.78f, 0.98f};
  Vec3 day_nadir{0.18f, 0.25f, 0.34f};
  Vec3 twilight_zenith{0.045f, 0.10f, 0.26f};
  Vec3 twilight_horizon{1.0f, 0.32f, 0.10f};
  Vec3 twilight_nadir{0.05f, 0.035f, 0.06f};
  Vec3 night_zenith{0.007f, 0.015f, 0.045f};
  Vec3 night_horizon{0.045f, 0.085f, 0.17f};
  Vec3 night_nadir{0.008f, 0.014f, 0.032f};
  Vec3 sun_color{1.0f, 0.92f, 0.78f};
  Vec3 moon_color{0.42f, 0.56f, 0.92f};
  float sun_intensity = 1.25f;
  float moon_intensity = 0.18f;
  float day_ambient = 0.32f;
  float night_ambient = 0.11f;
  // Authored RGB tint applied after the time-of-day sky palettes are blended.
  // White preserves the palette exactly. This is also the simple live colour
  // control exposed by the runtime world-lighting menu.
  Vec3 sky_tint{1.0f, 1.0f, 1.0f};
};

struct DayNightState {
  float elapsed_seconds = 0.0f;
  float phase = 0.0f;
  float time_of_day_hours = 0.0f;
  Vec3 sun_direction_to_light;
  Vec3 moon_direction_to_light;
  Vec3 light_direction_to_light;
  Vec3 light_color;
  Vec3 sky_zenith;
  Vec3 sky_horizon;
  Vec3 sky_nadir;
  float light_intensity = 0.0f;
  float ambient = 0.0f;
  float daylight_amount = 0.0f;
  float night_amount = 0.0f;
  float twilight_amount = 0.0f;
  float star_visibility = 0.0f;
  float sun_visibility = 0.0f;
  float moon_visibility = 0.0f;
  bool sun_is_key_light = true;
};

// Renderer-neutral storm settings. The deterministic weather clock lives in
// the runtime adapter, while the authored density, wind, and lightning
// cadence remain portable map data.
struct WeatherDefinition {
  bool enabled = false;
  float rain_intensity = 0.0f;
  Vec3 wind{0.0f, 0.0f, 0.0f};
  float rain_fall_speed = 24.0f;
  float lightning_interval_min = 9.0f;
  float lightning_interval_max = 16.0f;
  float thunder_delay = 0.9f;
};

struct RenderVertex {
  Vec3 position;
  Vec3 normal;
  Vec2 uv;
  MaterialId material = 0;
  Vec2 lightmap_uv;
};

struct RenderMesh {
  std::vector<RenderVertex> vertices;
  std::vector<std::uint32_t> indices;
};

struct CollisionTriangle {
  Vec3 a;
  Vec3 b;
  Vec3 c;
  Vec3 normal;
  SurfaceId surface = 0;
  MaterialId material = 0;
};

// The first 120 bytes of one retail Pegasus tSplineData segment. Values are
// stored as host-order IEEE-754 bit patterns so an extracted retail segment
// can round-trip without changing a single coefficient or auxiliary field.
struct NativeGrindSegment {
  std::array<std::uint32_t, 30> words{};
};

// A grind rail is independent from its visible/collision mesh. Hand-authored
// maps use readable points. Retail imports instead preserve exact native cubic
// segment payloads plus their original spline identity/type.
struct GrindRail {
  GrindRailId id = 0;
  std::string name;
  std::vector<Vec3> points;
  bool closed = false;
  std::uint64_t retail_spline_id = 0;
  std::uint64_t retail_type_signature = 0;
  std::uint32_t retail_flags = 0;
  std::uint32_t retail_trailing_word = 0;
  std::vector<NativeGrindSegment> native_segments;
};

// A route consumed by Skate's native AI skater controller. The owned map
// supplies only world-space guide points and population intent; steering,
// board physics, animation, tricks, collisions, and bails remain native.
struct NpcRoute {
  NpcRouteId id = 0;
  std::string name;
  std::vector<Vec3> points;
  bool closed = true;
  std::uint32_t skater_count = 1;
  float speed = 5.5f;
  float spawn_spacing = 3.0f;
};

// A project-owned moving collision primitive. Geometry is authored around
// local origin while the deterministic path supplies its world-map pose.
// Keeping shape and motion separate lets adapters submit the same pose to
// rendering and physics without baking a new mesh every frame.
struct KinematicBox {
  KinematicObjectId id = 0;
  std::string name;
  Vec3 local_min;
  Vec3 local_max;
  Vec3 path_start;
  Vec3 path_end;
  float travel_seconds = 1.0f;
  SurfaceId surface = 0;
  MaterialId material = 0;
};

struct KinematicPose {
  Vec3 position;
  Vec3 velocity;
  float path_alpha = 0.0f;
  bool returning = false;
};

// A genuinely simulated one-degree-of-freedom rigid body. Door render and
// collision geometry are authored in a hinge-local orthonormal frame:
// X points across the closed leaf, Y follows hinge_axis, and Z is thickness.
// The runtime rotates this complete local frame around the hinge in response
// to physical contact; no opening timeline or trigger animation is stored.
struct HingedDoor {
  HingedDoorId id = 0;
  std::string name;
  Vec3 hinge_position;
  Vec3 hinge_axis{0.0f, 1.0f, 0.0f};
  Vec3 closed_width_axis{1.0f, 0.0f, 0.0f};
  Vec3 closed_depth_axis{0.0f, 0.0f, 1.0f};
  Vec3 local_min;
  Vec3 local_max;
  float minimum_angle_radians = -1.75f;
  float maximum_angle_radians = 1.75f;
  float initial_angle_radians = 0.0f;
  float mass = 32.0f;
  float angular_damping = 2.2f;
  float return_spring_strength = 0.0f;
  float maximum_angular_speed = 8.0f;
  float contact_impulse_scale = 1.0f;
  float static_friction = 0.55f;
  float restitution = 0.02f;
  SurfaceId surface = 0;
  RenderMesh render_mesh;
  std::vector<CollisionTriangle> collision_triangles;
};

struct HingedDoorPose {
  float angle_radians = 0.0f;
  float angular_velocity = 0.0f;
};

// Authored fluid domain and its independently moving displacement body.
// The basin geometry remains ordinary map surfaces; this contract supplies
// the simulation bounds, resolution, and pusher motion.
struct WaterBasin {
  WaterBasinId id = 0;
  std::string name;
  Vec3 minimum;
  Vec3 maximum;
  float rest_surface_height = 0.0f;
  std::uint32_t columns = 49;
  std::uint32_t rows = 49;
  float damping = 0.34f;
  Vec3 pusher_local_min;
  Vec3 pusher_local_max;
  Vec3 pusher_path_start;
  Vec3 pusher_path_end;
  float pusher_travel_seconds = 4.0f;
  MaterialId pusher_material = 0;
};

// Renderer-neutral authored planar mirror. The owned map supplies geometry
// and orientation; a capable renderer may use hardware ray tracing, while a
// future fallback can preserve the same map contract with another method.
struct RaytracedMirror {
  RaytracedMirrorId id = 0;
  std::string name;
  Vec3 center;
  Vec3 right{1.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  float half_width = 1.0f;
  float half_height = 1.0f;
};

// A horizontal, irregular-edged wet patch. It is presentation-only and sits
// just above authoritative collision geometry. DXR renderers trace a true
// reflected ray; fallback renderers can preserve the same footprint.
struct RaytracedPuddle {
  RaytracedPuddleId id = 0;
  std::string name;
  Vec3 center;
  Vec3 right{1.0f, 0.0f, 0.0f};
  Vec3 forward{0.0f, 0.0f, -1.0f};
  float half_width = 1.0f;
  float half_length = 1.0f;
  float reflectivity = 0.72f;
  float ripple_strength = 0.018f;
};

// A renderer-neutral moving spherical area light. The two orbit axes define
// an ellipse around orbit_center; renderers use source_radius both for the
// visible emissive sphere and for soft-shadow sampling.
enum class LocalLightType : std::uint32_t {
  Point = 0,
  Spot = 1,
  Area = 2,
};

struct MovingLightOrb {
  MovingLightOrbId id = 0;
  std::string name;
  LocalLightType type = LocalLightType::Point;
  Vec3 orbit_center;
  Vec3 orbit_axis_u{1.0f, 0.0f, 0.0f};
  Vec3 orbit_axis_v{0.0f, 1.0f, 0.0f};
  Vec3 direction{0.0f, -1.0f, 0.0f};
  Vec3 color{1.0f, 1.0f, 1.0f};
  float source_radius = 0.25f;
  float influence_radius = 10.0f;
  float intensity = 8.0f;
  float period_seconds = 6.0f;
  float phase_radians = 0.0f;
  float spot_inner_cosine = 1.0f;
  float spot_outer_cosine = 1.0f;
  bool visible_source = true;
};

struct MovingLightOrbPose {
  Vec3 position;
  Vec3 velocity;
};

struct MapDefinition {
  std::string name;
  SpawnPoint spawn;
  SkyDefinition sky;
  DirectionalLightDefinition sun;
  DayNightCycleDefinition day_night_cycle;
  WeatherDefinition weather;
  std::vector<SurfaceMaterial> materials;
  std::vector<ImageTexture> textures;
  RenderMesh render_mesh;
  std::vector<CollisionTriangle> collision_triangles;
  std::vector<GrindRail> grind_rails;
  std::vector<NpcRoute> npc_routes;
  std::vector<KinematicBox> kinematic_boxes;
  std::vector<HingedDoor> hinged_doors;
  std::vector<WaterBasin> water_basins;
  std::vector<RaytracedMirror> raytraced_mirrors;
  std::vector<RaytracedPuddle> raytraced_puddles;
  std::vector<MovingLightOrb> moving_light_orbs;
};

// Evaluates a cosine-eased ping-pong path. It is a pure function so replay,
// the recomp adapter, and the standalone game can produce the same pose.
KinematicPose EvaluateKinematicBox(const KinematicBox& object,
                                   float elapsed_seconds);
MovingLightOrbPose EvaluateMovingLightOrb(const MovingLightOrb& light,
                                          float elapsed_seconds);
DayNightState EvaluateDayNightCycle(
    const DayNightCycleDefinition& cycle, float elapsed_seconds);

struct RayHit {
  bool hit = false;
  float distance = std::numeric_limits<float>::infinity();
  Vec3 point;
  Vec3 normal;
  SurfaceId surface = 0;
  MaterialId material = 0;
};

struct Contact {
  Vec3 point;
  Vec3 normal;
  float penetration = 0.0f;
  SurfaceId surface = 0;
  MaterialId material = 0;
};

class MapBuilder {
 public:
  explicit MapBuilder(std::string name);

  MaterialId AddMaterial(std::string name,
                         float friction,
                         float restitution,
                         SurfaceFlags flags,
                         Vec3 display_color = {0.5f, 0.5f, 0.5f},
                         MaterialPattern pattern = MaterialPattern::Solid,
                         float texture_scale = 1.0f,
                         float roughness = 0.8f,
                         float variation = 0.15f,
                         float emissive_intensity = 0.0f);
  void SetSpawn(Vec3 position, float heading_radians);
  void SetSky(Vec3 zenith_color, Vec3 horizon_color, Vec3 nadir_color);
  void SetDirectionalSun(Vec3 direction_to_light, Vec3 color,
                         float intensity, float ambient);
  void SetDayNightCycle(DayNightCycleDefinition cycle);
  void SetWeather(float rain_intensity, Vec3 wind,
                  float rain_fall_speed, float lightning_interval_min,
                  float lightning_interval_max, float thunder_delay);
  GrindRailId AddGrindRail(std::string name,
                           std::vector<Vec3> points,
                           bool closed = false);
  KinematicObjectId AddKinematicBox(std::string name,
                                    SurfaceId surface,
                                    MaterialId material,
                                    Vec3 local_min,
                                    Vec3 local_max,
                                    Vec3 path_start,
                                    Vec3 path_end,
                                    float travel_seconds);
  WaterBasinId AddWaterBasin(WaterBasin basin);
  RaytracedMirrorId AddRaytracedMirror(RaytracedMirror mirror);
  RaytracedPuddleId AddRaytracedPuddle(RaytracedPuddle puddle);
  MovingLightOrbId AddMovingLightOrb(MovingLightOrb light);

  void AddQuad(SurfaceId surface,
               MaterialId material,
               Vec3 a,
               Vec3 b,
               Vec3 c,
               Vec3 d,
               Vec2 uv_scale = {1.0f, 1.0f});

  void AddWedgeX(SurfaceId surface,
                 MaterialId material,
                 float x_min,
                 float x_max,
                 float z_min,
                 float z_max,
                 float y_low,
                 float y_high);

  void AddBox(SurfaceId surface,
              MaterialId material,
              Vec3 minimum,
              Vec3 maximum);

  MapDefinition Build() &&;

 private:
  void RequireMaterial(MaterialId material) const;

  MapDefinition map_;
  MaterialId next_material_id_ = 1;
  GrindRailId next_grind_rail_id_ = 1;
  KinematicObjectId next_kinematic_object_id_ = 1;
  WaterBasinId next_water_basin_id_ = 1;
  RaytracedMirrorId next_raytraced_mirror_id_ = 1;
  RaytracedPuddleId next_raytraced_puddle_id_ = 1;
  MovingLightOrbId next_moving_light_orb_id_ = 1;
};

class WorldMap {
 public:
  explicit WorldMap(MapDefinition definition);

  const MapDefinition& Definition() const;
  const SurfaceMaterial* FindMaterial(MaterialId id) const;

  RayHit RayCast(Vec3 origin,
                 Vec3 direction,
                 float maximum_distance) const;
  RayHit ProbeGround(Vec3 origin, float maximum_distance) const;
  RayHit ProbeLowestSkateableGround(Vec3 origin,
                                    float maximum_distance) const;
  std::vector<Contact> QuerySphere(Vec3 center,
                                   float radius,
                                   std::size_t maximum_contacts = 8) const;

 private:
  MapDefinition definition_;
};

}  // namespace skate::world
