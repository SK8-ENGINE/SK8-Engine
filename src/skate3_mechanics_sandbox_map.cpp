#include "skate3_mechanics_sandbox_map.h"

#include "skate/world/maps.h"
#include "skate/world/owned_map_package.h"
#include "skate/world/render_world.h"
#include "skate/world/water_simulation.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#endif

namespace skate3::mechanics_sandbox::map {
namespace {

std::atomic<uint64_t> g_water_simulation_steps{0};
std::atomic<uint64_t> g_water_dropped_frames{0};
std::atomic<uint32_t> g_water_minimum_bits{0};
std::atomic<uint32_t> g_water_maximum_bits{0};
std::atomic<uint32_t> g_water_mean_bits{0};
std::atomic<uint32_t> g_water_energy_bits{0};
float g_moving_light_time = 0.0f;
std::vector<MovingLightSnapshot> g_moving_light_snapshots;
float g_day_night_elapsed = 0.0f;
bool g_day_night_initialized = false;
skate::world::DayNightState g_day_night_state;
skate::world::DayNightCycleDefinition g_day_night_cycle;
bool g_day_night_runtime_initialized = false;
bool g_day_night_paused = false;
bool g_dynamic_lighting_enabled = true;
float g_day_night_manual_hour = 0.0f;
std::mutex g_day_night_mutex;
WeatherSnapshot g_weather_snapshot;
VisualMesh g_rain_visual;
VisualMesh g_lightning_visual;

const skate::world::WorldMap& ActiveWorld();

constexpr float kRadiansToDegrees = 57.29577951308232f;
constexpr float kDegreesToRadians = 0.017453292519943295f;

float WrapHour(float hour) {
  hour = std::fmod(hour, 24.0f);
  return hour < 0.0f ? hour + 24.0f : hour;
}

void EnsureDayNightRuntimeInitialized() {
  if (g_day_night_runtime_initialized) {
    return;
  }
  g_day_night_cycle = ActiveWorld().Definition().day_night_cycle;
  g_day_night_paused = g_day_night_cycle.duration_seconds <= 0.0f;
  g_day_night_manual_hour =
      WrapHour(g_day_night_cycle.start_time_hours);
  g_day_night_elapsed = 0.0f;
  g_day_night_runtime_initialized = true;
}

float ElapsedForHour(
    const skate::world::DayNightCycleDefinition& cycle,
    float hour, float previous_elapsed) {
  if (cycle.duration_seconds <= 0.0f) {
    return 0.0f;
  }
  hour = WrapHour(hour);
  if (!cycle.ping_pong) {
    const float offset =
        WrapHour(hour - cycle.start_time_hours) / 24.0f;
    return offset * cycle.duration_seconds;
  }
  const float range =
      cycle.end_time_hours - cycle.start_time_hours;
  if (std::abs(range) <= 1.0e-5f) {
    return 0.0f;
  }
  const float path =
      std::clamp((hour - cycle.start_time_hours) / range, 0.0f, 1.0f);
  const float forward_offset =
      std::acos(std::clamp(1.0f - 2.0f * path, -1.0f, 1.0f)) /
      (2.0f * 3.14159265358979323846f);
  const float old_offset =
      std::fmod(std::max(previous_elapsed, 0.0f),
                cycle.duration_seconds) /
      cycle.duration_seconds;
  const float offset =
      old_offset > 0.5f ? 1.0f - forward_offset : forward_offset;
  return offset * cycle.duration_seconds;
}

constexpr std::size_t kRainDropCount = 1100;
constexpr std::size_t kLightningSegments = 18;

float Hash01(std::uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return static_cast<float>(value & 0x00ffffffu) /
         static_cast<float>(0x01000000u);
}

float Fract(float value) {
  return value - std::floor(value);
}

VisualVertex WeatherVertex(float x, float y, float z) {
  VisualVertex vertex{};
  vertex.position[0] = x;
  vertex.position[1] = y;
  vertex.position[2] = z;
  vertex.normal[1] = 1.0f;
  return vertex;
}

void EnsureRainTopology() {
  if (!g_rain_visual.indices.empty()) {
    return;
  }
  g_rain_visual.vertices.resize(kRainDropCount * 8);
  g_rain_visual.indices.reserve(kRainDropCount * 12);
  for (std::size_t drop = 0; drop < kRainDropCount; ++drop) {
    const std::uint16_t base =
        static_cast<std::uint16_t>(drop * 8);
    g_rain_visual.indices.insert(
        g_rain_visual.indices.end(),
        {static_cast<std::uint16_t>(base + 0),
         static_cast<std::uint16_t>(base + 2),
         static_cast<std::uint16_t>(base + 1),
         static_cast<std::uint16_t>(base + 1),
         static_cast<std::uint16_t>(base + 2),
         static_cast<std::uint16_t>(base + 3),
         static_cast<std::uint16_t>(base + 4),
         static_cast<std::uint16_t>(base + 6),
         static_cast<std::uint16_t>(base + 5),
         static_cast<std::uint16_t>(base + 5),
         static_cast<std::uint16_t>(base + 6),
         static_cast<std::uint16_t>(base + 7)});
  }
  VisualDraw draw;
  draw.index_count =
      static_cast<std::uint32_t>(g_rain_visual.indices.size());
  draw.color[0] = 0.58f;
  draw.color[1] = 0.74f;
  draw.color[2] = 0.94f;
  draw.color[3] = 0.34f;
  g_rain_visual.draws.push_back(draw);
}

void RebuildLightningVisual() {
  g_lightning_visual = {};
  g_lightning_visual.vertices.reserve(kLightningSegments * 8);
  g_lightning_visual.indices.reserve(kLightningSegments * 12);
  const float strike_x = g_weather_snapshot.lightning_position[0];
  const float strike_z = g_weather_snapshot.lightning_position[2];
  for (std::size_t segment = 0;
       segment < kLightningSegments; ++segment) {
    const float t0 =
        static_cast<float>(segment) / kLightningSegments;
    const float t1 =
        static_cast<float>(segment + 1) / kLightningSegments;
    const float taper0 = std::sin(t0 * 3.14159265f);
    const float taper1 = std::sin(t1 * 3.14159265f);
    const std::uint32_t seed =
        static_cast<std::uint32_t>(
            g_weather_snapshot.strike_count * 131u +
            segment * 17u);
    const std::uint32_t next_seed = seed + 17u;
    const float x0 = strike_x +
        (Hash01(seed) * 2.0f - 1.0f) * 1.6f * taper0;
    const float z0 = strike_z +
        (Hash01(seed + 1u) * 2.0f - 1.0f) * 1.2f * taper0;
    const float x1 = strike_x +
        (Hash01(next_seed) * 2.0f - 1.0f) * 1.6f * taper1;
    const float z1 = strike_z +
        (Hash01(next_seed + 1u) * 2.0f - 1.0f) * 1.2f * taper1;
    const float y0 = 46.0f * (1.0f - t0) + 0.08f * t0;
    const float y1 = 46.0f * (1.0f - t1) + 0.08f * t1;
    const float width = 0.035f + (1.0f - t0) * 0.025f;
    const std::uint16_t base =
        static_cast<std::uint16_t>(
            g_lightning_visual.vertices.size());
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x0 - width, y0, z0));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x0 + width, y0, z0));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x1 - width, y1, z1));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x1 + width, y1, z1));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x0, y0, z0 - width));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x0, y0, z0 + width));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x1, y1, z1 - width));
    g_lightning_visual.vertices.push_back(
        WeatherVertex(x1, y1, z1 + width));
    g_lightning_visual.indices.insert(
        g_lightning_visual.indices.end(),
        {static_cast<std::uint16_t>(base + 0),
         static_cast<std::uint16_t>(base + 2),
         static_cast<std::uint16_t>(base + 1),
         static_cast<std::uint16_t>(base + 1),
         static_cast<std::uint16_t>(base + 2),
         static_cast<std::uint16_t>(base + 3),
         static_cast<std::uint16_t>(base + 4),
         static_cast<std::uint16_t>(base + 6),
         static_cast<std::uint16_t>(base + 5),
         static_cast<std::uint16_t>(base + 5),
         static_cast<std::uint16_t>(base + 6),
         static_cast<std::uint16_t>(base + 7)});
  }
  VisualDraw draw;
  draw.index_count =
      static_cast<std::uint32_t>(g_lightning_visual.indices.size());
  g_lightning_visual.draws.push_back(draw);
}

#if defined(_WIN32)
const std::vector<std::uint8_t>& ThunderWave() {
  static const std::vector<std::uint8_t> wave = [] {
    constexpr std::uint32_t sample_rate = 22050;
    constexpr std::uint32_t sample_count = sample_rate * 4;
    constexpr std::uint32_t data_bytes = sample_count * 2;
    std::vector<std::uint8_t> bytes(44 + data_bytes, 0);
    const auto write16 = [&bytes](std::size_t offset, std::uint16_t value) {
      bytes[offset + 0] = static_cast<std::uint8_t>(value);
      bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto write32 = [&bytes](std::size_t offset, std::uint32_t value) {
      bytes[offset + 0] = static_cast<std::uint8_t>(value);
      bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
      bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
      bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    std::memcpy(bytes.data() + 0, "RIFF", 4);
    write32(4, 36 + data_bytes);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    write32(16, 16);
    write16(20, 1);
    write16(22, 1);
    write32(24, sample_rate);
    write32(28, sample_rate * 2);
    write16(32, 2);
    write16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    write32(40, data_bytes);
    std::uint32_t noise_state = 0x51f15e5du;
    float low_noise = 0.0f;
    for (std::uint32_t index = 0; index < sample_count; ++index) {
      noise_state ^= noise_state << 13;
      noise_state ^= noise_state >> 17;
      noise_state ^= noise_state << 5;
      const float white =
          (static_cast<float>(noise_state & 0xffffu) / 32767.5f) - 1.0f;
      low_noise += (white - low_noise) * 0.018f;
      const float time =
          static_cast<float>(index) / sample_rate;
      const float crack = white * std::exp(-time * 15.0f) * 0.62f;
      const float rumble =
          (low_noise * 2.8f +
           std::sin(time * 2.0f * 3.14159265f * 43.0f) * 0.16f +
           std::sin(time * 2.0f * 3.14159265f * 67.0f) * 0.08f) *
          std::exp(-time * 0.82f);
      const float sample =
          std::clamp(crack + rumble, -1.0f, 1.0f);
      const std::int16_t pcm =
          static_cast<std::int16_t>(sample * 24500.0f);
      write16(44 + index * 2,
              static_cast<std::uint16_t>(pcm));
    }
    return bytes;
  }();
  return wave;
}

void PlayProceduralThunder() {
  const std::vector<std::uint8_t>& wave = ThunderWave();
  PlaySoundA(
      reinterpret_cast<LPCSTR>(wave.data()), nullptr,
      SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
}
#else
void PlayProceduralThunder() {}
#endif

struct WeatherRuntime {
  float elapsed_seconds = 0.0f;
  float strike_started = -100.0f;
  float next_strike = 2.75f;
  bool thunder_played = true;
  std::uint64_t strike_count = 0;
  std::uint64_t thunder_count = 0;

  void Advance(float frame_seconds) {
    const skate::world::WeatherDefinition& weather =
        ActiveWorld().Definition().weather;
    if (!weather.enabled) {
      g_weather_snapshot = {};
      return;
    }
    if (std::isfinite(frame_seconds) && frame_seconds > 0.0f) {
      elapsed_seconds += std::min(frame_seconds, 0.1f);
    }
    if (elapsed_seconds >= next_strike) {
      strike_started = elapsed_seconds;
      ++strike_count;
      thunder_played = false;
      const float position_hash =
          Hash01(static_cast<std::uint32_t>(strike_count * 41u));
      const float depth_hash =
          Hash01(static_cast<std::uint32_t>(strike_count * 67u));
      g_weather_snapshot.lightning_position[0] =
          -30.0f + position_hash * 44.0f;
      g_weather_snapshot.lightning_position[1] = 38.0f;
      g_weather_snapshot.lightning_position[2] =
          -43.0f + depth_hash * 52.0f;
      const float interval_hash =
          Hash01(static_cast<std::uint32_t>(strike_count * 97u));
      next_strike = elapsed_seconds +
          weather.lightning_interval_min +
          (weather.lightning_interval_max -
           weather.lightning_interval_min) * interval_hash;
      RebuildLightningVisual();
    }
    const float age = elapsed_seconds - strike_started;
    float flash = 0.0f;
    if (age >= 0.0f && age < 0.085f) {
      flash = 1.0f - age / 0.085f;
    } else if (age >= 0.13f && age < 0.29f) {
      const float pulse = (age - 0.13f) / 0.16f;
      flash = std::sin(pulse * 3.14159265f) * 0.72f;
    }
    if (!thunder_played && age >= weather.thunder_delay) {
      thunder_played = true;
      ++thunder_count;
      PlayProceduralThunder();
    }
    g_weather_snapshot.elapsed_seconds = elapsed_seconds;
    g_weather_snapshot.rain_intensity = weather.rain_intensity;
    g_weather_snapshot.flash_intensity = flash;
    g_weather_snapshot.wind[0] = weather.wind.x;
    g_weather_snapshot.wind[1] = weather.wind.y;
    g_weather_snapshot.wind[2] = weather.wind.z;
    g_weather_snapshot.strike_count = strike_count;
    g_weather_snapshot.thunder_count = thunder_count;
  }
};

WeatherRuntime& ActiveWeatherRuntime() {
  static WeatherRuntime runtime;
  return runtime;
}

const std::string& ActivePackagePath() {
  static const std::string package_path([] {
    const char* selected_map = std::getenv("SKATE3_OWNED_MAP");
    return selected_map != nullptr && selected_map[0] != '\0'
               ? std::string(selected_map)
               : std::string("owned_maps/blender_bake_showcase.skate");
  }());
  return package_path;
}

const skate::world::WorldMap& ActiveWorld() {
  static const skate::world::WorldMap world([] {
    try {
      const std::string& package_path = ActivePackagePath();
      REXLOG_INFO(
          "mechanics-sandbox: loading owned map package '{}'",
          package_path);
      const auto load_started = std::chrono::steady_clock::now();
      skate::world::MapDefinition imported =
          skate::world::LoadOwnedMapPackage(package_path);
      const auto load_ms = std::chrono::duration_cast<
          std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - load_started);
      REXLOG_INFO(
          "mechanics-sandbox: loaded owned Blender map package '{}' from '{}' "
          "(vertices={} collision={} textures={} rails={} doors={} lights={} "
          "npc_routes={} load_ms={})",
          imported.name, package_path, imported.render_mesh.vertices.size(),
          imported.collision_triangles.size(), imported.textures.size(),
          imported.grind_rails.size(), imported.hinged_doors.size(),
          imported.moving_light_orbs.size(), imported.npc_routes.size(),
          load_ms.count());
      return imported;
    } catch (const std::exception& error) {
      REXLOG_ERROR(
          "mechanics-sandbox: owned Blender map package failed to load: {}; "
          "falling back to handwritten map",
          error.what());
      return skate::world::MakeStarterFlatgroundMap();
    }
  }());
  return world;
}

const skate::world::SurfaceMaterial* FindMaterial(
    const skate::world::MapDefinition& definition,
    skate::world::MaterialId id) {
  const auto found = std::find_if(
      definition.materials.begin(), definition.materials.end(),
      [id](const skate::world::SurfaceMaterial& material) {
        return material.id == id;
      });
  return found == definition.materials.end() ? nullptr : &*found;
}

VisualVertex ConvertVertex(
    const skate::world::RenderVertex& source_vertex) {
  VisualVertex vertex{};
  // Owned geometry shares the retained renderer's legacy vertex layout, but
  // it is never bone-skinned. Reserve the last three blend-index lanes as
  // the static marker so the first can carry the connected-surface
  // presentation rank. This lets the shader reject skinning even if a later
  // draw variant accidentally loses the owned-material constant sentinel.
  // Leaving the marker lanes at zero makes an authored tangent frame look
  // like weights for player bone 0.
  vertex.blend_index[0] = source_vertex.presentation_rank;
  vertex.blend_index[1] = 0xFF;
  vertex.blend_index[2] = 0xFF;
  vertex.blend_index[3] = 0xFF;
  vertex.position[0] = source_vertex.position.x;
  vertex.position[1] = source_vertex.position.y;
  vertex.position[2] = source_vertex.position.z;
  vertex.uv[0] = source_vertex.uv.x;
  vertex.uv[1] = source_vertex.uv.y;
  vertex.uv2[0] = source_vertex.lightmap_uv.x;
  vertex.uv2[1] = source_vertex.lightmap_uv.y;
  if (std::abs(source_vertex.tangent_handedness) > 0.5f) {
    const auto pack_snorm = [](float value) {
      return static_cast<std::uint8_t>(std::lround(
          (std::clamp(value, -1.0f, 1.0f) * 0.5f + 0.5f) *
          255.0f));
    };
    vertex.blend_weight[0] =
        pack_snorm(source_vertex.tangent_binormal.x);
    vertex.blend_weight[1] =
        pack_snorm(source_vertex.tangent_binormal.y);
    vertex.blend_weight[2] =
        pack_snorm(source_vertex.tangent_binormal.z);
    vertex.blend_weight[3] =
        source_vertex.tangent_handedness >= 0.0f ? 200 : 100;
  }
  vertex.normal[0] = source_vertex.normal.x;
  vertex.normal[1] = source_vertex.normal.y;
  vertex.normal[2] = source_vertex.normal.z;
  vertex.uv3[0] = source_vertex.decal_uv.x;
  vertex.uv3[1] = source_vertex.decal_uv.y;
  return vertex;
}

skate::world::TextureId RetailTexture(
    const skate::world::SurfaceMaterial& material,
    const char* semantic) {
  const auto found = std::find_if(
      material.retail.texture_bindings.begin(),
      material.retail.texture_bindings.end(),
      [semantic](const skate::world::RetailTextureBinding& binding) {
        return binding.semantic == semantic;
      });
  return found == material.retail.texture_bindings.end()
             ? 0
             : found->texture;
}

float RetailParameter(
    const skate::world::SurfaceMaterial& material,
    const char* name,
    float fallback) {
  const auto found = std::find_if(
      material.retail.parameters.begin(),
      material.retail.parameters.end(),
      [name](const skate::world::RetailMaterialParameter& parameter) {
        return parameter.name == name;
      });
  if (found == material.retail.parameters.end() ||
      found->values.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const float value = std::strtof(found->values.front().c_str(), &end);
  return end != found->values.front().c_str() &&
                 std::isfinite(value)
             ? value
             : fallback;
}

void PopulateVisualDrawMaterial(
    VisualDraw& draw,
    const skate::world::SurfaceMaterial& material) {
  draw.color[0] = material.display_color.x;
  draw.color[1] = material.display_color.y;
  draw.color[2] = material.display_color.z;
  draw.color[3] = 1.0f;
  draw.material[0] = static_cast<float>(material.pattern);
  draw.material[1] = material.texture_scale;
  draw.material[2] =
      material.emissive_intensity > 0.0f
          ? -material.emissive_intensity
          : material.roughness;
  draw.material[3] = material.variation;
  draw.albedo_texture = material.albedo_texture;
  draw.indirect_lightmap = material.indirect_lightmap;
  draw.normal_texture = material.normal_texture;
  draw.orm_texture = material.orm_texture;
  draw.emissive_texture = material.emissive_texture;
  draw.baked_indirect_strength = material.baked_indirect_strength;
  draw.alpha_mode = material.alpha_mode;
  draw.alpha_cutoff = material.alpha_cutoff;
  draw.presentation_depth_layer =
      material.presentation_depth_layer;
  // Break ties between coplanar materials in the same semantic layer. The
  // stable package material ID is sufficient. Eight bits sharply reduce
  // collisions in large imported worlds while remaining exactly encodable
  // in the float-backed owned-material flag word.
  draw.presentation_depth_order = material.id & 255u;
  if (!material.retail.enabled) {
    return;
  }
  draw.retail_shader_family =
      static_cast<std::uint32_t>(material.retail.shader_family);
  draw.retail_render_flags =
      static_cast<std::uint32_t>(material.retail.render_flags);
  const skate::world::TextureId retail_diffuse =
      RetailTexture(material, "diffuse");
  const skate::world::TextureId retail_transparent =
      RetailTexture(material, "transparent");
  const skate::world::TextureId retail_lightmap =
      RetailTexture(material, "lightmap");
  draw.retail_chromaticity_texture =
      RetailTexture(material, "chromaticity");
  if (draw.albedo_texture == 0) {
    draw.albedo_texture =
        retail_diffuse != 0 ? retail_diffuse : retail_transparent;
  }
  if (draw.indirect_lightmap == 0) {
    draw.indirect_lightmap = retail_lightmap;
  }
  // The dedicated normal slot is the Blender/export policy's authoritative
  // decision. Retail metadata deliberately remains lossless and can still
  // contain pseudo-normal constants such as default_normal
  // 0x0000043d03e3870a, even when extraction excludes them from
  // `normal_texture`. Backfilling that preserved binding here defeated the
  // exclusion and sampled its black/green 16x16 pattern as tangent normals,
  // producing the repeating black bands on University decal materials.
  // A zero dedicated slot therefore means "use the exact shader's flat
  // normal fallback", not "restore the provenance-only retail binding".
  draw.retail_macro_texture = RetailTexture(material, "macrooverlay");
  draw.retail_decal_texture = RetailTexture(material, "decal");
  draw.retail_specular_texture = RetailTexture(material, "specular");
  if (draw.retail_specular_texture == 0) {
    draw.retail_specular_texture = RetailTexture(material, "noise");
  }
  draw.retail_detail_texture = RetailTexture(material, "detail");
  draw.retail_environment_texture =
      RetailTexture(material, "environment");
  draw.retail_normal2_texture = RetailTexture(material, "normal2");
  draw.retail_macro_scale =
      RetailParameter(material, "macroOverlayUVScale", 1.0f);
  draw.retail_macro_opacity =
      RetailParameter(material, "macroOverlayOpacity", 1.0f);
  draw.retail_detail_scale =
      RetailParameter(material, "detailNormalUVScale", 0.0f);
  draw.retail_scroll_u =
      RetailParameter(material, "uAnimationSpeed", 0.0f);
  draw.retail_scroll_v =
      RetailParameter(material, "vAnimationSpeed", 0.0f);
  draw.skate2_lightmap_component =
      RetailParameter(material, "skate2_lightmap_component", -1.0f);
}

VisualWorld BuildVisualWorld() {
  const skate::world::MapDefinition& definition =
      ActiveWorld().Definition();
  const auto build_started = std::chrono::steady_clock::now();
  skate::world::RenderWorldBuildOptions build_options;
  build_options.progress = [](std::size_t completed,
                              std::size_t total) {
    REXLOG_INFO(
        "mechanics-sandbox: visual chunking progress {}/{} ({}%)",
        completed, total,
        total == 0 ? 100 : completed * 100 / total);
  };
  const skate::world::RenderWorld source =
      skate::world::BuildRenderWorld(definition, build_options);
  const auto partition_finished = std::chrono::steady_clock::now();
  VisualWorld world;
  world.chunk_size = source.chunk_size;
  world.source_triangle_count = source.source_triangle_count;
  world.output_triangle_count = source.output_triangle_count;
  world.chunks.reserve(source.chunks.size());
  for (const skate::world::RenderChunk& source_chunk : source.chunks) {
    if (source_chunk.vertices.size() >
        std::numeric_limits<uint16_t>::max()) {
      throw std::runtime_error(
          "owned render chunk exceeds the 16-bit GPU adapter limit");
    }
    VisualChunk chunk;
    chunk.cell_x = source_chunk.cell_x;
    chunk.cell_z = source_chunk.cell_z;
    chunk.part = source_chunk.part;
    chunk.bounds_min[0] = source_chunk.bounds_min.x;
    chunk.bounds_min[1] = source_chunk.bounds_min.y;
    chunk.bounds_min[2] = source_chunk.bounds_min.z;
    chunk.bounds_max[0] = source_chunk.bounds_max.x;
    chunk.bounds_max[1] = source_chunk.bounds_max.y;
    chunk.bounds_max[2] = source_chunk.bounds_max.z;
    chunk.vertices.reserve(source_chunk.vertices.size());
    for (const skate::world::RenderVertex& vertex :
         source_chunk.vertices) {
      chunk.vertices.push_back(ConvertVertex(vertex));
    }
    chunk.indices.reserve(source_chunk.indices.size());
    for (std::uint32_t index : source_chunk.indices) {
      if (index > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error(
            "owned render chunk contains a non-16-bit index");
      }
      chunk.indices.push_back(static_cast<uint16_t>(index));
    }
    for (const skate::world::RenderBatch& source_draw :
         source_chunk.batches) {
      const skate::world::SurfaceMaterial* material =
          FindMaterial(definition, source_draw.material);
      if (material == nullptr) {
        throw std::runtime_error(
            "owned render chunk references an unknown material");
      }
      VisualDraw draw;
      draw.first_index = source_draw.first_index;
      draw.index_count = source_draw.index_count;
      draw.cull_backfaces = source_draw.cull_backfaces;
      PopulateVisualDrawMaterial(draw, *material);
      chunk.draws.push_back(draw);
    }
    world.chunks.push_back(std::move(chunk));
  }
  for (std::size_t chunk_index = 0;
       chunk_index < world.chunks.size(); ++chunk_index) {
    const VisualChunk& chunk = world.chunks[chunk_index];
    if (world.cells.empty() ||
        world.cells.back().cell_x != chunk.cell_x ||
        world.cells.back().cell_z != chunk.cell_z) {
      world.cells.push_back(
          {chunk.cell_x, chunk.cell_z, chunk_index, 1});
    } else {
      ++world.cells.back().chunk_count;
    }
  }
  const auto build_finished = std::chrono::steady_clock::now();
  REXLOG_INFO(
      "mechanics-sandbox: visual world ready chunks={} cells={} "
      "source_triangles={} output_triangles={} culled_materials={} "
      "presentation_surfaces={} partition_ms={} adapter_ms={} total_ms={}",
      world.chunks.size(), world.cells.size(),
      world.source_triangle_count, world.output_triangle_count,
      source.backface_culled_material_count,
      source.presentation_surface_count,
      std::chrono::duration_cast<std::chrono::milliseconds>(
          partition_finished - build_started).count(),
      std::chrono::duration_cast<std::chrono::milliseconds>(
          build_finished - partition_finished).count(),
      std::chrono::duration_cast<std::chrono::milliseconds>(
          build_finished - build_started).count());
  return world;
}

VisualMesh BuildSkyMesh() {
  const skate::world::SkyDefinition& sky =
      ActiveWorld().Definition().sky;
  VisualMesh mesh;
  if (!sky.enabled) {
    return mesh;
  }
  constexpr int kSegments = 48;
  constexpr int kBands = 16;
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kRadius = 150.0f;
  VisualDraw draw;
  draw.first_index = 0;
  for (int band = 0; band < kBands; ++band) {
    const float latitude0 =
        -kPi * 0.5f + kPi * static_cast<float>(band) / kBands;
    const float latitude1 =
        -kPi * 0.5f + kPi * static_cast<float>(band + 1) / kBands;
    for (int segment = 0; segment < kSegments; ++segment) {
      const float longitude0 =
          2.0f * kPi * static_cast<float>(segment) / kSegments;
      const float longitude1 =
          2.0f * kPi * static_cast<float>(segment + 1) / kSegments;
      const auto append = [&mesh](float latitude, float longitude) {
        const float horizontal = std::cos(latitude);
        const float x = horizontal * std::cos(longitude);
        const float y = std::sin(latitude);
        const float z = horizontal * std::sin(longitude);
        skate::world::RenderVertex source;
        source.position = {x * kRadius, y * kRadius, z * kRadius};
        source.normal = {-x, -y, -z};
        source.uv = {
            longitude / (2.0f * kPi),
            latitude / kPi + 0.5f,
        };
        mesh.vertices.push_back(ConvertVertex(source));
        return static_cast<uint16_t>(mesh.vertices.size() - 1);
      };
      const uint16_t a = append(latitude0, longitude0);
      const uint16_t b = append(latitude0, longitude1);
      const uint16_t c = append(latitude1, longitude1);
      const uint16_t d = append(latitude1, longitude0);
      // Winding faces the camera at the centre of the sphere.
      mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
    }
  }
  // Sky colour is evaluated continuously per pixel from the map-owned
  // zenith/horizon/nadir palette. One draw avoids visible latitude bands.
  draw.index_count = static_cast<uint32_t>(mesh.indices.size());
  draw.color[3] = 1.0f;
  mesh.draws.push_back(draw);
  return mesh;
}

VisualMesh BuildMovingLightVisualMesh() {
  VisualMesh mesh;
  constexpr int kLongitudeSegments = 20;
  constexpr int kLatitudeSegments = 12;
  constexpr float kPi = 3.14159265358979323846f;
  for (int latitude = 0; latitude <= kLatitudeSegments;
       ++latitude) {
    const float v =
        static_cast<float>(latitude) / kLatitudeSegments;
    const float phi = -kPi * 0.5f + v * kPi;
    const float ring = std::cos(phi);
    const float y = std::sin(phi);
    for (int longitude = 0;
         longitude <= kLongitudeSegments; ++longitude) {
      const float u =
          static_cast<float>(longitude) / kLongitudeSegments;
      const float theta = u * 2.0f * kPi;
      const float x = ring * std::cos(theta);
      const float z = ring * std::sin(theta);
      skate::world::RenderVertex source;
      source.position = {x, y, z};
      source.normal = {x, y, z};
      source.uv = {u, v};
      mesh.vertices.push_back(ConvertVertex(source));
    }
  }
  for (int latitude = 0; latitude < kLatitudeSegments;
       ++latitude) {
    for (int longitude = 0;
         longitude < kLongitudeSegments; ++longitude) {
      const std::uint16_t a = static_cast<std::uint16_t>(
          latitude * (kLongitudeSegments + 1) + longitude);
      const std::uint16_t b =
          static_cast<std::uint16_t>(a + kLongitudeSegments + 1);
      const std::uint16_t c = static_cast<std::uint16_t>(b + 1);
      const std::uint16_t d = static_cast<std::uint16_t>(a + 1);
      mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
    }
  }
  VisualDraw draw;
  draw.index_count =
      static_cast<std::uint32_t>(mesh.indices.size());
  mesh.draws.push_back(draw);
  return mesh;
}

VisualMesh BuildRemoteSkaterVisualMesh() {
  // This intentionally simple rigid proxy proves the multiplayer transport,
  // map-space convention, smoothing, and owned renderer integration before
  // skeletal animation is replicated. Its origin is the board transform.
  skate::world::MapBuilder builder("remote_skater_visual");
  const skate::world::MaterialId material = builder.AddMaterial(
      "remote_player", 0.8f, 0.0f, skate::world::SurfaceFlags::None,
      {0.08f, 0.78f, 1.0f}, skate::world::MaterialPattern::Painted,
      0.35f, 0.24f, 0.04f, 0.0f);
  constexpr skate::world::SurfaceId surface = 1;
  builder.AddBox(surface, material, {-0.43f, -0.045f, -0.12f},
                 {0.43f, 0.015f, 0.12f});
  builder.AddBox(surface, material, {-0.20f, 0.02f, -0.10f},
                 {-0.05f, 0.62f, 0.10f});
  builder.AddBox(surface, material, {0.05f, 0.02f, -0.10f},
                 {0.20f, 0.62f, 0.10f});
  builder.AddBox(surface, material, {-0.25f, 0.58f, -0.13f},
                 {0.25f, 1.25f, 0.13f});
  builder.AddBox(surface, material, {-0.40f, 0.62f, -0.09f},
                 {-0.24f, 1.18f, 0.09f});
  builder.AddBox(surface, material, {0.24f, 0.62f, -0.09f},
                 {0.40f, 1.18f, 0.09f});
  builder.AddBox(surface, material, {-0.15f, 1.25f, -0.13f},
                 {0.15f, 1.55f, 0.13f});
  const skate::world::MapDefinition local =
      std::move(builder).Build();

  VisualMesh mesh;
  mesh.vertices.reserve(local.render_mesh.vertices.size());
  for (const skate::world::RenderVertex& vertex :
       local.render_mesh.vertices) {
    mesh.vertices.push_back(ConvertVertex(vertex));
  }
  mesh.indices.reserve(local.render_mesh.indices.size());
  for (std::uint32_t index : local.render_mesh.indices) {
    if (index > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error(
          "remote skater visual exceeds the 16-bit GPU limit");
    }
    mesh.indices.push_back(static_cast<std::uint16_t>(index));
  }
  VisualDraw draw;
  draw.index_count = static_cast<std::uint32_t>(mesh.indices.size());
  draw.color[0] = 0.08f;
  draw.color[1] = 0.78f;
  draw.color[2] = 1.0f;
  draw.color[3] = 1.0f;
  draw.material[0] =
      static_cast<float>(skate::world::MaterialPattern::Painted);
  draw.material[1] = 0.35f;
  draw.material[2] = 0.24f;
  draw.material[3] = 0.04f;
  mesh.draws.push_back(draw);
  return mesh;
}

std::vector<VisualMesh> BuildKinematicVisualMeshes() {
  const skate::world::MapDefinition& definition =
      ActiveWorld().Definition();
  std::vector<VisualMesh> meshes;
  meshes.reserve(definition.kinematic_boxes.size());
  for (const skate::world::KinematicBox& object :
       definition.kinematic_boxes) {
    const skate::world::SurfaceMaterial* material =
        FindMaterial(definition, object.material);
    if (material == nullptr) {
      throw std::runtime_error(
          "owned kinematic object references an unknown material");
    }

    skate::world::MapBuilder builder(object.name + "_visual");
    const skate::world::MaterialId local_material =
        builder.AddMaterial(
            material->name, material->friction, material->restitution,
            material->flags, material->display_color, material->pattern,
            material->texture_scale, material->roughness,
            material->variation, material->emissive_intensity);
    builder.AddBox(object.surface, local_material,
                   object.local_min, object.local_max);
    const skate::world::MapDefinition local =
        std::move(builder).Build();

    VisualMesh mesh;
    if (local.render_mesh.vertices.size() >
        std::numeric_limits<uint16_t>::max()) {
      throw std::runtime_error(
          "owned kinematic visual exceeds the 16-bit GPU limit");
    }
    mesh.vertices.reserve(local.render_mesh.vertices.size());
    for (const skate::world::RenderVertex& vertex :
         local.render_mesh.vertices) {
      mesh.vertices.push_back(ConvertVertex(vertex));
    }
    mesh.indices.reserve(local.render_mesh.indices.size());
    for (std::uint32_t index : local.render_mesh.indices) {
      if (index > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error(
            "owned kinematic visual contains a non-16-bit index");
      }
      mesh.indices.push_back(static_cast<uint16_t>(index));
    }
    VisualDraw draw;
    draw.index_count = static_cast<uint32_t>(mesh.indices.size());
    PopulateVisualDrawMaterial(draw, *material);
    mesh.draws.push_back(draw);
    meshes.push_back(std::move(mesh));
  }
  return meshes;
}

std::vector<VisualMesh> BuildHingedDoorVisualMeshes() {
  const skate::world::MapDefinition& definition =
      ActiveWorld().Definition();
  std::vector<VisualMesh> meshes;
  meshes.reserve(definition.hinged_doors.size());
  for (const skate::world::HingedDoor& door :
       definition.hinged_doors) {
    if (door.render_mesh.vertices.size() >
        std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error(
          "owned hinged-door visual exceeds the 16-bit GPU limit");
    }
    VisualMesh mesh;
    mesh.vertices.reserve(door.render_mesh.vertices.size());
    for (const skate::world::RenderVertex& vertex :
         door.render_mesh.vertices) {
      mesh.vertices.push_back(ConvertVertex(vertex));
    }

    std::map<skate::world::MaterialId,
             std::vector<std::uint16_t>> grouped_indices;
    for (std::size_t first = 0;
         first < door.render_mesh.indices.size(); first += 3) {
      const std::uint32_t first_vertex =
          door.render_mesh.indices[first];
      if (first_vertex >= door.render_mesh.vertices.size()) {
        throw std::runtime_error(
            "owned hinged-door visual index is out of range");
      }
      const skate::world::MaterialId material =
          door.render_mesh.vertices[first_vertex].material;
      auto& indices = grouped_indices[material];
      for (std::size_t corner = 0; corner < 3; ++corner) {
        const std::uint32_t index =
            door.render_mesh.indices[first + corner];
        if (index > std::numeric_limits<std::uint16_t>::max()) {
          throw std::runtime_error(
              "owned hinged-door visual contains a non-16-bit index");
        }
        indices.push_back(static_cast<std::uint16_t>(index));
      }
    }
    for (const auto& [material_id, indices] : grouped_indices) {
      const skate::world::SurfaceMaterial* material =
          FindMaterial(definition, material_id);
      if (material == nullptr) {
        throw std::runtime_error(
            "owned hinged-door visual references an unknown material");
      }
      VisualDraw draw;
      draw.first_index =
          static_cast<std::uint32_t>(mesh.indices.size());
      draw.index_count = static_cast<std::uint32_t>(indices.size());
      PopulateVisualDrawMaterial(draw, *material);
      mesh.indices.insert(
          mesh.indices.end(), indices.begin(), indices.end());
      mesh.draws.push_back(draw);
    }
    meshes.push_back(std::move(mesh));
  }
  return meshes;
}

VisualMesh BuildWaterPusherVisualMesh(
    const skate::world::MapDefinition& definition,
    const skate::world::WaterBasin& basin) {
  const skate::world::SurfaceMaterial* material =
      FindMaterial(definition, basin.pusher_material);
  if (material == nullptr) {
    throw std::runtime_error(
        "owned water pusher references an unknown material");
  }

  skate::world::MapBuilder builder(basin.name + "_pusher_visual");
  const skate::world::MaterialId local_material =
      builder.AddMaterial(
          material->name, material->friction, material->restitution,
          material->flags, material->display_color, material->pattern,
          material->texture_scale, material->roughness,
          material->variation, material->emissive_intensity);
  builder.AddBox(1, local_material, basin.pusher_local_min,
                 basin.pusher_local_max);
  const skate::world::MapDefinition local =
      std::move(builder).Build();

  VisualMesh mesh;
  mesh.vertices.reserve(local.render_mesh.vertices.size());
  for (const skate::world::RenderVertex& vertex :
       local.render_mesh.vertices) {
    mesh.vertices.push_back(ConvertVertex(vertex));
  }
  mesh.indices.reserve(local.render_mesh.indices.size());
  for (std::uint32_t index : local.render_mesh.indices) {
    if (index > std::numeric_limits<uint16_t>::max()) {
      throw std::runtime_error(
          "owned water pusher exceeds the 16-bit GPU limit");
    }
    mesh.indices.push_back(static_cast<uint16_t>(index));
  }
  VisualDraw draw;
  draw.index_count = static_cast<uint32_t>(mesh.indices.size());
  PopulateVisualDrawMaterial(draw, *material);
  mesh.draws.push_back(draw);
  return mesh;
}

struct WaterRuntime {
  const skate::world::WaterBasin* basin = nullptr;
  std::unique_ptr<skate::world::ShallowWaterSimulation> simulation;
  VisualMesh surface;
  VisualMesh pusher;
  skate::world::KinematicBox pusher_motion;
  skate::world::KinematicPose pusher_pose;
  float elapsed_seconds = 0.0f;
  float accumulator = 0.0f;
  uint64_t simulation_steps = 0;
  uint64_t dropped_frames = 0;

  WaterRuntime() {
    const skate::world::MapDefinition& definition =
        ActiveWorld().Definition();
    if (definition.water_basins.empty()) {
      return;
    }
    basin = &definition.water_basins.front();
    skate::world::ShallowWaterConfig config;
    config.minimum = {basin->minimum.x, basin->minimum.z};
    config.maximum = {basin->maximum.x, basin->maximum.z};
    config.columns = basin->columns;
    config.rows = basin->rows;
    config.rest_surface_height = basin->rest_surface_height;
    config.rest_depth =
        basin->rest_surface_height - basin->minimum.y;
    config.linear_damping = basin->damping;
    simulation =
        std::make_unique<skate::world::ShallowWaterSimulation>(config);

    pusher_motion.name = basin->name + "_pusher";
    pusher_motion.local_min = basin->pusher_local_min;
    pusher_motion.local_max = basin->pusher_local_max;
    pusher_motion.path_start = basin->pusher_path_start;
    pusher_motion.path_end = basin->pusher_path_end;
    pusher_motion.travel_seconds = basin->pusher_travel_seconds;
    pusher_pose =
        skate::world::EvaluateKinematicBox(pusher_motion, 0.0f);
    pusher = BuildWaterPusherVisualMesh(definition, *basin);

    const std::size_t vertex_count =
        static_cast<std::size_t>(config.columns) * config.rows;
    surface.vertices.resize(vertex_count);
    surface.indices.reserve(
        static_cast<std::size_t>(config.columns - 1) *
        (config.rows - 1) * 6);
    for (std::uint32_t row = 0; row + 1 < config.rows; ++row) {
      for (std::uint32_t column = 0;
           column + 1 < config.columns; ++column) {
        const std::uint32_t a = row * config.columns + column;
        const std::uint32_t b = a + 1;
        const std::uint32_t d = a + config.columns;
        const std::uint32_t c = d + 1;
        surface.indices.insert(
            surface.indices.end(),
            {static_cast<uint16_t>(a), static_cast<uint16_t>(d),
             static_cast<uint16_t>(c), static_cast<uint16_t>(a),
             static_cast<uint16_t>(c), static_cast<uint16_t>(b)});
      }
    }
    VisualDraw draw;
    draw.index_count =
        static_cast<uint32_t>(surface.indices.size());
    draw.color[0] = 0.025f;
    draw.color[1] = 0.24f;
    draw.color[2] = 0.34f;
    draw.color[3] = 1.0f;
    surface.draws.push_back(draw);

    // Establish the initial solid mask without adding a visible impulse.
    skate::world::WaterObstacle obstacle;
    obstacle.center = {
        pusher_pose.position.x, pusher_pose.position.z};
    obstacle.half_extents = {
        (basin->pusher_local_max.x -
         basin->pusher_local_min.x) *
            0.5f,
        (basin->pusher_local_max.z -
         basin->pusher_local_min.z) *
            0.5f,
    };
    simulation->Step(1.0f / 240.0f, obstacle);
    UpdateSurface(1.0f);
  }

  bool Advance(float frame_seconds) {
    if (!simulation || !std::isfinite(frame_seconds) ||
        frame_seconds < 0.0f) {
      return false;
    }
    if (frame_seconds > 0.20f) {
      frame_seconds = 0.20f;
      ++dropped_frames;
    }
    accumulator =
        std::min(accumulator + frame_seconds, 0.25f);
    constexpr float step_seconds = 1.0f / 240.0f;
    std::uint32_t substeps = 0;
    while (accumulator >= step_seconds && substeps < 60) {
      elapsed_seconds += step_seconds;
      pusher_pose = skate::world::EvaluateKinematicBox(
          pusher_motion, elapsed_seconds);
      skate::world::WaterObstacle obstacle;
      obstacle.center = {
          pusher_pose.position.x, pusher_pose.position.z};
      obstacle.half_extents = {
          (basin->pusher_local_max.x -
           basin->pusher_local_min.x) *
              0.5f,
          (basin->pusher_local_max.z -
           basin->pusher_local_min.z) *
              0.5f,
      };
      obstacle.velocity = {
          pusher_pose.velocity.x, pusher_pose.velocity.z};
      if (!simulation->Step(step_seconds, obstacle)) {
        return false;
      }
      accumulator -= step_seconds;
      ++simulation_steps;
      ++substeps;
    }
    const float interpolation_alpha =
        std::clamp(accumulator / step_seconds, 0.0f, 1.0f);
    // Present both the analytic pusher and water at the same interpolated
    // fixed-step time. This adds one simulation step of presentation latency
    // (4.17 ms) while removing state and lighting-normal stair-stepping.
    const float presentation_seconds = std::max(
        0.0f, elapsed_seconds - step_seconds + accumulator);
    pusher_pose = skate::world::EvaluateKinematicBox(
        pusher_motion, presentation_seconds);
    UpdateSurface(interpolation_alpha);
    const skate::world::WaterStatistics statistics =
        simulation->Statistics();
    g_water_simulation_steps.store(
        simulation_steps, std::memory_order_release);
    g_water_dropped_frames.store(
        dropped_frames, std::memory_order_release);
    g_water_minimum_bits.store(
        std::bit_cast<uint32_t>(statistics.minimum_displacement),
        std::memory_order_release);
    g_water_maximum_bits.store(
        std::bit_cast<uint32_t>(statistics.maximum_displacement),
        std::memory_order_release);
    g_water_mean_bits.store(
        std::bit_cast<uint32_t>(statistics.mean_displacement),
        std::memory_order_release);
    g_water_energy_bits.store(
        std::bit_cast<uint32_t>(statistics.kinetic_energy),
        std::memory_order_release);
    return true;
  }

  void UpdateSurface(float interpolation_alpha) {
    if (!simulation) {
      return;
    }
    for (std::uint32_t row = 0;
         row < simulation->Rows(); ++row) {
      for (std::uint32_t column = 0;
           column < simulation->Columns(); ++column) {
        const std::size_t index =
            static_cast<std::size_t>(row) *
                simulation->Columns() +
            column;
        const skate::world::Vec2 position =
            simulation->SamplePosition(column, row);
        const skate::world::Vec3 normal =
            simulation->InterpolatedSurfaceNormal(
                column, row, interpolation_alpha);
        VisualVertex vertex{};
        vertex.position[0] = position.x;
        vertex.position[1] =
            simulation->InterpolatedSurfaceHeight(
                column, row, interpolation_alpha);
        vertex.position[2] = position.y;
        vertex.normal[0] = normal.x;
        vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] =
            static_cast<float>(column) /
            static_cast<float>(simulation->Columns() - 1);
        vertex.uv[1] =
            static_cast<float>(row) /
            static_cast<float>(simulation->Rows() - 1);
        vertex.uv2[0] = vertex.uv[0];
        vertex.uv2[1] = vertex.uv[1];
        vertex.uv3[0] = vertex.uv[0];
        vertex.uv3[1] = vertex.uv[1];
        surface.vertices[index] = vertex;
      }
    }
  }
};

WaterRuntime& ActiveWaterRuntime() {
  static WaterRuntime runtime;
  return runtime;
}

std::set<skate::world::SurfaceId> CollectSurfaces(bool ramps_only) {
  std::set<skate::world::SurfaceId> surfaces;
  for (const skate::world::CollisionTriangle& triangle :
       ActiveWorld().Definition().collision_triangles) {
    if (ramps_only) {
      const float up = std::abs(triangle.normal.y);
      if (up <= 0.1f || up >= 0.999f) {
        continue;
      }
    }
    surfaces.insert(triangle.surface);
  }
  return surfaces;
}

}  // namespace

const VisualWorld& ActiveVisualWorld() {
  static const VisualWorld world = BuildVisualWorld();
  return world;
}

const VisualMesh& ActiveSkyMesh() {
  static const VisualMesh sky = BuildSkyMesh();
  return sky;
}

const VisualMesh& ActiveKinematicVisualMesh(std::size_t index) {
  static const std::vector<VisualMesh> meshes =
      BuildKinematicVisualMeshes();
  if (index >= meshes.size()) {
    throw std::out_of_range(
        "owned kinematic visual index is out of range");
  }
  return meshes[index];
}

const VisualMesh& ActiveHingedDoorVisualMesh(std::size_t index) {
  static const std::vector<VisualMesh> meshes =
      BuildHingedDoorVisualMeshes();
  if (index >= meshes.size()) {
    throw std::out_of_range(
        "owned hinged-door visual index is out of range");
  }
  return meshes[index];
}

const VisualMesh& ActiveWaterVisualMesh() {
  return ActiveWaterRuntime().surface;
}

const VisualMesh& ActiveWaterPusherVisualMesh() {
  return ActiveWaterRuntime().pusher;
}

const VisualMesh& ActiveMovingLightVisualMesh() {
  static const VisualMesh mesh = BuildMovingLightVisualMesh();
  return mesh;
}

const VisualMesh& ActiveRemoteSkaterVisualMesh() {
  static const VisualMesh mesh = BuildRemoteSkaterVisualMesh();
  return mesh;
}

const VisualMesh& ActiveRainVisualMesh() {
  EnsureRainTopology();
  return g_rain_visual;
}

const VisualMesh& ActiveLightningVisualMesh() {
  return g_lightning_visual;
}

const skate::world::MapDefinition& ActiveDefinition() {
  return ActiveWorld().Definition();
}

const skate::world::ImageTexture* ActiveImageTexture(
    skate::world::TextureId id) {
  const auto& textures = ActiveWorld().Definition().textures;
  const auto found = std::find_if(
      textures.begin(), textures.end(),
      [id](const skate::world::ImageTexture& texture) {
        return texture.id == id;
      });
  return found == textures.end() ? nullptr : &*found;
}

const char* ActiveMapName() {
  return ActiveWorld().Definition().name.c_str();
}

const char* ActiveMapPackagePath() {
  return ActivePackagePath().c_str();
}

std::size_t ActiveSurfaceCount() {
  static const std::size_t count = CollectSurfaces(false).size();
  return count;
}

std::size_t ActiveRampCount() {
  static const std::size_t count = CollectSurfaces(true).size();
  return count;
}

std::size_t ActiveKinematicObjectCount() {
  return ActiveWorld().Definition().kinematic_boxes.size();
}

std::size_t ActiveHingedDoorCount() {
  return ActiveWorld().Definition().hinged_doors.size();
}

std::size_t ActiveWaterBasinCount() {
  return ActiveWorld().Definition().water_basins.size();
}

std::size_t ActiveRaytracedMirrorCount() {
  return ActiveWorld().Definition().raytraced_mirrors.size();
}

std::size_t ActiveRaytracedPuddleCount() {
  return ActiveWorld().Definition().raytraced_puddles.size();
}

std::size_t ActiveMovingLightCount() {
  return ActiveWorld().Definition().moving_light_orbs.size();
}

void AdvanceMovingLights(float frame_seconds) {
  if (std::isfinite(frame_seconds) && frame_seconds > 0.0f) {
    g_moving_light_time =
        std::fmod(g_moving_light_time +
                      std::min(frame_seconds, 0.1f),
                  3600.0f);
  }
  const auto& lights =
      ActiveWorld().Definition().moving_light_orbs;
  g_moving_light_snapshots.resize(lights.size());
  for (std::size_t index = 0; index < lights.size(); ++index) {
    const skate::world::MovingLightOrb& light = lights[index];
    const skate::world::MovingLightOrbPose pose =
        skate::world::EvaluateMovingLightOrb(
            light, g_moving_light_time);
    MovingLightSnapshot& snapshot =
        g_moving_light_snapshots[index];
    snapshot.position[0] = pose.position.x;
    snapshot.position[1] = pose.position.y;
    snapshot.position[2] = pose.position.z;
    snapshot.color[0] = light.color.x;
    snapshot.color[1] = light.color.y;
    snapshot.color[2] = light.color.z;
    snapshot.direction[0] = light.direction.x;
    snapshot.direction[1] = light.direction.y;
    snapshot.direction[2] = light.direction.z;
    snapshot.source_radius = light.source_radius;
    snapshot.influence_radius = light.influence_radius;
    snapshot.intensity = light.intensity;
    snapshot.spot_inner_cosine = light.spot_inner_cosine;
    snapshot.spot_outer_cosine = light.spot_outer_cosine;
    snapshot.type = static_cast<std::uint32_t>(light.type);
    snapshot.visible_source = light.visible_source;
  }
}

bool ActiveMovingLightSnapshot(
    std::size_t index, MovingLightSnapshot& out) {
  if (g_moving_light_snapshots.size() !=
      ActiveMovingLightCount()) {
    AdvanceMovingLights(0.0f);
  }
  if (index >= g_moving_light_snapshots.size()) {
    return false;
  }
  out = g_moving_light_snapshots[index];
  return true;
}

void AdvanceDayNightCycle(float frame_seconds) {
  std::scoped_lock lock(g_day_night_mutex);
  EnsureDayNightRuntimeInitialized();
  if (std::isfinite(frame_seconds) && frame_seconds > 0.0f) {
    if (!g_day_night_paused &&
        g_day_night_cycle.duration_seconds > 0.0f) {
      g_day_night_elapsed += std::min(frame_seconds, 0.1f);
      if (g_day_night_elapsed > 86400.0f) {
        g_day_night_elapsed =
            std::fmod(g_day_night_elapsed,
                      g_day_night_cycle.duration_seconds);
      }
    }
  }
  if (g_day_night_paused ||
      g_day_night_cycle.duration_seconds <= 0.0f) {
    skate::world::DayNightCycleDefinition frozen = g_day_night_cycle;
    frozen.duration_seconds = 0.0f;
    frozen.start_time_hours = g_day_night_manual_hour;
    g_day_night_state =
        skate::world::EvaluateDayNightCycle(frozen, 0.0f);
  } else {
    g_day_night_state =
        skate::world::EvaluateDayNightCycle(
            g_day_night_cycle, g_day_night_elapsed);
    g_day_night_manual_hour =
        g_day_night_state.time_of_day_hours;
  }
  g_day_night_initialized = true;
}

skate::world::DayNightState ActiveDayNightState() {
  std::scoped_lock lock(g_day_night_mutex);
  EnsureDayNightRuntimeInitialized();
  if (!g_day_night_initialized) {
    if (g_day_night_paused ||
        g_day_night_cycle.duration_seconds <= 0.0f) {
      skate::world::DayNightCycleDefinition frozen =
          g_day_night_cycle;
      frozen.duration_seconds = 0.0f;
      frozen.start_time_hours = g_day_night_manual_hour;
      g_day_night_state =
          skate::world::EvaluateDayNightCycle(frozen, 0.0f);
    } else {
      g_day_night_state =
          skate::world::EvaluateDayNightCycle(
              g_day_night_cycle, g_day_night_elapsed);
      g_day_night_manual_hour =
          g_day_night_state.time_of_day_hours;
    }
    g_day_night_initialized = true;
  }
  return g_day_night_state;
}

bool DynamicWorldLightingEnabled() {
  std::scoped_lock lock(g_day_night_mutex);
  EnsureDayNightRuntimeInitialized();
  return g_dynamic_lighting_enabled;
}

WorldLightingSettings ActiveWorldLightingSettings() {
  std::scoped_lock lock(g_day_night_mutex);
  EnsureDayNightRuntimeInitialized();
  WorldLightingSettings settings;
  settings.available = g_day_night_cycle.enabled;
  settings.paused = g_day_night_paused;
  settings.ping_pong = g_day_night_cycle.ping_pong;
  settings.dynamic_lighting_enabled = g_dynamic_lighting_enabled;
  settings.time_of_day_hours = g_day_night_manual_hour;
  settings.cycle_duration_seconds =
      g_day_night_cycle.duration_seconds;
  settings.start_hour = g_day_night_cycle.start_time_hours;
  settings.end_hour = g_day_night_cycle.end_time_hours;
  settings.orbit_azimuth_degrees =
      g_day_night_cycle.orbit_azimuth_radians *
      kRadiansToDegrees;
  settings.sky_red = g_day_night_cycle.sky_tint.x;
  settings.sky_green = g_day_night_cycle.sky_tint.y;
  settings.sky_blue = g_day_night_cycle.sky_tint.z;
  settings.sunlight_red = g_day_night_cycle.sun_color.x;
  settings.sunlight_green = g_day_night_cycle.sun_color.y;
  settings.sunlight_blue = g_day_night_cycle.sun_color.z;
  settings.sun_intensity = g_day_night_cycle.sun_intensity;
  settings.moon_intensity = g_day_night_cycle.moon_intensity;
  settings.day_ambient = g_day_night_cycle.day_ambient;
  settings.night_ambient = g_day_night_cycle.night_ambient;
  return settings;
}

void SetWorldLightingSetting(WorldLightingSetting setting, float value) {
  if (!std::isfinite(value)) {
    return;
  }
  std::scoped_lock lock(g_day_night_mutex);
  EnsureDayNightRuntimeInitialized();
  const float current_hour = g_day_night_manual_hour;
  switch (setting) {
    case WorldLightingSetting::kPaused:
      g_day_night_paused = value >= 0.5f;
      break;
    case WorldLightingSetting::kTimeOfDay:
      g_day_night_manual_hour = WrapHour(value);
      g_day_night_elapsed =
          ElapsedForHour(g_day_night_cycle,
                         g_day_night_manual_hour,
                         g_day_night_elapsed);
      break;
    case WorldLightingSetting::kCycleDuration:
      g_day_night_cycle.duration_seconds =
          std::clamp(value, 0.0f, 1800.0f);
      g_day_night_elapsed =
          ElapsedForHour(g_day_night_cycle, current_hour,
                         g_day_night_elapsed);
      break;
    case WorldLightingSetting::kPingPong:
      g_day_night_cycle.ping_pong = value >= 0.5f;
      g_day_night_elapsed =
          ElapsedForHour(g_day_night_cycle, current_hour,
                         g_day_night_elapsed);
      break;
    case WorldLightingSetting::kStartHour:
      g_day_night_cycle.start_time_hours = WrapHour(value);
      g_day_night_elapsed =
          ElapsedForHour(g_day_night_cycle, current_hour,
                         g_day_night_elapsed);
      break;
    case WorldLightingSetting::kEndHour:
      g_day_night_cycle.end_time_hours = WrapHour(value);
      g_day_night_elapsed =
          ElapsedForHour(g_day_night_cycle, current_hour,
                         g_day_night_elapsed);
      break;
    case WorldLightingSetting::kOrbitAzimuthDegrees:
      g_day_night_cycle.orbit_azimuth_radians =
          std::clamp(value, -180.0f, 180.0f) *
          kDegreesToRadians;
      break;
    case WorldLightingSetting::kSkyRed:
      g_day_night_cycle.sky_tint.x =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kSkyGreen:
      g_day_night_cycle.sky_tint.y =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kSkyBlue:
      g_day_night_cycle.sky_tint.z =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kSunlightRed:
      g_day_night_cycle.sun_color.x =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kSunlightGreen:
      g_day_night_cycle.sun_color.y =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kSunlightBlue:
      g_day_night_cycle.sun_color.z =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kSunIntensity:
      g_day_night_cycle.sun_intensity =
          std::clamp(value, 0.0f, 4.0f);
      break;
    case WorldLightingSetting::kMoonIntensity:
      g_day_night_cycle.moon_intensity =
          std::clamp(value, 0.0f, 2.0f);
      break;
    case WorldLightingSetting::kDayAmbient:
      g_day_night_cycle.day_ambient =
          std::clamp(value, 0.0f, 1.0f);
      break;
    case WorldLightingSetting::kNightAmbient:
      g_day_night_cycle.night_ambient =
          std::clamp(value, 0.0f, 1.0f);
      break;
    case WorldLightingSetting::kDynamicLightingEnabled:
      g_dynamic_lighting_enabled = value >= 0.5f;
      break;
  }
  g_day_night_initialized = false;
}

void ResetWorldLightingSettings() {
  std::scoped_lock lock(g_day_night_mutex);
  g_day_night_cycle =
      ActiveWorld().Definition().day_night_cycle;
  g_day_night_paused =
      g_day_night_cycle.duration_seconds <= 0.0f;
  g_dynamic_lighting_enabled = true;
  g_day_night_manual_hour =
      WrapHour(g_day_night_cycle.start_time_hours);
  g_day_night_elapsed = 0.0f;
  g_day_night_runtime_initialized = true;
  g_day_night_initialized = false;
}

void AdvanceWeather(float frame_seconds) {
  ActiveWeatherRuntime().Advance(frame_seconds);
}

void UpdateRainVisualMesh(const float local_camera_position[3]) {
  if (local_camera_position == nullptr) {
    return;
  }
  EnsureRainTopology();
  const skate::world::WeatherDefinition& weather =
      ActiveWorld().Definition().weather;
  if (!weather.enabled || weather.rain_intensity <= 0.0f) {
    return;
  }
  const float height = 27.0f;
  const float time = g_weather_snapshot.elapsed_seconds;
  const float velocity_x = weather.wind.x;
  const float velocity_y = -weather.rain_fall_speed;
  const float velocity_z = weather.wind.z;
  const float velocity_length = std::sqrt(
      velocity_x * velocity_x + velocity_y * velocity_y +
      velocity_z * velocity_z);
  const float direction_x = velocity_x / velocity_length;
  const float direction_y = velocity_y / velocity_length;
  const float direction_z = velocity_z / velocity_length;
  for (std::size_t drop = 0; drop < kRainDropCount; ++drop) {
    const std::uint32_t seed =
        static_cast<std::uint32_t>(drop * 11u + 19u);
    const float phase = Fract(
        Hash01(seed + 2u) -
        time * weather.rain_fall_speed / height);
    const float fall_age = (1.0f - phase) *
        height / weather.rain_fall_speed;
    const float x = local_camera_position[0] +
        (Hash01(seed) * 2.0f - 1.0f) * 29.0f +
        weather.wind.x * fall_age;
    const float y = local_camera_position[1] - 5.0f +
        phase * height;
    const float z = local_camera_position[2] +
        (Hash01(seed + 1u) * 2.0f - 1.0f) * 29.0f +
        weather.wind.z * fall_age;
    const float length =
        0.72f + Hash01(seed + 3u) * 0.72f;
    const float tail_x = x - direction_x * length;
    const float tail_y = y - direction_y * length;
    const float tail_z = z - direction_z * length;
    const float width =
        0.010f + Hash01(seed + 4u) * 0.010f;
    const std::size_t base = drop * 8;
    g_rain_visual.vertices[base + 0] =
        WeatherVertex(x - width, y, z);
    g_rain_visual.vertices[base + 1] =
        WeatherVertex(x + width, y, z);
    g_rain_visual.vertices[base + 2] =
        WeatherVertex(tail_x - width, tail_y, tail_z);
    g_rain_visual.vertices[base + 3] =
        WeatherVertex(tail_x + width, tail_y, tail_z);
    g_rain_visual.vertices[base + 4] =
        WeatherVertex(x, y, z - width);
    g_rain_visual.vertices[base + 5] =
        WeatherVertex(x, y, z + width);
    g_rain_visual.vertices[base + 6] =
        WeatherVertex(tail_x, tail_y, tail_z - width);
    g_rain_visual.vertices[base + 7] =
        WeatherVertex(tail_x, tail_y, tail_z + width);
  }
}

WeatherSnapshot ActiveWeatherSnapshot() {
  return g_weather_snapshot;
}

bool ActiveLightningLightSnapshot(MovingLightSnapshot& out) {
  if (!ActiveWorld().Definition().weather.enabled ||
      g_weather_snapshot.flash_intensity <= 0.001f) {
    return false;
  }
  out = {};
  out.position[0] = g_weather_snapshot.lightning_position[0];
  out.position[1] = g_weather_snapshot.lightning_position[1];
  out.position[2] = g_weather_snapshot.lightning_position[2];
  out.color[0] = 0.58f;
  out.color[1] = 0.72f;
  out.color[2] = 1.0f;
  out.source_radius = 2.5f;
  out.influence_radius = 180.0f;
  out.intensity =
      32.0f * g_weather_snapshot.flash_intensity;
  return true;
}

bool AdvanceWaterSimulation(float elapsed_seconds) {
  return ActiveWaterRuntime().Advance(elapsed_seconds);
}

bool ActiveWaterPusherPose(float out_position[3]) {
  if (out_position == nullptr || !ActiveWaterRuntime().basin) {
    return false;
  }
  const skate::world::KinematicPose& pose =
      ActiveWaterRuntime().pusher_pose;
  out_position[0] = pose.position.x;
  out_position[1] = pose.position.y;
  out_position[2] = pose.position.z;
  return true;
}

WaterTelemetry ActiveWaterTelemetry() {
  WaterTelemetry telemetry;
  telemetry.simulation_steps =
      g_water_simulation_steps.load(std::memory_order_acquire);
  telemetry.dropped_frames =
      g_water_dropped_frames.load(std::memory_order_acquire);
  telemetry.minimum_displacement =
      std::bit_cast<float>(
          g_water_minimum_bits.load(std::memory_order_acquire));
  telemetry.maximum_displacement =
      std::bit_cast<float>(
          g_water_maximum_bits.load(std::memory_order_acquire));
  telemetry.mean_displacement =
      std::bit_cast<float>(
          g_water_mean_bits.load(std::memory_order_acquire));
  telemetry.kinetic_energy =
      std::bit_cast<float>(
          g_water_energy_bits.load(std::memory_order_acquire));
  return telemetry;
}

bool QueryContact(const float position[3], float radius, Contact& out) {
  if (position == nullptr || radius < 0.0f) {
    return false;
  }

  const std::vector<skate::world::Contact> contacts =
      ActiveWorld().QuerySphere(
          {position[0], position[1], position[2]}, radius, 1);
  if (contacts.empty()) {
    return false;
  }

  const skate::world::Contact& source = contacts.front();
  out.id = source.surface;
  out.point[0] = source.point.x;
  out.point[1] = source.point.y;
  out.point[2] = source.point.z;
  out.normal[0] = source.normal.x;
  out.normal[1] = source.normal.y;
  out.normal[2] = source.normal.z;
  out.penetration = source.penetration;
  return true;
}

bool QueryRaySegment(const float start[3], const float delta[3], RayHit& out) {
  if (start == nullptr || delta == nullptr ||
      !std::isfinite(start[0]) || !std::isfinite(start[1]) ||
      !std::isfinite(start[2]) || !std::isfinite(delta[0]) ||
      !std::isfinite(delta[1]) || !std::isfinite(delta[2])) {
    return false;
  }
  const skate::world::Vec3 direction{delta[0], delta[1], delta[2]};
  const float distance = std::sqrt(skate::world::LengthSquared(direction));
  if (!std::isfinite(distance) || distance <= 1.0e-6f) {
    return false;
  }
  const skate::world::RayHit source = ActiveWorld().RayCast(
      {start[0], start[1], start[2]}, direction, distance);
  if (!source.hit) {
    return false;
  }
  out.id = source.surface;
  out.material = source.material;
  out.point[0] = source.point.x;
  out.point[1] = source.point.y;
  out.point[2] = source.point.z;
  out.normal[0] = source.normal.x;
  out.normal[1] = source.normal.y;
  out.normal[2] = source.normal.z;
  out.distance = source.distance;
  return true;
}

bool QueryGround(const float position[3], float probe_above,
                 float probe_below, GroundHit& out) {
  if (position == nullptr || probe_above < 0.0f || probe_below < 0.0f) {
    return false;
  }

  const skate::world::Vec3 origin{
      position[0], position[1] + probe_above, position[2]};
  const skate::world::RayHit source =
      ActiveWorld().RayCast(origin, {0.0f, -1.0f, 0.0f},
                            probe_above + probe_below);
  if (!source.hit) {
    return false;
  }

  out.id = source.surface;
  out.point[0] = source.point.x;
  out.point[1] = source.point.y;
  out.point[2] = source.point.z;
  out.normal[0] = source.normal.x;
  out.normal[1] = source.normal.y;
  out.normal[2] = source.normal.z;
  out.distance = source.distance;
  return true;
}

bool QueryLowestGround(const float position[3], float probe_above,
                       float probe_below, GroundHit& out) {
  if (position == nullptr || probe_above < 0.0f || probe_below < 0.0f) {
    return false;
  }

  const skate::world::Vec3 origin{
      position[0], position[1] + probe_above, position[2]};
  const skate::world::RayHit source =
      ActiveWorld().ProbeLowestSkateableGround(
          origin, probe_above + probe_below);
  if (!source.hit) {
    return false;
  }

  out.id = source.surface;
  out.point[0] = source.point.x;
  out.point[1] = source.point.y;
  out.point[2] = source.point.z;
  out.normal[0] = source.normal.x;
  out.normal[1] = source.normal.y;
  out.normal[2] = source.normal.z;
  out.distance = source.distance;
  return true;
}

}  // namespace skate3::mechanics_sandbox::map
