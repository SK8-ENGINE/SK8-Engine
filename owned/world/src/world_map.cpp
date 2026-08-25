#include "skate/world/world_map.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace skate::world {
namespace {

constexpr float kEpsilon = 1.0e-6f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

bool ValidColor(Vec3 color) {
  return color.x >= 0.0f && color.x <= 1.0f &&
         color.y >= 0.0f && color.y <= 1.0f &&
         color.z >= 0.0f && color.z <= 1.0f;
}

bool Finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

float SmoothStep(float edge0, float edge1, float value) {
  const float t =
      std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

Vec3 Lerp(Vec3 from, Vec3 to, float amount) {
  return from * (1.0f - amount) + to * amount;
}

Vec3 ClosestPointOnTriangle(Vec3 point,
                            Vec3 a,
                            Vec3 b,
                            Vec3 c) {
  const Vec3 ab = b - a;
  const Vec3 ac = c - a;
  const Vec3 ap = point - a;
  const float d1 = Dot(ab, ap);
  const float d2 = Dot(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f) {
    return a;
  }

  const Vec3 bp = point - b;
  const float d3 = Dot(ab, bp);
  const float d4 = Dot(ac, bp);
  if (d3 >= 0.0f && d4 <= d3) {
    return b;
  }

  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    const float value = d1 / (d1 - d3);
    return a + ab * value;
  }

  const Vec3 cp = point - c;
  const float d5 = Dot(ab, cp);
  const float d6 = Dot(ac, cp);
  if (d6 >= 0.0f && d5 <= d6) {
    return c;
  }

  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    const float value = d2 / (d2 - d6);
    return a + ac * value;
  }

  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f &&
      (d5 - d6) >= 0.0f) {
    const Vec3 edge = c - b;
    const float value =
        (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return b + edge * value;
  }

  const float denominator = 1.0f / (va + vb + vc);
  const float v = vb * denominator;
  const float w = vc * denominator;
  return a + ab * v + ac * w;
}

bool RayTriangle(Vec3 origin,
                 Vec3 direction,
                 const CollisionTriangle& triangle,
                 float& distance) {
  const Vec3 edge1 = triangle.b - triangle.a;
  const Vec3 edge2 = triangle.c - triangle.a;
  const Vec3 p = Cross(direction, edge2);
  const float determinant = Dot(edge1, p);
  if (std::abs(determinant) <= kEpsilon) {
    return false;
  }

  const float inverse = 1.0f / determinant;
  const Vec3 t = origin - triangle.a;
  const float u = Dot(t, p) * inverse;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const Vec3 q = Cross(t, edge1);
  const float v = Dot(direction, q) * inverse;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float candidate = Dot(edge2, q) * inverse;
  if (candidate < 0.0f) {
    return false;
  }

  distance = candidate;
  return true;
}

}  // namespace

KinematicPose EvaluateKinematicBox(const KinematicBox& object,
                                   float elapsed_seconds) {
  KinematicPose pose;
  if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0f ||
      !std::isfinite(object.travel_seconds) ||
      object.travel_seconds <= 0.0f) {
    pose.position = object.path_start;
    return pose;
  }

  const float leg = std::floor(elapsed_seconds / object.travel_seconds);
  const float leg_time =
      elapsed_seconds - leg * object.travel_seconds;
  const float leg_alpha =
      std::clamp(leg_time / object.travel_seconds, 0.0f, 1.0f);
  pose.returning =
      static_cast<std::uint64_t>(leg) % 2u != 0u;
  const float forward_alpha =
      pose.returning ? 1.0f - leg_alpha : leg_alpha;
  pose.path_alpha =
      0.5f - 0.5f * std::cos(kPi * forward_alpha);

  const Vec3 path = object.path_end - object.path_start;
  pose.position = object.path_start + path * pose.path_alpha;
  const float direction = pose.returning ? -1.0f : 1.0f;
  const float speed_scale =
      direction * 0.5f * kPi *
      std::sin(kPi * forward_alpha) / object.travel_seconds;
  pose.velocity = path * speed_scale;
  return pose;
}

MovingLightOrbPose EvaluateMovingLightOrb(
    const MovingLightOrb& light, float elapsed_seconds) {
  MovingLightOrbPose pose;
  pose.position = light.orbit_center + light.orbit_axis_u;
  if (!std::isfinite(elapsed_seconds) ||
      !std::isfinite(light.period_seconds) ||
      light.period_seconds <= 0.0f) {
    return pose;
  }
  const float angular_speed = kTwoPi / light.period_seconds;
  const float angle =
      light.phase_radians + elapsed_seconds * angular_speed;
  pose.position =
      light.orbit_center +
      light.orbit_axis_u * std::cos(angle) +
      light.orbit_axis_v * std::sin(angle);
  pose.velocity =
      light.orbit_axis_u * (-std::sin(angle) * angular_speed) +
      light.orbit_axis_v * (std::cos(angle) * angular_speed);
  return pose;
}

DayNightState EvaluateDayNightCycle(
    const DayNightCycleDefinition& cycle, float elapsed_seconds) {
  DayNightState state;
  if (!cycle.enabled || !std::isfinite(cycle.duration_seconds) ||
      cycle.duration_seconds < 0.0f) {
    return state;
  }

  const bool frozen = cycle.duration_seconds == 0.0f;
  state.elapsed_seconds =
      !frozen && std::isfinite(elapsed_seconds) && elapsed_seconds > 0.0f
          ? elapsed_seconds
          : 0.0f;
  const float cycle_offset =
      frozen
          ? 0.0f
          : std::fmod(state.elapsed_seconds, cycle.duration_seconds) /
                cycle.duration_seconds;
  if (cycle.ping_pong && !frozen) {
    // 0 -> 1 -> 0 over one authored duration, with a smooth turnaround at
    // morning/evening instead of stepping the sun direction backwards.
    const float path =
        0.5f - 0.5f * std::cos(cycle_offset * kTwoPi);
    state.phase =
        (cycle.start_time_hours +
         (cycle.end_time_hours - cycle.start_time_hours) * path) /
        24.0f;
  } else {
    state.phase =
        cycle.start_time_hours / 24.0f + cycle_offset;
  }
  state.phase = std::fmod(state.phase, 1.0f);
  if (state.phase < 0.0f) {
    state.phase += 1.0f;
  }
  state.time_of_day_hours = state.phase * 24.0f;

  // Sunrise is 06:00, solar noon is 12:00, and sunset is 18:00.
  const float orbit_angle = state.phase * kTwoPi - 0.5f * kPi;
  const float horizontal = std::cos(orbit_angle);
  const float azimuth_cos = std::cos(cycle.orbit_azimuth_radians);
  const float azimuth_sin = std::sin(cycle.orbit_azimuth_radians);
  state.sun_direction_to_light = Normalize({
      horizontal * azimuth_cos,
      std::sin(orbit_angle),
      horizontal * azimuth_sin,
  });
  state.moon_direction_to_light =
      state.sun_direction_to_light * -1.0f;

  state.sun_visibility =
      SmoothStep(-0.04f, 0.14f, state.sun_direction_to_light.y);
  state.moon_visibility =
      SmoothStep(-0.02f, 0.18f, state.moon_direction_to_light.y);
  state.daylight_amount =
      SmoothStep(-0.10f, 0.30f, state.sun_direction_to_light.y);
  state.night_amount =
      SmoothStep(-0.06f, 0.24f, state.moon_direction_to_light.y);
  state.twilight_amount =
      1.0f - SmoothStep(
                 0.02f, 0.34f,
                 std::abs(state.sun_direction_to_light.y));
  state.star_visibility =
      state.night_amount *
      SmoothStep(0.02f, 0.34f, state.moon_direction_to_light.y);

  Vec3 zenith =
      Lerp(cycle.night_zenith, cycle.day_zenith,
           state.daylight_amount);
  Vec3 horizon =
      Lerp(cycle.night_horizon, cycle.day_horizon,
           state.daylight_amount);
  Vec3 nadir =
      Lerp(cycle.night_nadir, cycle.day_nadir,
           state.daylight_amount);
  const float twilight_blend =
      state.twilight_amount *
      (1.0f - state.daylight_amount * 0.35f);
  state.sky_zenith =
      Lerp(zenith, cycle.twilight_zenith, twilight_blend * 0.72f);
  state.sky_horizon =
      Lerp(horizon, cycle.twilight_horizon, twilight_blend);
  state.sky_nadir =
      Lerp(nadir, cycle.twilight_nadir, twilight_blend * 0.62f);
  state.sky_zenith = {
      state.sky_zenith.x * cycle.sky_tint.x,
      state.sky_zenith.y * cycle.sky_tint.y,
      state.sky_zenith.z * cycle.sky_tint.z,
  };
  state.sky_horizon = {
      state.sky_horizon.x * cycle.sky_tint.x,
      state.sky_horizon.y * cycle.sky_tint.y,
      state.sky_horizon.z * cycle.sky_tint.z,
  };
  state.sky_nadir = {
      state.sky_nadir.x * cycle.sky_tint.x,
      state.sky_nadir.y * cycle.sky_tint.y,
      state.sky_nadir.z * cycle.sky_tint.z,
  };

  state.sun_is_key_light =
      state.sun_visibility >= state.moon_visibility;
  state.light_direction_to_light =
      state.sun_is_key_light ? state.sun_direction_to_light
                             : state.moon_direction_to_light;
  state.light_color =
      state.sun_is_key_light
          ? Lerp(cycle.twilight_horizon, cycle.sun_color,
                 SmoothStep(0.0f, 0.55f,
                            state.sun_direction_to_light.y))
          : cycle.moon_color;
  state.light_intensity =
      state.sun_is_key_light
          ? cycle.sun_intensity * state.sun_visibility
          : cycle.moon_intensity * state.moon_visibility;
  state.ambient =
      cycle.night_ambient +
      (cycle.day_ambient - cycle.night_ambient) *
          state.daylight_amount;
  return state;
}

MapBuilder::MapBuilder(std::string name) {
  if (name.empty()) {
    throw std::invalid_argument("map name must not be empty");
  }
  map_.name = std::move(name);
}

MaterialId MapBuilder::AddMaterial(std::string name,
                                   float friction,
                                   float restitution,
                                   SurfaceFlags flags,
                                   Vec3 display_color,
                                   MaterialPattern pattern,
                                   float texture_scale,
                                   float roughness,
                                   float variation,
                                   float emissive_intensity) {
  if (name.empty()) {
    throw std::invalid_argument("material name must not be empty");
  }
  if (friction < 0.0f || restitution < 0.0f) {
    throw std::invalid_argument(
        "material friction and restitution must be non-negative");
  }
  if (display_color.x < 0.0f || display_color.x > 1.0f ||
      display_color.y < 0.0f || display_color.y > 1.0f ||
      display_color.z < 0.0f || display_color.z > 1.0f) {
    throw std::invalid_argument(
        "material display color components must be between zero and one");
  }
  if (!std::isfinite(texture_scale) || texture_scale <= 0.0f ||
      !std::isfinite(roughness) || roughness < 0.0f || roughness > 1.0f ||
      !std::isfinite(variation) || variation < 0.0f ||
      variation > 1.0f ||
      !std::isfinite(emissive_intensity) ||
      emissive_intensity < 0.0f ||
      emissive_intensity > 64.0f) {
    throw std::invalid_argument(
        "material texture properties are invalid");
  }

  const MaterialId id = next_material_id_++;
  SurfaceMaterial material;
  material.id = id;
  material.name = std::move(name);
  material.friction = friction;
  material.restitution = restitution;
  material.flags = flags;
  material.display_color = display_color;
  material.pattern = pattern;
  material.texture_scale = texture_scale;
  material.roughness = roughness;
  material.variation = variation;
  material.emissive_intensity = emissive_intensity;
  map_.materials.push_back(std::move(material));
  return id;
}

void MapBuilder::SetSpawn(Vec3 position, float heading_radians) {
  map_.spawn = {position, heading_radians};
}

void MapBuilder::SetSky(Vec3 zenith_color,
                        Vec3 horizon_color,
                        Vec3 nadir_color) {
  if (!ValidColor(zenith_color) || !ValidColor(horizon_color) ||
      !ValidColor(nadir_color)) {
    throw std::invalid_argument(
        "sky color components must be between zero and one");
  }
  map_.sky = {true, zenith_color, horizon_color, nadir_color};
}

void MapBuilder::SetDirectionalSun(Vec3 direction_to_light,
                                   Vec3 color,
                                   float intensity,
                                   float ambient) {
  if (LengthSquared(direction_to_light) <= kEpsilon ||
      !ValidColor(color) || !std::isfinite(intensity) ||
      !std::isfinite(ambient) || intensity < 0.0f ||
      ambient < 0.0f || ambient > 1.0f) {
    throw std::invalid_argument("directional sun definition is invalid");
  }
  map_.sun = {
      Normalize(direction_to_light), color, intensity, ambient};
}

void MapBuilder::SetDayNightCycle(DayNightCycleDefinition cycle) {
  const Vec3 colors[] = {
      cycle.day_zenith, cycle.day_horizon, cycle.day_nadir,
      cycle.twilight_zenith, cycle.twilight_horizon,
      cycle.twilight_nadir, cycle.night_zenith,
      cycle.night_horizon, cycle.night_nadir, cycle.sun_color,
      cycle.moon_color,
  };
  const bool colors_valid = std::all_of(
      std::begin(colors), std::end(colors), ValidColor);
  if (!cycle.enabled || !std::isfinite(cycle.duration_seconds) ||
      cycle.duration_seconds < 0.0f ||
      !std::isfinite(cycle.start_time_hours) ||
      cycle.start_time_hours < 0.0f ||
      cycle.start_time_hours >= 24.0f ||
      !std::isfinite(cycle.end_time_hours) ||
      cycle.end_time_hours < 0.0f ||
      cycle.end_time_hours >= 24.0f ||
      !std::isfinite(cycle.orbit_azimuth_radians) ||
      !std::isfinite(cycle.sun_intensity) ||
      !std::isfinite(cycle.moon_intensity) ||
      !std::isfinite(cycle.day_ambient) ||
      !std::isfinite(cycle.night_ambient) ||
      cycle.sun_intensity < 0.0f || cycle.moon_intensity < 0.0f ||
      cycle.day_ambient < 0.0f || cycle.day_ambient > 1.0f ||
      cycle.night_ambient < 0.0f || cycle.night_ambient > 1.0f ||
      !colors_valid) {
    throw std::invalid_argument("day/night cycle definition is invalid");
  }
  map_.day_night_cycle = std::move(cycle);
}

void MapBuilder::SetWeather(float rain_intensity, Vec3 wind,
                            float rain_fall_speed,
                            float lightning_interval_min,
                            float lightning_interval_max,
                            float thunder_delay) {
  if (!std::isfinite(rain_intensity) || rain_intensity < 0.0f ||
      rain_intensity > 1.0f || !Finite(wind) ||
      !std::isfinite(rain_fall_speed) || rain_fall_speed <= 0.0f ||
      !std::isfinite(lightning_interval_min) ||
      !std::isfinite(lightning_interval_max) ||
      lightning_interval_min <= 0.0f ||
      lightning_interval_max < lightning_interval_min ||
      !std::isfinite(thunder_delay) || thunder_delay < 0.0f) {
    throw std::invalid_argument("weather definition is invalid");
  }
  map_.weather = {
      true, rain_intensity, wind, rain_fall_speed,
      lightning_interval_min, lightning_interval_max, thunder_delay};
}

GrindRailId MapBuilder::AddGrindRail(std::string name,
                                     std::vector<Vec3> points,
                                     bool closed) {
  if (name.empty()) {
    throw std::invalid_argument("grind rail name must not be empty");
  }
  if (points.size() < 2) {
    throw std::invalid_argument(
        "grind rail must contain at least two points");
  }
  const bool duplicate_name = std::any_of(
      map_.grind_rails.begin(), map_.grind_rails.end(),
      [&name](const GrindRail& rail) { return rail.name == name; });
  if (duplicate_name) {
    throw std::invalid_argument("grind rail name must be unique");
  }

  bool has_segment = false;
  for (std::size_t index = 1; index < points.size(); ++index) {
    if (LengthSquared(points[index] - points[index - 1]) > kEpsilon) {
      has_segment = true;
      break;
    }
  }
  if (!has_segment && closed &&
      LengthSquared(points.front() - points.back()) > kEpsilon) {
    has_segment = true;
  }
  if (!has_segment) {
    throw std::invalid_argument(
        "grind rail must contain a non-zero-length segment");
  }

  const GrindRailId id = next_grind_rail_id_++;
  GrindRail rail;
  rail.id = id;
  rail.name = std::move(name);
  rail.points = std::move(points);
  rail.closed = closed;
  map_.grind_rails.push_back(std::move(rail));
  return id;
}

KinematicObjectId MapBuilder::AddKinematicBox(
    std::string name, SurfaceId surface, MaterialId material,
    Vec3 local_min, Vec3 local_max, Vec3 path_start, Vec3 path_end,
    float travel_seconds) {
  RequireMaterial(material);
  if (name.empty()) {
    throw std::invalid_argument(
        "kinematic object name must not be empty");
  }
  if (surface == 0) {
    throw std::invalid_argument(
        "kinematic object surface id zero is reserved");
  }
  if (!Finite(local_min) || !Finite(local_max) ||
      local_min.x >= local_max.x || local_min.y >= local_max.y ||
      local_min.z >= local_max.z) {
    throw std::invalid_argument(
        "kinematic object local bounds must be finite and ordered");
  }
  if (!Finite(path_start) || !Finite(path_end) ||
      LengthSquared(path_end - path_start) <= kEpsilon ||
      !std::isfinite(travel_seconds) || travel_seconds <= 0.0f) {
    throw std::invalid_argument(
        "kinematic object path must be finite, non-zero, and timed");
  }
  const bool duplicate_name = std::any_of(
      map_.kinematic_boxes.begin(), map_.kinematic_boxes.end(),
      [&name](const KinematicBox& object) {
        return object.name == name;
      });
  if (duplicate_name) {
    throw std::invalid_argument(
        "kinematic object name must be unique");
  }

  const KinematicObjectId id = next_kinematic_object_id_++;
  map_.kinematic_boxes.push_back(
      {id, std::move(name), local_min, local_max, path_start,
       path_end, travel_seconds, surface, material});
  return id;
}

WaterBasinId MapBuilder::AddWaterBasin(WaterBasin basin) {
  RequireMaterial(basin.pusher_material);
  if (basin.name.empty()) {
    throw std::invalid_argument("water basin name must not be empty");
  }
  if (!Finite(basin.minimum) || !Finite(basin.maximum) ||
      basin.minimum.x >= basin.maximum.x ||
      basin.minimum.y >= basin.maximum.y ||
      basin.minimum.z >= basin.maximum.z ||
      !std::isfinite(basin.rest_surface_height) ||
      basin.rest_surface_height <= basin.minimum.y ||
      basin.rest_surface_height >= basin.maximum.y ||
      basin.columns < 3 || basin.rows < 3 ||
      basin.columns > 512 || basin.rows > 512 ||
      !std::isfinite(basin.damping) || basin.damping < 0.0f) {
    throw std::invalid_argument(
        "water basin domain must be finite, ordered, and bounded");
  }
  if (!Finite(basin.pusher_local_min) ||
      !Finite(basin.pusher_local_max) ||
      basin.pusher_local_min.x >= basin.pusher_local_max.x ||
      basin.pusher_local_min.y >= basin.pusher_local_max.y ||
      basin.pusher_local_min.z >= basin.pusher_local_max.z ||
      !Finite(basin.pusher_path_start) ||
      !Finite(basin.pusher_path_end) ||
      LengthSquared(basin.pusher_path_end -
                    basin.pusher_path_start) <= kEpsilon ||
      !std::isfinite(basin.pusher_travel_seconds) ||
      basin.pusher_travel_seconds <= 0.0f) {
    throw std::invalid_argument(
        "water pusher bounds and path are invalid");
  }
  const bool duplicate_name = std::any_of(
      map_.water_basins.begin(), map_.water_basins.end(),
      [&basin](const WaterBasin& candidate) {
        return candidate.name == basin.name;
      });
  if (duplicate_name) {
    throw std::invalid_argument("water basin name must be unique");
  }
  basin.id = next_water_basin_id_++;
  map_.water_basins.push_back(std::move(basin));
  return map_.water_basins.back().id;
}

RaytracedMirrorId MapBuilder::AddRaytracedMirror(
    RaytracedMirror mirror) {
  if (mirror.name.empty() || !Finite(mirror.center) ||
      !Finite(mirror.right) || !Finite(mirror.up) ||
      !std::isfinite(mirror.half_width) ||
      !std::isfinite(mirror.half_height) ||
      mirror.half_width <= 0.0f || mirror.half_height <= 0.0f) {
    throw std::invalid_argument(
        "raytraced mirror must have finite geometry");
  }
  const Vec3 right = Normalize(mirror.right);
  const Vec3 up = Normalize(mirror.up);
  if (LengthSquared(right) <= kEpsilon ||
      LengthSquared(up) <= kEpsilon ||
      std::abs(Dot(right, up)) > 1.0e-3f) {
    throw std::invalid_argument(
        "raytraced mirror axes must be orthonormal");
  }
  const bool duplicate_name = std::any_of(
      map_.raytraced_mirrors.begin(),
      map_.raytraced_mirrors.end(),
      [&mirror](const RaytracedMirror& candidate) {
        return candidate.name == mirror.name;
      });
  if (duplicate_name) {
    throw std::invalid_argument(
        "raytraced mirror name must be unique");
  }
  mirror.id = next_raytraced_mirror_id_++;
  mirror.right = right;
  mirror.up = up;
  map_.raytraced_mirrors.push_back(std::move(mirror));
  return map_.raytraced_mirrors.back().id;
}

RaytracedPuddleId MapBuilder::AddRaytracedPuddle(
    RaytracedPuddle puddle) {
  if (puddle.name.empty() || !Finite(puddle.center) ||
      !Finite(puddle.right) || !Finite(puddle.forward) ||
      !std::isfinite(puddle.half_width) ||
      !std::isfinite(puddle.half_length) ||
      !std::isfinite(puddle.reflectivity) ||
      !std::isfinite(puddle.ripple_strength) ||
      puddle.half_width <= 0.0f || puddle.half_length <= 0.0f ||
      puddle.reflectivity < 0.0f || puddle.reflectivity > 1.0f ||
      puddle.ripple_strength < 0.0f ||
      LengthSquared(puddle.right) <= kEpsilon ||
      LengthSquared(puddle.forward) <= kEpsilon) {
    throw std::invalid_argument(
        "raytraced puddle must have finite geometry");
  }
  Vec3 right = Normalize(puddle.right);
  Vec3 forward =
      puddle.forward - right * Dot(puddle.forward, right);
  if (LengthSquared(forward) <= kEpsilon) {
    throw std::invalid_argument(
        "raytraced puddle axes must be independent");
  }
  forward = Normalize(forward);
  const bool duplicate_name = std::any_of(
      map_.raytraced_puddles.begin(), map_.raytraced_puddles.end(),
      [&puddle](const RaytracedPuddle& candidate) {
        return candidate.name == puddle.name;
      });
  if (duplicate_name) {
    throw std::invalid_argument(
        "raytraced puddle name must be unique");
  }
  puddle.id = next_raytraced_puddle_id_++;
  puddle.right = right;
  puddle.forward = forward;
  map_.raytraced_puddles.push_back(std::move(puddle));
  return map_.raytraced_puddles.back().id;
}

MovingLightOrbId MapBuilder::AddMovingLightOrb(
    MovingLightOrb light) {
  const bool static_light = light.period_seconds == 0.0f;
  if (light.name.empty() || !Finite(light.orbit_center) ||
      !Finite(light.orbit_axis_u) || !Finite(light.orbit_axis_v) ||
      !Finite(light.direction) || !ValidColor(light.color) ||
      static_cast<std::uint32_t>(light.type) >
          static_cast<std::uint32_t>(LocalLightType::Area) ||
      (!static_light &&
       (LengthSquared(light.orbit_axis_u) <= kEpsilon ||
        LengthSquared(light.orbit_axis_v) <= kEpsilon)) ||
      !std::isfinite(light.source_radius) ||
      !std::isfinite(light.influence_radius) ||
      !std::isfinite(light.intensity) ||
      !std::isfinite(light.period_seconds) ||
      !std::isfinite(light.phase_radians) ||
      !std::isfinite(light.spot_inner_cosine) ||
      !std::isfinite(light.spot_outer_cosine) ||
      light.spot_inner_cosine < -1.0f ||
      light.spot_inner_cosine > 1.0f ||
      light.spot_outer_cosine < -1.0f ||
      light.spot_outer_cosine > light.spot_inner_cosine ||
      light.source_radius <= 0.0f ||
      light.influence_radius <= light.source_radius ||
      light.intensity <= 0.0f ||
      light.period_seconds < 0.0f ||
      LengthSquared(light.direction) <= kEpsilon) {
    throw std::invalid_argument(
        "moving light orb definition is invalid");
  }
  const bool duplicate_name = std::any_of(
      map_.moving_light_orbs.begin(),
      map_.moving_light_orbs.end(),
      [&light](const MovingLightOrb& candidate) {
        return candidate.name == light.name;
      });
  if (duplicate_name) {
    throw std::invalid_argument(
        "moving light orb name must be unique");
  }
  light.id = next_moving_light_orb_id_++;
  light.direction = Normalize(light.direction);
  map_.moving_light_orbs.push_back(std::move(light));
  return map_.moving_light_orbs.back().id;
}

void MapBuilder::RequireMaterial(MaterialId material) const {
  const bool found = std::any_of(
      map_.materials.begin(),
      map_.materials.end(),
      [material](const SurfaceMaterial& candidate) {
        return candidate.id == material;
      });
  if (!found) {
    throw std::invalid_argument("surface references an unknown material");
  }
}

void MapBuilder::AddQuad(SurfaceId surface,
                         MaterialId material,
                         Vec3 a,
                         Vec3 b,
                         Vec3 c,
                         Vec3 d,
                         Vec2 uv_scale) {
  RequireMaterial(material);
  if (surface == 0) {
    throw std::invalid_argument("surface id zero is reserved");
  }

  const Vec3 first_normal = Cross(b - a, c - a);
  const Vec3 second_normal = Cross(c - a, d - a);
  const bool first_valid = LengthSquared(first_normal) > kEpsilon;
  const bool second_valid = LengthSquared(second_normal) > kEpsilon;
  if (!first_valid && !second_valid) {
    throw std::invalid_argument("quad must have non-zero area");
  }
  const Vec3 normal = Normalize(first_normal + second_normal);

  const std::uint32_t base =
      static_cast<std::uint32_t>(map_.render_mesh.vertices.size());
  map_.render_mesh.vertices.insert(
      map_.render_mesh.vertices.end(),
      {
          {a, normal, {0.0f, 0.0f}, material, {0.0f, 0.0f},
           {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f},
          {b, normal, {0.0f, uv_scale.y}, material,
           {0.0f, uv_scale.y}, {0.0f, uv_scale.y},
           {0.0f, 0.0f, 0.0f}, 0.0f},
          {c, normal, {uv_scale.x, uv_scale.y}, material,
           {uv_scale.x, uv_scale.y}, {uv_scale.x, uv_scale.y},
           {0.0f, 0.0f, 0.0f}, 0.0f},
          {d, normal, {uv_scale.x, 0.0f}, material,
           {uv_scale.x, 0.0f}, {uv_scale.x, 0.0f},
           {0.0f, 0.0f, 0.0f}, 0.0f},
      });
  map_.render_mesh.indices.insert(
      map_.render_mesh.indices.end(),
      {base, base + 1, base + 2, base, base + 2, base + 3});
  if (first_valid) {
    map_.collision_triangles.push_back(
        {a, b, c, normal, surface, material});
  }
  if (second_valid) {
    map_.collision_triangles.push_back(
        {a, c, d, normal, surface, material});
  }
}

void MapBuilder::AddWedgeX(SurfaceId surface,
                           MaterialId material,
                           float x_min,
                           float x_max,
                           float z_min,
                           float z_max,
                           float y_low,
                           float y_high) {
  if (x_min >= x_max || z_min >= z_max) {
    throw std::invalid_argument("wedge bounds must be ordered");
  }

  const float base_y = std::min(y_low, y_high);
  const Vec3 slope_a{x_min, y_low, z_min};
  const Vec3 slope_b{x_min, y_low, z_max};
  const Vec3 slope_c{x_max, y_high, z_max};
  const Vec3 slope_d{x_max, y_high, z_min};
  AddQuad(surface, material, slope_a, slope_b, slope_c, slope_d);

  if (y_high > base_y) {
    AddQuad(surface, material,
            {x_max, base_y, z_min},
            {x_max, y_high, z_min},
            {x_max, y_high, z_max},
            {x_max, base_y, z_max});
  }
  if (y_low > base_y) {
    AddQuad(surface, material,
            {x_min, base_y, z_max},
            {x_min, y_low, z_max},
            {x_min, y_low, z_min},
            {x_min, base_y, z_min});
  }

  AddQuad(surface, material,
          {x_min, base_y, z_min},
          {x_min, y_low, z_min},
          {x_max, y_high, z_min},
          {x_max, base_y, z_min});
  AddQuad(surface, material,
          {x_max, base_y, z_max},
          {x_max, y_high, z_max},
          {x_min, y_low, z_max},
          {x_min, base_y, z_max});
}

void MapBuilder::AddBox(SurfaceId surface,
                        MaterialId material,
                        Vec3 minimum,
                        Vec3 maximum) {
  if (minimum.x >= maximum.x || minimum.y >= maximum.y ||
      minimum.z >= maximum.z) {
    throw std::invalid_argument("box bounds must be ordered");
  }

  AddQuad(surface, material,
          {minimum.x, maximum.y, minimum.z},
          {minimum.x, maximum.y, maximum.z},
          {maximum.x, maximum.y, maximum.z},
          {maximum.x, maximum.y, minimum.z});
  AddQuad(surface, material,
          {minimum.x, minimum.y, maximum.z},
          {minimum.x, minimum.y, minimum.z},
          {maximum.x, minimum.y, minimum.z},
          {maximum.x, minimum.y, maximum.z});
  AddQuad(surface, material,
          {minimum.x, minimum.y, minimum.z},
          {minimum.x, maximum.y, minimum.z},
          {maximum.x, maximum.y, minimum.z},
          {maximum.x, minimum.y, minimum.z});
  AddQuad(surface, material,
          {maximum.x, minimum.y, maximum.z},
          {maximum.x, maximum.y, maximum.z},
          {minimum.x, maximum.y, maximum.z},
          {minimum.x, minimum.y, maximum.z});
  AddQuad(surface, material,
          {minimum.x, minimum.y, maximum.z},
          {minimum.x, maximum.y, maximum.z},
          {minimum.x, maximum.y, minimum.z},
          {minimum.x, minimum.y, minimum.z});
  AddQuad(surface, material,
          {maximum.x, minimum.y, minimum.z},
          {maximum.x, maximum.y, minimum.z},
          {maximum.x, maximum.y, maximum.z},
          {maximum.x, minimum.y, maximum.z});
}

MapDefinition MapBuilder::Build() && {
  if (map_.materials.empty()) {
    throw std::logic_error("map must contain at least one material");
  }
  if (map_.collision_triangles.empty()) {
    throw std::logic_error("map must contain collision geometry");
  }
  return std::move(map_);
}

WorldMap::WorldMap(MapDefinition definition)
    : definition_(std::move(definition)) {
  if (definition_.name.empty()) {
    throw std::invalid_argument("map definition has no name");
  }
  if (definition_.collision_triangles.empty()) {
    throw std::invalid_argument("map definition has no collision geometry");
  }
}

const MapDefinition& WorldMap::Definition() const {
  return definition_;
}

MapDefinition& WorldMap::MutableDefinition() {
  return definition_;
}

const SurfaceMaterial* WorldMap::FindMaterial(MaterialId id) const {
  const auto found = std::find_if(
      definition_.materials.begin(),
      definition_.materials.end(),
      [id](const SurfaceMaterial& material) {
        return material.id == id;
      });
  return found == definition_.materials.end() ? nullptr : &*found;
}

RayHit WorldMap::RayCast(Vec3 origin,
                         Vec3 direction,
                         float maximum_distance) const {
  RayHit result;
  if (maximum_distance < 0.0f) {
    return result;
  }

  direction = Normalize(direction);
  if (LengthSquared(direction) <= kEpsilon) {
    return result;
  }

  for (const CollisionTriangle& triangle :
       definition_.collision_triangles) {
    float distance = 0.0f;
    if (!RayTriangle(origin, direction, triangle, distance) ||
        distance > maximum_distance ||
        distance >= result.distance) {
      continue;
    }

    result.hit = true;
    result.distance = distance;
    result.point = origin + direction * distance;
    result.normal = triangle.normal;
    if (Dot(result.normal, direction) > 0.0f) {
      result.normal = result.normal * -1.0f;
    }
    result.surface = triangle.surface;
    result.material = triangle.material;
  }

  return result;
}

RayHit WorldMap::ProbeGround(Vec3 origin,
                             float maximum_distance) const {
  return RayCast(origin, {0.0f, -1.0f, 0.0f}, maximum_distance);
}

RayHit WorldMap::ProbeLowestSkateableGround(
    Vec3 origin, float maximum_distance) const {
  RayHit result;
  if (maximum_distance < 0.0f) {
    return result;
  }

  float farthest_distance = -1.0f;
  for (const CollisionTriangle& triangle :
       definition_.collision_triangles) {
    const SurfaceMaterial* material = FindMaterial(triangle.material);
    if (triangle.normal.y <= 0.45f || material == nullptr ||
        !HasFlag(material->flags, SurfaceFlags::Skateable)) {
      continue;
    }

    float distance = 0.0f;
    if (!RayTriangle(origin, {0.0f, -1.0f, 0.0f}, triangle, distance) ||
        distance > maximum_distance || distance <= farthest_distance) {
      continue;
    }

    farthest_distance = distance;
    result.hit = true;
    result.distance = distance;
    result.point = origin + Vec3{0.0f, -1.0f, 0.0f} * distance;
    result.normal = triangle.normal;
    result.surface = triangle.surface;
    result.material = triangle.material;
  }
  return result;
}

std::vector<Contact> WorldMap::QuerySphere(
    Vec3 center,
    float radius,
    std::size_t maximum_contacts) const {
  std::vector<Contact> contacts;
  if (radius < 0.0f || maximum_contacts == 0) {
    return contacts;
  }

  const float radius_squared = radius * radius;
  for (const CollisionTriangle& triangle :
       definition_.collision_triangles) {
    const Vec3 point = ClosestPointOnTriangle(
        center, triangle.a, triangle.b, triangle.c);
    const Vec3 delta = center - point;
    const float distance_squared = LengthSquared(delta);
    if (distance_squared > radius_squared) {
      continue;
    }

    const float distance =
        std::sqrt(std::max(0.0f, distance_squared));
    Vec3 normal = distance > kEpsilon
                      ? delta / distance
                      : triangle.normal;
    if (Dot(normal, triangle.normal) < 0.0f) {
      normal = normal * -1.0f;
    }
    contacts.push_back(
        {point,
         normal,
         radius - distance,
         triangle.surface,
         triangle.material});
  }

  std::sort(
      contacts.begin(),
      contacts.end(),
      [](const Contact& left, const Contact& right) {
        return left.penetration > right.penetration;
      });
  if (contacts.size() > maximum_contacts) {
    contacts.resize(maximum_contacts);
  }
  return contacts;
}

}  // namespace skate::world
