#include "skate/world/maps.h"

#include <algorithm>
#include <cmath>

namespace skate::world {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct SurfaceIds {
  SurfaceId next = 1;

  SurfaceId Take() {
    return next++;
  }
};

void AddBankX(MapBuilder& map, SurfaceIds& ids, MaterialId material,
              float x0, float x1, float z0, float z1,
              float y0, float y1) {
  map.AddWedgeX(ids.Take(), material, x0, x1, z0, z1, y0, y1);
}

void AddBankZ(MapBuilder& map, SurfaceIds& ids, MaterialId material,
              float x0, float x1, float z0, float z1,
              float y0, float y1) {
  const SurfaceId surface = ids.Take();
  const float base = std::min(y0, y1);
  map.AddQuad(surface, material,
              {x0, y0, z0}, {x0, y1, z1},
              {x1, y1, z1}, {x1, y0, z0},
              {x1 - x0, z1 - z0});
  if (y0 > base) {
    map.AddQuad(surface, material,
                {x1, base, z0}, {x1, y0, z0},
                {x0, y0, z0}, {x0, base, z0});
  }
  if (y1 > base) {
    map.AddQuad(surface, material,
                {x0, base, z1}, {x0, y1, z1},
                {x1, y1, z1}, {x1, base, z1});
  }
  map.AddQuad(surface, material,
              {x0, base, z0}, {x0, y0, z0},
              {x0, y1, z1}, {x0, base, z1});
  map.AddQuad(surface, material,
              {x1, base, z1}, {x1, y1, z1},
              {x1, y0, z0}, {x1, base, z0});
}

void AddQuarterPipeX(MapBuilder& map, SurfaceIds& ids,
                     MaterialId riding_material,
                     MaterialId side_material,
                     float x0, float x1, float z0, float z1,
                     float height, bool rise_positive_x) {
  constexpr int kSegments = 10;
  const SurfaceId riding_surface = ids.Take();
  float previous_x = rise_positive_x ? x0 : x1;
  float previous_y = 0.0f;
  for (int segment = 1; segment <= kSegments; ++segment) {
    const float t = static_cast<float>(segment) / kSegments;
    const float profile = 1.0f - std::cos(t * kPi * 0.46f);
    const float current_x =
        rise_positive_x ? x0 + (x1 - x0) * t
                        : x1 - (x1 - x0) * t;
    const float current_y = height * profile;
    const float xa = std::min(previous_x, current_x);
    const float xb = std::max(previous_x, current_x);
    const float ya = previous_x <= current_x ? previous_y : current_y;
    const float yb = previous_x <= current_x ? current_y : previous_y;
    map.AddQuad(riding_surface, riding_material,
                {xa, ya, z0}, {xa, ya, z1},
                {xb, yb, z1}, {xb, yb, z0},
                {xb - xa, z1 - z0});
    previous_x = current_x;
    previous_y = current_y;
  }

  const SurfaceId side_surface = ids.Take();
  const float high_x = rise_positive_x ? x1 : x0;
  map.AddBox(side_surface, side_material,
             {high_x - 0.35f, 0.0f, z0 - 0.35f},
             {high_x + 0.35f, height, z1 + 0.35f});
}

void AddStairSetX(MapBuilder& map, SurfaceIds& ids, MaterialId material,
                  float x0, float z0, float width, float tread,
                  float rise, int count, bool rise_positive_x) {
  const SurfaceId surface = ids.Take();
  for (int step = 0; step < count; ++step) {
    const float xa = rise_positive_x
                         ? x0 + step * tread
                         : x0 - (step + 1) * tread;
    const float xb = xa + tread;
    const float top = (step + 1) * rise;
    map.AddBox(surface, material,
               {xa, -0.04f, z0}, {xb, top, z0 + width});
  }
}

void AddRailX(MapBuilder& map, SurfaceIds& ids, MaterialId metal,
              const char* name,
              float x0, float x1, float z, float height) {
  const SurfaceId surface = ids.Take();
  map.AddBox(surface, metal,
             {x0, height - 0.055f, z - 0.055f},
             {x1, height + 0.055f, z + 0.055f});
  for (float x : {x0 + 0.3f, x1 - 0.3f}) {
    map.AddBox(surface, metal,
               {x - 0.045f, 0.0f, z - 0.045f},
               {x + 0.045f, height, z + 0.045f});
  }
  map.AddGrindRail(
      name, {{x0, height + 0.055f, z},
             {x1, height + 0.055f, z}});
}

void AddRailZ(MapBuilder& map, SurfaceIds& ids, MaterialId metal,
              const char* name,
              float x, float z0, float z1, float height) {
  const SurfaceId surface = ids.Take();
  map.AddBox(surface, metal,
             {x - 0.055f, height - 0.055f, z0},
             {x + 0.055f, height + 0.055f, z1});
  for (float z : {z0 + 0.3f, z1 - 0.3f}) {
    map.AddBox(surface, metal,
               {x - 0.045f, 0.0f, z - 0.045f},
               {x + 0.045f, height, z + 0.045f});
  }
  map.AddGrindRail(
      name, {{x, height + 0.055f, z0},
             {x, height + 0.055f, z1}});
}

void AddBench(MapBuilder& map, SurfaceIds& ids,
              MaterialId wood, MaterialId metal,
              float x, float z, bool along_x) {
  const SurfaceId surface = ids.Take();
  const Vec3 seat_min = along_x
                            ? Vec3{x - 1.4f, 0.42f, z - 0.32f}
                            : Vec3{x - 0.32f, 0.42f, z - 1.4f};
  const Vec3 seat_max = along_x
                            ? Vec3{x + 1.4f, 0.56f, z + 0.32f}
                            : Vec3{x + 0.32f, 0.56f, z + 1.4f};
  map.AddBox(surface, wood, seat_min, seat_max);
  for (float offset : {-0.9f, 0.9f}) {
    const Vec3 leg_min = along_x
                             ? Vec3{x + offset - 0.06f, 0.0f, z - 0.22f}
                             : Vec3{x - 0.22f, 0.0f, z + offset - 0.06f};
    const Vec3 leg_max = along_x
                             ? Vec3{x + offset + 0.06f, 0.44f, z + 0.22f}
                             : Vec3{x + 0.22f, 0.44f, z + offset + 0.06f};
    map.AddBox(surface, metal, leg_min, leg_max);
  }
}

void AddBuilding(MapBuilder& map, SurfaceIds& ids,
                 MaterialId wall, MaterialId roof, MaterialId glass,
                 Vec3 minimum, Vec3 maximum, int window_rows) {
  map.AddBox(ids.Take(), wall, minimum, maximum);
  map.AddBox(ids.Take(), roof,
             {minimum.x - 0.25f, maximum.y, minimum.z - 0.25f},
             {maximum.x + 0.25f, maximum.y + 0.35f, maximum.z + 0.25f});

  const float available = maximum.y - minimum.y - 2.0f;
  for (int row = 0; row < window_rows; ++row) {
    const float y = minimum.y + 1.4f +
                    available * (static_cast<float>(row) + 0.5f) /
                        std::max(window_rows, 1);
    map.AddBox(ids.Take(), glass,
               {minimum.x + 1.0f, y - 0.35f, minimum.z - 0.08f},
               {maximum.x - 1.0f, y + 0.35f, minimum.z + 0.04f});
  }
}

void AddCyberTower(MapBuilder& map, SurfaceIds& ids,
                   MaterialId body, MaterialId roof,
                   MaterialId lit_glass, MaterialId neon,
                   Vec3 minimum, Vec3 maximum, int window_rows) {
  map.AddBox(ids.Take(), body, minimum, maximum);
  map.AddBox(ids.Take(), roof,
             {minimum.x - 0.35f, maximum.y, minimum.z - 0.35f},
             {maximum.x + 0.35f, maximum.y + 0.55f,
              maximum.z + 0.35f});

  const float facade_z = minimum.z - 0.10f;
  const float available_height = maximum.y - minimum.y - 3.0f;
  for (int row = 0; row < window_rows; ++row) {
    const float y =
        minimum.y + 2.0f +
        available_height * (static_cast<float>(row) + 0.5f) /
            std::max(window_rows, 1);
    map.AddBox(ids.Take(), lit_glass,
               {minimum.x + 1.1f, y - 0.30f, facade_z - 0.05f},
               {maximum.x - 1.1f, y + 0.30f, facade_z + 0.05f});
  }

  // Continuous emissive edge strips make the authored volume readable from
  // skating distance and remain true reflected geometry in the DXR scene.
  for (float x : {minimum.x + 0.42f, maximum.x - 0.42f}) {
    map.AddBox(ids.Take(), neon,
               {x - 0.075f, minimum.y + 0.8f, facade_z - 0.08f},
               {x + 0.075f, maximum.y - 0.45f, facade_z + 0.08f});
  }
  map.AddBox(ids.Take(), neon,
             {minimum.x + 0.35f, maximum.y - 1.05f, facade_z - 0.09f},
             {maximum.x - 0.35f, maximum.y - 0.83f, facade_z + 0.09f});

  const float crown_width = (maximum.x - minimum.x) * 0.34f;
  const float crown_center = (minimum.x + maximum.x) * 0.5f;
  map.AddBox(ids.Take(), neon,
             {crown_center - crown_width, maximum.y + 0.55f,
              (minimum.z + maximum.z) * 0.5f - 0.11f},
             {crown_center + crown_width, maximum.y + 1.05f,
              (minimum.z + maximum.z) * 0.5f + 0.11f});
}

void AddNeonBillboard(MapBuilder& map, SurfaceIds& ids,
                      MaterialId frame, MaterialId panel,
                      MaterialId accent, float x0, float x1,
                      float z, float bottom, float top) {
  map.AddBox(ids.Take(), frame,
             {x0 - 0.16f, 0.0f, z - 0.12f},
             {x0 + 0.16f, bottom, z + 0.12f});
  map.AddBox(ids.Take(), frame,
             {x1 - 0.16f, 0.0f, z - 0.12f},
             {x1 + 0.16f, bottom, z + 0.12f});
  map.AddBox(ids.Take(), frame,
             {x0 - 0.22f, bottom - 0.20f, z - 0.16f},
             {x1 + 0.22f, top + 0.20f, z + 0.16f});
  map.AddBox(ids.Take(), panel,
             {x0, bottom, z - 0.18f},
             {x1, top, z - 0.14f});
  const float stripe = bottom + (top - bottom) * 0.34f;
  map.AddBox(ids.Take(), accent,
             {x0 + 0.45f, stripe - 0.09f, z - 0.22f},
             {x1 - 0.45f, stripe + 0.09f, z - 0.17f});
}

void AddGroundWithRectangularHole(
    MapBuilder& map, SurfaceIds& ids, MaterialId material,
    float outer_x_min, float outer_x_max,
    float outer_z_min, float outer_z_max, float height,
    float hole_x_min, float hole_x_max,
    float hole_z_min, float hole_z_max) {
  const float cut_x_min = std::clamp(
      hole_x_min, outer_x_min, outer_x_max);
  const float cut_x_max = std::clamp(
      hole_x_max, outer_x_min, outer_x_max);
  const float cut_z_min = std::clamp(
      hole_z_min, outer_z_min, outer_z_max);
  const float cut_z_max = std::clamp(
      hole_z_max, outer_z_min, outer_z_max);
  const auto add = [&](float x0, float x1, float z0, float z1) {
    if (x1 - x0 <= 1.0e-4f || z1 - z0 <= 1.0e-4f) {
      return;
    }
    map.AddQuad(ids.Take(), material,
                {x0, height, z0}, {x0, height, z1},
                {x1, height, z1}, {x1, height, z0},
                {std::max((x1 - x0) * 0.5f, 1.0f),
                 std::max((z1 - z0) * 0.5f, 1.0f)});
  };
  if (cut_x_min >= cut_x_max || cut_z_min >= cut_z_max) {
    add(outer_x_min, outer_x_max, outer_z_min, outer_z_max);
    return;
  }
  add(outer_x_min, cut_x_min, outer_z_min, outer_z_max);
  add(cut_x_max, outer_x_max, outer_z_min, outer_z_max);
  add(cut_x_min, cut_x_max, outer_z_min, cut_z_min);
  add(cut_x_min, cut_x_max, cut_z_max, outer_z_max);
}

}  // namespace

MapDefinition MakeStarterFlatgroundMap() {
  MapBuilder map("neon_foundry_day_cycle");
  SurfaceIds ids;

  const auto skateable = SurfaceFlags::Skateable;
  const auto grindable =
      SurfaceFlags::Skateable | SurfaceFlags::Grindable;
  const auto wall = SurfaceFlags::Wall;

  const MaterialId plaza_concrete = map.AddMaterial(
      "warm_plaza_concrete", 0.83f, 0.015f, skateable,
      {0.42f, 0.44f, 0.48f}, MaterialPattern::Concrete, 1.3f, 0.22f, 0.20f);
  const MaterialId pale_concrete = map.AddMaterial(
      "pale_formed_concrete", 0.84f, 0.012f, skateable,
      {0.62f, 0.64f, 0.67f}, MaterialPattern::Concrete, 0.85f, 0.34f, 0.14f);
  const MaterialId dark_concrete = map.AddMaterial(
      "dark_formed_concrete", 0.84f, 0.012f, skateable,
      {0.16f, 0.18f, 0.22f}, MaterialPattern::Concrete, 1.0f, 0.25f, 0.16f);
  const MaterialId asphalt = map.AddMaterial(
      "fine_city_asphalt", 0.80f, 0.01f, skateable,
      {0.10f, 0.12f, 0.15f}, MaterialPattern::Asphalt, 2.4f, 0.10f, 0.22f);
  const MaterialId brick = map.AddMaterial(
      "weathered_red_brick", 0.79f, 0.01f, wall,
      {0.48f, 0.20f, 0.13f}, MaterialPattern::Brick, 2.0f, 0.86f, 0.24f);
  const MaterialId cream_brick = map.AddMaterial(
      "cream_brick", 0.79f, 0.01f, wall,
      {0.64f, 0.55f, 0.42f}, MaterialPattern::Brick, 1.8f, 0.88f, 0.18f);
  const MaterialId blue_metal = map.AddMaterial(
      "powder_blue_metal", 0.72f, 0.025f, grindable,
      {0.08f, 0.36f, 0.52f}, MaterialPattern::Metal, 3.0f, 0.34f, 0.10f);
  const MaterialId black_metal = map.AddMaterial(
      "blackened_steel", 0.70f, 0.025f, grindable,
      {0.07f, 0.08f, 0.085f}, MaterialPattern::Metal, 4.0f, 0.25f, 0.12f);
  const MaterialId corten = map.AddMaterial(
      "corten_steel", 0.73f, 0.02f, grindable,
      {0.54f, 0.20f, 0.075f}, MaterialPattern::Metal, 2.0f, 0.50f, 0.30f);
  const MaterialId wood = map.AddMaterial(
      "oiled_skate_wood", 0.81f, 0.015f, grindable,
      {0.45f, 0.25f, 0.095f}, MaterialPattern::Wood, 1.5f, 0.62f, 0.22f);
  const MaterialId tile = map.AddMaterial(
      "cream_plaza_tile", 0.82f, 0.012f, skateable,
      {0.48f, 0.50f, 0.55f}, MaterialPattern::Tile, 1.25f, 0.14f, 0.10f);
  const MaterialId grass = map.AddMaterial(
      "planter_grass", 0.95f, 0.0f, SurfaceFlags::None,
      {0.10f, 0.31f, 0.15f}, MaterialPattern::Grass, 3.5f, 1.0f, 0.35f);
  const MaterialId painted_yellow = map.AddMaterial(
      "safety_yellow", 0.79f, 0.015f, grindable,
      {0.93f, 0.60f, 0.055f}, MaterialPattern::Painted, 1.0f, 0.55f, 0.12f);
  const MaterialId painted_teal = map.AddMaterial(
      "park_teal", 0.82f, 0.012f, skateable,
      {0.03f, 0.48f, 0.45f}, MaterialPattern::Painted, 1.0f, 0.60f, 0.10f);
  const MaterialId painted_red = map.AddMaterial(
      "park_coral", 0.82f, 0.012f, skateable,
      {0.78f, 0.12f, 0.075f}, MaterialPattern::Painted, 1.0f, 0.60f, 0.10f);
  const MaterialId glass = map.AddMaterial(
      "blue_window_glass", 0.70f, 0.01f, wall,
      {0.075f, 0.20f, 0.27f}, MaterialPattern::Tile, 0.65f, 0.16f, 0.08f);
  const MaterialId roof = map.AddMaterial(
      "graphite_roof", 0.78f, 0.01f, skateable,
      {0.11f, 0.12f, 0.13f}, MaterialPattern::Asphalt, 1.8f, 0.90f, 0.16f);
  const MaterialId lit_glass = map.AddMaterial(
      "cyan_lit_window_ribbon", 0.70f, 0.01f, wall,
      {0.035f, 0.34f, 0.58f}, MaterialPattern::Tile,
      0.72f, 0.08f, 0.04f, 1.45f);
  const MaterialId neon_cyan = map.AddMaterial(
      "neon_cyan_emission", 0.70f, 0.0f, wall,
      {0.015f, 0.78f, 1.0f}, MaterialPattern::Solid,
      1.0f, 0.05f, 0.0f, 5.4f);
  const MaterialId neon_magenta = map.AddMaterial(
      "neon_magenta_emission", 0.70f, 0.0f, wall,
      {1.0f, 0.025f, 0.52f}, MaterialPattern::Solid,
      1.0f, 0.05f, 0.0f, 5.8f);
  const MaterialId neon_violet = map.AddMaterial(
      "neon_violet_emission", 0.70f, 0.0f, wall,
      {0.44f, 0.055f, 1.0f}, MaterialPattern::Solid,
      1.0f, 0.05f, 0.0f, 5.2f);
  const MaterialId neon_amber = map.AddMaterial(
      "neon_amber_emission", 0.70f, 0.0f, wall,
      {1.0f, 0.30f, 0.025f}, MaterialPattern::Solid,
      1.0f, 0.05f, 0.0f, 4.8f);

  // Spawn position is the authored ground point. The recomp adapter aligns
  // this local point with the live board's native ground point.
  map.SetSpawn({-38.0f, 0.0f, -14.0f}, 0.0f);
  map.SetSky(
      {0.09f, 0.34f, 0.72f},
      {0.58f, 0.78f, 0.98f},
      {0.18f, 0.25f, 0.34f});
  map.SetDirectionalSun(
      {0.30f, 0.82f, 0.46f},
      {1.0f, 0.92f, 0.78f},
      1.25f, 0.32f);
  DayNightCycleDefinition day_night;
  day_night.enabled = true;
  day_night.duration_seconds = 96.0f;
  day_night.start_time_hours = 9.0f;
  day_night.orbit_azimuth_radians = 0.62f;
  map.SetDayNightCycle(std::move(day_night));

  constexpr float basin_x_min = -65.0f;
  constexpr float basin_x_max = -43.0f;
  constexpr float basin_z_min = -23.0f;
  constexpr float basin_z_max = -1.0f;
  AddGroundWithRectangularHole(
      map, ids, asphalt, -160.0f, 160.0f, -160.0f, 160.0f,
      0.0f, basin_x_min, basin_x_max, basin_z_min, basin_z_max);
  AddGroundWithRectangularHole(
      map, ids, plaza_concrete, -62.0f, 64.0f, -45.0f, 45.0f,
      0.02f, basin_x_min, basin_x_max, basin_z_min, basin_z_max);
  AddGroundWithRectangularHole(
      map, ids, tile, -48.0f, 48.0f, -9.0f, 9.0f,
      0.035f, basin_x_min, basin_x_max, basin_z_min, basin_z_max);
  map.AddQuad(ids.Take(), asphalt,
              {-160.0f, 0.045f, 48.0f}, {-160.0f, 0.045f, 68.0f},
              {160.0f, 0.045f, 68.0f}, {160.0f, 0.045f, 48.0f},
              {160.0f, 10.0f});

  AddBankX(map, ids, painted_teal, -22.0f, -13.0f, -7.0f, 7.0f, 0.0f, 1.35f);
  map.AddBox(ids.Take(), pale_concrete,
             {-13.0f, 0.0f, -7.0f}, {-5.0f, 1.35f, 7.0f});
  AddBankX(map, ids, painted_red, -5.0f, 4.0f, -7.0f, 7.0f, 1.35f, 0.0f);
  map.AddBox(ids.Take(), dark_concrete,
             {12.0f, 0.0f, -3.0f}, {21.0f, 0.42f, 3.0f});
  // Long, low rail with a clear runway for deterministic grind tests.
  AddRailX(map, ids, blue_metal, "spawn_practice_rail",
           -28.0f, -7.0f, -14.0f, 0.34f);

  // Bright, broad waist-high moving collision test in the north side plaza.
  // Keeping it off the initial skating/camera line preserves a clean spawn
  // while retaining the exact same deterministic ping-pong motion.
  map.AddKinematicBox(
      "spawn_shuttle_platform", ids.Take(), painted_yellow,
      {-2.2f, -0.10f, -2.2f}, {2.2f, 1.40f, 2.2f},
      {-42.0f, 0.0f, 26.0f}, {-30.0f, 0.0f, 26.0f}, 4.5f);

  // Recessed simulation basin immediately west of spawn. The broad static
  // ground sheets above were split around this rectangle, so this is an
  // actual hole with a physical floor and walls rather than water painted
  // over the skating surface.
  map.AddBox(ids.Take(), dark_concrete,
             {basin_x_min, -1.62f, basin_z_min},
             {basin_x_max, -1.50f, basin_z_max});
  map.AddBox(ids.Take(), pale_concrete,
             {basin_x_min, -1.50f, basin_z_min},
             {basin_x_min + 0.50f, 0.24f, basin_z_max});
  map.AddBox(ids.Take(), pale_concrete,
             {basin_x_max - 0.50f, -1.50f, basin_z_min},
             {basin_x_max, 0.24f, basin_z_max});
  map.AddBox(ids.Take(), pale_concrete,
             {basin_x_min + 0.50f, -1.50f, basin_z_min},
             {basin_x_max - 0.50f, 0.24f,
              basin_z_min + 0.50f});
  map.AddBox(ids.Take(), pale_concrete,
             {basin_x_min + 0.50f, -1.50f,
              basin_z_max - 0.50f},
             {basin_x_max - 0.50f, 0.24f, basin_z_max});
  map.AddBox(ids.Take(), blue_metal,
             {basin_x_min - 0.12f, 0.24f, basin_z_min - 0.12f},
             {basin_x_min + 0.62f, 0.36f, basin_z_max + 0.12f});
  map.AddBox(ids.Take(), blue_metal,
             {basin_x_max - 0.62f, 0.24f, basin_z_min - 0.12f},
             {basin_x_max + 0.12f, 0.36f, basin_z_max + 0.12f});
  map.AddBox(ids.Take(), blue_metal,
             {basin_x_min + 0.50f, 0.24f, basin_z_min - 0.12f},
             {basin_x_max - 0.50f, 0.36f,
              basin_z_min + 0.62f});
  map.AddBox(ids.Take(), blue_metal,
             {basin_x_min + 0.50f, 0.24f,
              basin_z_max - 0.62f},
             {basin_x_max - 0.50f, 0.36f, basin_z_max + 0.12f});

  WaterBasin basin;
  basin.name = "west_wave_basin";
  basin.minimum = {
      basin_x_min + 0.52f, -1.50f, basin_z_min + 0.52f};
  basin.maximum = {
      basin_x_max - 0.52f, 0.50f, basin_z_max - 0.52f};
  basin.rest_surface_height = -0.28f;
  basin.columns = 57;
  basin.rows = 57;
  basin.damping = 0.30f;
  basin.pusher_local_min = {-0.42f, -1.42f, -3.4f};
  basin.pusher_local_max = {0.42f, 0.38f, 3.4f};
  basin.pusher_path_start = {-60.5f, 0.0f, -12.0f};
  basin.pusher_path_end = {-47.5f, 0.0f, -12.0f};
  basin.pusher_travel_seconds = 5.0f;
  basin.pusher_material = painted_yellow;
  map.AddWaterBasin(std::move(basin));

  // Hardware-raytraced mirror immediately south of spawn. The reflective
  // rectangle is renderer-neutral authored data; the surrounding steel and
  // backing remain ordinary visible/collision map geometry.
  constexpr float mirror_x = -14.0f;
  constexpr float mirror_z_min = -34.0f;
  constexpr float mirror_z_max = -22.0f;
  constexpr float mirror_y_min = 0.30f;
  constexpr float mirror_y_max = 6.30f;
  map.AddBox(ids.Take(), dark_concrete,
             {mirror_x + 0.12f, 0.08f, mirror_z_min - 0.28f},
             {mirror_x + 0.34f, 6.52f, mirror_z_max + 0.28f});
  map.AddBox(ids.Take(), black_metal,
             {mirror_x - 0.15f, 0.08f, mirror_z_min - 0.30f},
             {mirror_x + 0.20f, 6.52f, mirror_z_min + 0.05f});
  map.AddBox(ids.Take(), black_metal,
             {mirror_x - 0.15f, 0.08f, mirror_z_max - 0.05f},
             {mirror_x + 0.20f, 6.52f, mirror_z_max + 0.30f});
  map.AddBox(ids.Take(), black_metal,
             {mirror_x - 0.15f, 0.08f, mirror_z_min - 0.30f},
             {mirror_x + 0.20f, mirror_y_min + 0.15f,
              mirror_z_max + 0.30f});
  map.AddBox(ids.Take(), black_metal,
             {mirror_x - 0.15f, mirror_y_max - 0.15f,
              mirror_z_min - 0.30f},
             {mirror_x + 0.20f, 6.52f, mirror_z_max + 0.30f});
  RaytracedMirror mirror;
  mirror.name = "spawn_industrial_mirror";
  mirror.center = {
      mirror_x,
      (mirror_y_min + mirror_y_max) * 0.5f,
      (mirror_z_min + mirror_z_max) * 0.5f,
  };
  mirror.right = {0.0f, 0.0f, 1.0f};
  mirror.up = {0.0f, 1.0f, 0.0f};
  mirror.half_width = (mirror_z_max - mirror_z_min) * 0.5f;
  mirror.half_height = (mirror_y_max - mirror_y_min) * 0.5f;
  map.AddRaytracedMirror(std::move(mirror));

  // Wet patches around the mirror court. They do not alter authoritative
  // collision; their DXR footprints sit six centimetres above the authored
  // ground plane to avoid depth fighting.
  RaytracedPuddle puddle;
  puddle.name = "mirror_court_wide_puddle";
  puddle.center = {-29.0f, 0.061f, -28.0f};
  puddle.half_width = 15.5f;
  puddle.half_length = 7.2f;
  puddle.reflectivity = 0.72f;
  puddle.ripple_strength = 0.016f;
  map.AddRaytracedPuddle(std::move(puddle));

  puddle = {};
  puddle.name = "spawn_runway_puddle";
  puddle.center = {-33.0f, 0.061f, -11.8f};
  puddle.right = {0.98f, 0.0f, 0.20f};
  puddle.forward = {0.20f, 0.0f, -0.98f};
  puddle.half_width = 14.0f;
  puddle.half_length = 6.4f;
  puddle.reflectivity = 0.68f;
  puddle.ripple_strength = 0.014f;
  map.AddRaytracedPuddle(std::move(puddle));

  puddle = {};
  puddle.name = "rail_side_puddle";
  puddle.center = {7.0f, 0.061f, 9.5f};
  puddle.right = {0.995f, 0.0f, -0.10f};
  puddle.forward = {-0.10f, 0.0f, -0.995f};
  puddle.half_width = 27.0f;
  puddle.half_length = 11.0f;
  puddle.reflectivity = 0.64f;
  puddle.ripple_strength = 0.012f;
  map.AddRaytracedPuddle(std::move(puddle));

  // Three independently moving spherical area lights turn the mirror court
  // into a compact night-lighting showcase. Their trajectories, colours,
  // radii, and physical influence are map data rather than shader animation.
  MovingLightOrb cyan;
  cyan.name = "mirror_cyan_orb";
  cyan.orbit_center = {-20.2f, 3.35f, -31.4f};
  cyan.orbit_axis_u = {2.0f, 0.0f, 0.0f};
  cyan.orbit_axis_v = {0.0f, 1.25f, 1.65f};
  cyan.color = {0.04f, 0.78f, 1.0f};
  cyan.source_radius = 0.38f;
  cyan.influence_radius = 23.0f;
  cyan.intensity = 8.4f;
  cyan.period_seconds = 6.0f;
  map.AddMovingLightOrb(std::move(cyan));

  MovingLightOrb magenta;
  magenta.name = "mirror_magenta_orb";
  magenta.orbit_center = {-20.0f, 4.15f, -27.9f};
  magenta.orbit_axis_u = {2.35f, 0.0f, 0.0f};
  magenta.orbit_axis_v = {0.0f, 1.05f, 1.90f};
  magenta.color = {1.0f, 0.06f, 0.62f};
  magenta.source_radius = 0.42f;
  magenta.influence_radius = 25.0f;
  magenta.intensity = 9.2f;
  magenta.period_seconds = 7.6f;
  magenta.phase_radians = 2.1f;
  map.AddMovingLightOrb(std::move(magenta));

  MovingLightOrb amber;
  amber.name = "mirror_amber_orb";
  amber.orbit_center = {-20.4f, 2.65f, -24.4f};
  amber.orbit_axis_u = {1.75f, 0.0f, 0.0f};
  amber.orbit_axis_v = {0.0f, 0.90f, 1.45f};
  amber.color = {1.0f, 0.42f, 0.055f};
  amber.source_radius = 0.34f;
  amber.influence_radius = 21.0f;
  amber.intensity = 7.6f;
  amber.period_seconds = 5.2f;
  amber.phase_radians = 4.0f;
  map.AddMovingLightOrb(std::move(amber));

  AddRailX(map, ids, black_metal, "manual_pad_south_rail",
           11.4f, 21.6f, -3.22f, 0.50f);
  AddRailX(map, ids, black_metal, "manual_pad_north_rail",
           11.4f, 21.6f, 3.22f, 0.50f);

  AddStairSetX(map, ids, pale_concrete,
               28.0f, 13.0f, 9.0f, 0.85f, 0.18f, 6, true);
  map.AddBox(ids.Take(), pale_concrete,
             {33.1f, 0.0f, 13.0f}, {45.0f, 1.08f, 22.0f});
  map.AddBox(ids.Take(), corten,
             {30.0f, 0.0f, 17.15f}, {42.5f, 0.62f, 17.85f});
  AddRailX(map, ids, blue_metal, "stair_entry_rail",
           28.3f, 33.0f, 12.65f, 0.58f);

  map.AddBox(ids.Take(), dark_concrete,
             {-20.0f, 0.0f, 24.0f}, {-3.0f, 0.58f, 37.0f});
  map.AddBox(ids.Take(), grass,
             {-18.6f, 0.58f, 25.4f}, {-4.4f, 0.82f, 35.6f});
  AddBankZ(map, ids, plaza_concrete,
           -25.0f, -20.0f, 24.0f, 37.0f, 0.0f, 0.58f);
  AddBankZ(map, ids, plaza_concrete,
           -3.0f, 2.0f, 24.0f, 37.0f, 0.58f, 0.0f);

  AddQuarterPipeX(map, ids, wood, painted_yellow,
                  -52.0f, -40.0f, -38.0f, -24.0f, 3.4f, false);
  AddQuarterPipeX(map, ids, wood, painted_yellow,
                  42.0f, 54.0f, -38.0f, -24.0f, 3.4f, true);
  AddBankX(map, ids, painted_red,
           -8.0f, 0.0f, -35.0f, -23.0f, 0.0f, 1.25f);
  AddBankX(map, ids, painted_teal,
           0.0f, 8.0f, -35.0f, -23.0f, 1.25f, 0.0f);
  AddRailZ(map, ids, black_metal, "pyramid_west_rail",
           -12.0f, -39.0f, -25.0f, 0.62f);
  AddRailZ(map, ids, black_metal, "pyramid_east_rail",
           12.0f, -39.0f, -25.0f, 0.62f);

  map.AddBox(ids.Take(), brick,
             {72.0f, 0.0f, -34.0f}, {111.0f, 2.2f, -12.0f});
  AddBankX(map, ids, pale_concrete,
           62.0f, 72.0f, -32.0f, -14.0f, 0.0f, 2.2f);
  map.AddBox(ids.Take(), painted_yellow,
             {76.0f, 2.2f, -34.3f}, {93.0f, 2.55f, -33.8f});
  AddStairSetX(map, ids, pale_concrete,
               101.0f, -11.0f, 7.0f, 1.0f, 0.22f, 8, false);
  AddRailX(map, ids, black_metal, "loading_stair_rail",
           93.0f, 101.0f, -11.35f, 1.0f);

  AddBench(map, ids, wood, black_metal, -34.0f, 15.0f, true);
  AddBench(map, ids, wood, black_metal, -34.0f, 20.0f, true);
  AddBench(map, ids, wood, black_metal, 8.0f, 35.0f, false);
  map.AddBox(ids.Take(), corten,
             {18.0f, 0.0f, 27.0f}, {27.0f, 0.62f, 39.0f});
  map.AddBox(ids.Take(), grass,
             {18.7f, 0.62f, 27.7f}, {26.3f, 0.82f, 38.3f});

  // Mid-distance cyberpunk skyline. These volumes sit outside the main
  // skating plaza but close enough for their emissive strips and windows to
  // read directly and in the wet-ground reflection field.
  AddCyberTower(map, ids, dark_concrete, roof, lit_glass, neon_cyan,
                {-88.0f, 0.0f, 68.0f}, {-67.0f, 46.0f, 88.0f}, 10);
  AddCyberTower(map, ids, black_metal, roof, lit_glass, neon_magenta,
                {-29.0f, 0.0f, 70.0f}, {-6.0f, 61.0f, 93.0f}, 13);
  AddCyberTower(map, ids, dark_concrete, roof, lit_glass, neon_violet,
                {19.0f, 0.0f, 69.0f}, {43.0f, 42.0f, 91.0f}, 9);
  AddCyberTower(map, ids, black_metal, roof, lit_glass, neon_amber,
                {69.0f, 0.0f, 18.0f}, {91.0f, 49.0f, 44.0f}, 10);
  AddCyberTower(map, ids, dark_concrete, roof, lit_glass, neon_cyan,
                {-94.0f, 0.0f, 8.0f}, {-72.0f, 39.0f, 35.0f}, 8);
  AddCyberTower(map, ids, black_metal, roof, lit_glass, neon_magenta,
                {-34.0f, 0.0f, -88.0f}, {-10.0f, 55.0f, -67.0f}, 12);
  AddCyberTower(map, ids, dark_concrete, roof, lit_glass, neon_violet,
                {42.0f, 0.0f, -87.0f}, {66.0f, 43.0f, -64.0f}, 9);

  AddNeonBillboard(map, ids, black_metal, neon_magenta, neon_cyan,
                   -48.0f, -34.0f, 44.2f, 3.2f, 8.5f);
  AddNeonBillboard(map, ids, black_metal, neon_violet, neon_amber,
                   18.0f, 34.0f, 44.0f, 3.8f, 9.6f);
  AddNeonBillboard(map, ids, black_metal, neon_cyan, neon_magenta,
                   -8.0f, 8.0f, -46.5f, 4.0f, 10.5f);

  AddBuilding(map, ids, brick, roof, glass,
              {-145.0f, 0.0f, 82.0f}, {-92.0f, 18.0f, 136.0f}, 5);
  AddBuilding(map, ids, cream_brick, roof, glass,
              {-78.0f, 0.0f, 91.0f}, {-34.0f, 27.0f, 143.0f}, 7);
  AddBuilding(map, ids, brick, roof, glass,
              {-19.0f, 0.0f, 96.0f}, {34.0f, 14.0f, 146.0f}, 4);
  AddBuilding(map, ids, cream_brick, roof, glass,
              {52.0f, 0.0f, 88.0f}, {101.0f, 34.0f, 141.0f}, 8);
  AddBuilding(map, ids, brick, roof, glass,
              {115.0f, 0.0f, 78.0f}, {151.0f, 22.0f, 137.0f}, 6);
  AddBuilding(map, ids, cream_brick, roof, glass,
              {-151.0f, 0.0f, -146.0f}, {-108.0f, 30.0f, -91.0f}, 8);
  AddBuilding(map, ids, brick, roof, glass,
              {-91.0f, 0.0f, -151.0f}, {-39.0f, 17.0f, -103.0f}, 5);
  AddBuilding(map, ids, cream_brick, roof, glass,
              {34.0f, 0.0f, -148.0f}, {79.0f, 25.0f, -98.0f}, 7);
  AddBuilding(map, ids, brick, roof, glass,
              {96.0f, 0.0f, -151.0f}, {149.0f, 38.0f, -89.0f}, 9);

  for (int lane = -4; lane <= 4; ++lane) {
    const float x = lane * 28.0f;
    map.AddBox(ids.Take(), painted_yellow,
               {x - 4.5f, 0.046f, 57.6f},
               {x + 4.5f, 0.075f, 58.0f});
  }
  for (float x : {-56.0f, -28.0f, 0.0f, 28.0f, 56.0f}) {
    map.AddBox(ids.Take(), black_metal,
               {x - 0.08f, 0.0f, 43.5f},
               {x + 0.08f, 2.4f, 43.66f});
    map.AddBox(ids.Take(), blue_metal,
               {x - 0.38f, 2.3f, 43.36f},
               {x + 0.38f, 2.48f, 43.80f});
  }

  return std::move(map).Build();
}

}  // namespace skate::world
