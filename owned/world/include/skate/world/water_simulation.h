#pragma once

#include "skate/world/math.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace skate::world {

// A vertical solid projected into a shallow-water heightfield. The no-flow
// boundary follows the obstacle velocity, so translating the solid transfers
// momentum into the fluid instead of merely adding a cosmetic wave.
struct WaterObstacle {
  Vec2 center;
  Vec2 half_extents;
  Vec2 velocity;
};

struct ShallowWaterConfig {
  Vec2 minimum;
  Vec2 maximum;
  std::uint32_t columns = 49;
  std::uint32_t rows = 49;
  float rest_surface_height = 0.0f;
  float rest_depth = 1.0f;
  float gravity = 9.81f;
  float linear_damping = 0.34f;
  float maximum_displacement = 0.55f;
};

struct WaterStatistics {
  float minimum_displacement = 0.0f;
  float maximum_displacement = 0.0f;
  float mean_displacement = 0.0f;
  float kinetic_energy = 0.0f;
  std::size_t solid_samples = 0;
};

// CPU reference implementation of a staggered-grid linear shallow-water
// solver. Height is cell-centred; horizontal velocity lives on cell faces.
// Closed basin and moving-solid boundaries are enforced before continuity is
// integrated. This is intentionally renderer-independent.
class ShallowWaterSimulation {
 public:
  explicit ShallowWaterSimulation(ShallowWaterConfig config);

  const ShallowWaterConfig& Config() const { return config_; }
  std::uint32_t Columns() const { return config_.columns; }
  std::uint32_t Rows() const { return config_.rows; }
  float CellSizeX() const { return cell_size_x_; }
  float CellSizeZ() const { return cell_size_z_; }

  void Reset();

  // Returns false without changing state when dt or the obstacle is invalid,
  // or when dt violates the solver's conservative CFL bound.
  bool Step(float dt, const WaterObstacle& obstacle);

  float SurfaceHeight(std::uint32_t column, std::uint32_t row) const;
  Vec3 SurfaceNormal(std::uint32_t column, std::uint32_t row) const;
  // Render-only temporal sampling between the two most recently completed
  // fixed simulation states. Alpha 0 selects the previous state and alpha 1
  // selects the current state; values outside that range are clamped.
  float InterpolatedSurfaceHeight(std::uint32_t column,
                                  std::uint32_t row,
                                  float alpha) const;
  Vec3 InterpolatedSurfaceNormal(std::uint32_t column,
                                 std::uint32_t row,
                                 float alpha) const;
  bool Solid(std::uint32_t column, std::uint32_t row) const;
  float ObstacleCoverage(std::uint32_t column,
                         std::uint32_t row) const;
  Vec2 SamplePosition(std::uint32_t column, std::uint32_t row) const;
  WaterStatistics Statistics() const;

 private:
  std::size_t CellIndex(std::uint32_t column,
                        std::uint32_t row) const;
  std::size_t UIndex(std::uint32_t face,
                     std::uint32_t row) const;
  std::size_t VIndex(std::uint32_t column,
                     std::uint32_t face) const;
  bool ValidObstacle(const WaterObstacle& obstacle) const;
  void UpdateObstacleCoverage(const WaterObstacle& obstacle);
  void RemoveMeanHeightDrift();

  ShallowWaterConfig config_;
  float cell_size_x_ = 0.0f;
  float cell_size_z_ = 0.0f;
  std::vector<float> previous_height_;
  std::vector<float> height_;
  std::vector<float> next_height_;
  std::vector<float> velocity_x_;
  std::vector<float> velocity_z_;
  std::vector<float> obstacle_coverage_;
};

}  // namespace skate::world
