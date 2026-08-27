#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "skate/world/box3d_physics.h"
#include "skate/world/skate_object_package.h"
#include "skate/world/world_map.h"

namespace skate3::mechanics_sandbox::map {

// Matches the native scene renderer's decoded static vertex layout. The
// Blender export keeps this deliberately boring: positions, two UV sets,
// packed skin bytes, a normal, and a third UV set.
struct VisualVertex {
  float position[3];
  float uv[2];
  float uv2[2];
  uint8_t blend_weight[4];
  uint8_t blend_index[4];
  float normal[3];
  float uv3[2];
};

struct VisualDraw {
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  float color[4] = {};
  // pattern, world-space repeat scale, roughness, variation.
  float material[4] = {};
  skate::world::TextureId albedo_texture = 0;
  skate::world::TextureId indirect_lightmap = 0;
  skate::world::TextureId retail_chromaticity_texture = 0;
  skate::world::TextureId normal_texture = 0;
  skate::world::TextureId orm_texture = 0;
  skate::world::TextureId emissive_texture = 0;
  skate::world::TextureId secondary_albedo_texture = 0;
  skate::world::TextureId blend_mask_texture = 0;
  skate::world::TextureId retail_macro_texture = 0;
  skate::world::TextureId retail_decal_texture = 0;
  skate::world::TextureId retail_specular_texture = 0;
  skate::world::TextureId retail_detail_texture = 0;
  skate::world::TextureId retail_environment_texture = 0;
  skate::world::TextureId retail_normal2_texture = 0;
  uint32_t retail_shader_family = 0;
  uint32_t retail_render_flags = 0;
  float retail_macro_scale = 1.0f;
  float retail_macro_opacity = 1.0f;
  float retail_detail_scale = 0.0f;
  float retail_scroll_u = 0.0f;
  float retail_scroll_v = 0.0f;
  float baked_indirect_strength = 0.0f;
  float blend_factor = 0.0f;
  uint32_t blend_mask_channel = 0;
  uint32_t albedo_address_mode = 0;
  uint32_t secondary_address_mode = 0;
  uint32_t blend_mask_address_mode = 0;
  float skate2_lightmap_component = -1.0f;
  skate::world::SurfaceMaterial::AlphaMode alpha_mode =
      skate::world::SurfaceMaterial::AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;
  uint32_t presentation_depth_layer = 0;
  uint32_t presentation_depth_order = 0;
  bool cull_backfaces = false;
};

struct VisualMesh {
  std::vector<VisualVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<VisualDraw> draws;
};

struct VisualChunk : VisualMesh {
  int32_t cell_x = 0;
  int32_t cell_z = 0;
  uint32_t part = 0;
  float bounds_min[3] = {};
  float bounds_max[3] = {};
};

struct VisualCellRange {
  int32_t cell_x = 0;
  int32_t cell_z = 0;
  std::size_t first_chunk = 0;
  std::size_t chunk_count = 0;
};

struct VisualWorld {
  float chunk_size = 0.0f;
  std::size_t source_triangle_count = 0;
  std::size_t output_triangle_count = 0;
  std::vector<VisualChunk> chunks;
  std::vector<VisualCellRange> cells;
};

struct Contact {
  uint32_t id = 0;
  float point[3] = {};
  float normal[3] = {};
  float penetration = 0.0f;
};

struct GroundHit {
  uint32_t id = 0;
  float point[3] = {};
  float normal[3] = {};
  float distance = 0.0f;
};

struct RayHit {
  uint32_t id = 0;
  uint32_t material = 0;
  float point[3] = {};
  float normal[3] = {};
  float distance = 0.0f;
};

struct WaterTelemetry {
  uint64_t simulation_steps = 0;
  uint64_t dropped_frames = 0;
  float minimum_displacement = 0.0f;
  float maximum_displacement = 0.0f;
  float mean_displacement = 0.0f;
  float kinetic_energy = 0.0f;
};

struct MovingLightSnapshot {
  float position[3] = {};
  float direction[3] = {};
  float color[3] = {};
  float source_radius = 0.0f;
  float influence_radius = 0.0f;
  float intensity = 0.0f;
  float spot_inner_cosine = 1.0f;
  float spot_outer_cosine = 1.0f;
  uint32_t type = 0;
  bool visible_source = false;
};

struct WeatherSnapshot {
  float elapsed_seconds = 0.0f;
  float rain_intensity = 0.0f;
  float flash_intensity = 0.0f;
  float wind[3] = {};
  float lightning_position[3] = {};
  uint64_t strike_count = 0;
  uint64_t thunder_count = 0;
};

// This adapter converts the renderer-neutral project-owned map into the
// native sandbox renderer's legacy vertex layout. Visual and observer
// collision geometry therefore share one handwritten source definition.
const VisualWorld& ActiveVisualWorld();
const VisualMesh& ActiveSkyMesh();
const VisualMesh& ActiveEditorGizmoVisualMesh();
const std::vector<VisualMesh>& ActiveEditableObjectVisualMeshes(
    std::size_t index);
const VisualMesh& ActiveKinematicVisualMesh(std::size_t index);
const VisualMesh& ActiveHingedDoorVisualMesh(std::size_t index);
const VisualMesh& ActiveWaterVisualMesh();
const VisualMesh& ActiveWaterPusherVisualMesh();
const VisualMesh& ActiveMovingLightVisualMesh();
const VisualMesh& ActiveRemoteSkaterVisualMesh();
const VisualMesh& ActiveRainVisualMesh();
const VisualMesh& ActiveLightningVisualMesh();
const skate::world::MapDefinition& ActiveDefinition();
std::size_t AppendSpawnedObject(skate::world::SkateObjectAsset asset,
    skate::world::Vec3 map_position);
void AdvanceOwnedPhysics(double frame_seconds);
bool ActivePhysicsObjectPose(std::size_t index,
    skate::world::PhysicsObjectPose& out);
skate::world::PhysicsTelemetry ActivePhysicsTelemetry();
void UpdateOwnedPhysicsPlayerProxy(skate::world::Vec3 position,
    skate::world::Vec3 linear_velocity,
    bool active);
const skate::world::ImageTexture* ActiveImageTexture(
    skate::world::TextureId id);
const char* ActiveMapName();
// Exact package path selected for this process. Retail sidecar resources use
// this to resolve files beside the active .skate package.
const char* ActiveMapPackagePath();
std::size_t ActiveSurfaceCount();
std::size_t ActiveRampCount();
std::size_t ActiveEditableObjectCount();
std::size_t ActiveKinematicObjectCount();
std::size_t ActiveHingedDoorCount();
std::size_t ActiveWaterBasinCount();
std::size_t ActiveRaytracedMirrorCount();
std::size_t ActiveRaytracedPuddleCount();
std::size_t ActiveMovingLightCount();

// Advances the renderer-owned animation clock once and publishes one coherent
// pose snapshot consumed by raster, collision-independent visuals, and DXR.
void AdvanceMovingLights(float frame_seconds);
bool ActiveMovingLightSnapshot(std::size_t index, MovingLightSnapshot& out);

// One deterministic clock evaluates the project-owned celestial contract.
// Raster, sky, DXR, character adaptation, and telemetry consume this same
// published state.
void AdvanceDayNightCycle(float frame_seconds);
skate::world::DayNightState ActiveDayNightState();
bool DynamicWorldLightingEnabled();

enum class WorldLightingSetting {
  kPaused,
  kTimeOfDay,
  kCycleDuration,
  kPingPong,
  kStartHour,
  kEndHour,
  kOrbitAzimuthDegrees,
  kSkyRed,
  kSkyGreen,
  kSkyBlue,
  kSunlightRed,
  kSunlightGreen,
  kSunlightBlue,
  kSunIntensity,
  kMoonIntensity,
  kDayAmbient,
  kNightAmbient,
  kDynamicLightingEnabled,
};

struct WorldLightingSettings {
  bool available = false;
  bool paused = false;
  bool ping_pong = false;
  bool dynamic_lighting_enabled = true;
  float time_of_day_hours = 0.0f;
  float cycle_duration_seconds = 0.0f;
  float start_hour = 0.0f;
  float end_hour = 0.0f;
  float orbit_azimuth_degrees = 0.0f;
  float sky_red = 1.0f;
  float sky_green = 1.0f;
  float sky_blue = 1.0f;
  float sunlight_red = 1.0f;
  float sunlight_green = 1.0f;
  float sunlight_blue = 1.0f;
  float sun_intensity = 0.0f;
  float moon_intensity = 0.0f;
  float day_ambient = 0.0f;
  float night_ambient = 0.0f;
};

// Live session controls layered over the active map's authored SKATE values.
// Reset restores every value (and the clock position) to map defaults.
WorldLightingSettings ActiveWorldLightingSettings();
void SetWorldLightingSetting(WorldLightingSetting setting, float value);
void ResetWorldLightingSettings();

// One deterministic storm clock drives rain placement, lightning light/bolt,
// puddle ripples, and delayed procedural thunder.
void AdvanceWeather(float frame_seconds);
void UpdateRainVisualMesh(const float local_camera_position[3]);
WeatherSnapshot ActiveWeatherSnapshot();
bool ActiveLightningLightSnapshot(MovingLightSnapshot& out);

// Render-thread adapter for the project-owned fixed-step fluid solver.
// Returns false when the map has no authored basin.
bool AdvanceWaterSimulation(float elapsed_seconds);
bool ActiveWaterPusherPose(float out_position[3]);
WaterTelemetry ActiveWaterTelemetry();

bool QueryContact(const float position[3], float radius, Contact& out);
bool QueryRaySegment(const float start[3], const float delta[3], RayHit& out);
bool QueryGround(const float position[3], float probe_above, float probe_below,
                 GroundHit& out);
bool QueryLowestGround(const float position[3], float probe_above,
                       float probe_below, GroundHit& out);

}  // namespace skate3::mechanics_sandbox::map
