#include "skate/world/water_simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace skate::world {
namespace {

bool Finite(float value) {
  return std::isfinite(value);
}

bool Finite(Vec2 value) {
  return Finite(value.x) && Finite(value.y);
}

}  // namespace

ShallowWaterSimulation::ShallowWaterSimulation(
    ShallowWaterConfig config)
    : config_(config) {
  if (!Finite(config_.minimum) || !Finite(config_.maximum) ||
      config_.maximum.x <= config_.minimum.x ||
      config_.maximum.y <= config_.minimum.y ||
      config_.columns < 3 || config_.rows < 3 ||
      config_.columns > 512 || config_.rows > 512 ||
      !Finite(config_.rest_surface_height) ||
      !Finite(config_.rest_depth) || config_.rest_depth <= 0.0f ||
      !Finite(config_.gravity) || config_.gravity <= 0.0f ||
      !Finite(config_.linear_damping) ||
      config_.linear_damping < 0.0f ||
      !Finite(config_.maximum_displacement) ||
      config_.maximum_displacement <= 0.0f) {
    throw std::invalid_argument("invalid shallow-water configuration");
  }

  cell_size_x_ =
      (config_.maximum.x - config_.minimum.x) /
      static_cast<float>(config_.columns - 1);
  cell_size_z_ =
      (config_.maximum.y - config_.minimum.y) /
      static_cast<float>(config_.rows - 1);
  const std::size_t cells =
      static_cast<std::size_t>(config_.columns) * config_.rows;
  previous_height_.resize(cells);
  height_.resize(cells);
  next_height_.resize(cells);
  obstacle_coverage_.resize(cells);
  velocity_x_.resize(
      static_cast<std::size_t>(config_.columns + 1) * config_.rows);
  velocity_z_.resize(
      static_cast<std::size_t>(config_.columns) *
      (config_.rows + 1));
  Reset();
}

void ShallowWaterSimulation::Reset() {
  std::fill(previous_height_.begin(), previous_height_.end(), 0.0f);
  std::fill(height_.begin(), height_.end(), 0.0f);
  std::fill(next_height_.begin(), next_height_.end(), 0.0f);
  std::fill(velocity_x_.begin(), velocity_x_.end(), 0.0f);
  std::fill(velocity_z_.begin(), velocity_z_.end(), 0.0f);
  std::fill(obstacle_coverage_.begin(),
            obstacle_coverage_.end(), 0.0f);
}

bool ShallowWaterSimulation::Step(
    float dt, const WaterObstacle& obstacle) {
  if (!Finite(dt) || dt <= 0.0f || !ValidObstacle(obstacle)) {
    return false;
  }
  const float wave_speed =
      std::sqrt(config_.gravity * config_.rest_depth);
  const float minimum_cell =
      std::min(cell_size_x_, cell_size_z_);
  const float maximum_dt =
      0.45f * minimum_cell / (wave_speed * std::sqrt(2.0f));
  if (dt > maximum_dt) {
    return false;
  }

  // Preserve the last committed state for render interpolation. Simulation
  // itself always reads and writes the authoritative current state.
  previous_height_ = height_;
  UpdateObstacleCoverage(obstacle);
  const float damping =
      std::exp(-config_.linear_damping * dt);

  // X-face momentum. Face i separates cells i-1 and i. Closed basin
  // boundaries remain zero; a solid/fluid face follows the solid velocity.
  for (std::uint32_t row = 0; row < config_.rows; ++row) {
    velocity_x_[UIndex(0, row)] = 0.0f;
    velocity_x_[UIndex(config_.columns, row)] = 0.0f;
    for (std::uint32_t face = 1;
         face < config_.columns; ++face) {
      const float obstacle_weight = std::max(
          ObstacleCoverage(face - 1, row),
          ObstacleCoverage(face, row));
      float& velocity = velocity_x_[UIndex(face, row)];
      const float gradient =
          (height_[CellIndex(face, row)] -
           height_[CellIndex(face - 1, row)]) /
          cell_size_x_;
      const float fluid_velocity =
          (velocity - config_.gravity * dt * gradient) * damping;
      velocity = fluid_velocity +
                 (obstacle.velocity.x - fluid_velocity) *
                     obstacle_weight;
    }
  }

  // Z-face momentum.
  for (std::uint32_t column = 0;
       column < config_.columns; ++column) {
    velocity_z_[VIndex(column, 0)] = 0.0f;
    velocity_z_[VIndex(column, config_.rows)] = 0.0f;
    for (std::uint32_t face = 1;
         face < config_.rows; ++face) {
      const float obstacle_weight = std::max(
          ObstacleCoverage(column, face - 1),
          ObstacleCoverage(column, face));
      float& velocity = velocity_z_[VIndex(column, face)];
      const float gradient =
          (height_[CellIndex(column, face)] -
           height_[CellIndex(column, face - 1)]) /
          cell_size_z_;
      const float fluid_velocity =
          (velocity - config_.gravity * dt * gradient) * damping;
      velocity = fluid_velocity +
                 (obstacle.velocity.y - fluid_velocity) *
                     obstacle_weight;
    }
  }

  next_height_ = height_;
  for (std::uint32_t row = 0; row < config_.rows; ++row) {
    for (std::uint32_t column = 0;
         column < config_.columns; ++column) {
      const std::size_t index = CellIndex(column, row);
      const float fluid_fraction =
          1.0f - obstacle_coverage_[index];
      if (fluid_fraction <= 0.001f) {
        next_height_[index] = 0.0f;
        continue;
      }
      const float divergence =
          (velocity_x_[UIndex(column + 1, row)] -
           velocity_x_[UIndex(column, row)]) /
              cell_size_x_ +
          (velocity_z_[VIndex(column, row + 1)] -
           velocity_z_[VIndex(column, row)]) /
              cell_size_z_;
      next_height_[index] = std::clamp(
          height_[index] -
              config_.rest_depth * dt * divergence *
                  fluid_fraction,
          -config_.maximum_displacement,
          config_.maximum_displacement);
    }
  }
  height_.swap(next_height_);
  RemoveMeanHeightDrift();
  return true;
}

float ShallowWaterSimulation::SurfaceHeight(
    std::uint32_t column, std::uint32_t row) const {
  if (column >= config_.columns || row >= config_.rows) {
    throw std::out_of_range("water sample is out of range");
  }
  return config_.rest_surface_height +
         height_[CellIndex(column, row)];
}

Vec3 ShallowWaterSimulation::SurfaceNormal(
    std::uint32_t column, std::uint32_t row) const {
  if (column >= config_.columns || row >= config_.rows) {
    throw std::out_of_range("water sample is out of range");
  }
  const std::uint32_t left = column == 0 ? column : column - 1;
  const std::uint32_t right =
      std::min(column + 1, config_.columns - 1);
  const std::uint32_t near_row = row == 0 ? row : row - 1;
  const std::uint32_t far_row =
      std::min(row + 1, config_.rows - 1);
  const float span_x =
      static_cast<float>(right - left) * cell_size_x_;
  const float span_z =
      static_cast<float>(far_row - near_row) * cell_size_z_;
  const float slope_x =
      span_x > 0.0f
          ? (height_[CellIndex(right, row)] -
             height_[CellIndex(left, row)]) /
                span_x
          : 0.0f;
  const float slope_z =
      span_z > 0.0f
          ? (height_[CellIndex(column, far_row)] -
             height_[CellIndex(column, near_row)]) /
                span_z
          : 0.0f;
  return Normalize({-slope_x, 1.0f, -slope_z});
}

float ShallowWaterSimulation::InterpolatedSurfaceHeight(
    std::uint32_t column, std::uint32_t row, float alpha) const {
  if (column >= config_.columns || row >= config_.rows) {
    throw std::out_of_range("water sample is out of range");
  }
  if (!Finite(alpha)) {
    throw std::invalid_argument("water interpolation alpha is not finite");
  }
  const float blend = std::clamp(alpha, 0.0f, 1.0f);
  const std::size_t index = CellIndex(column, row);
  return config_.rest_surface_height +
         previous_height_[index] +
         (height_[index] - previous_height_[index]) * blend;
}

Vec3 ShallowWaterSimulation::InterpolatedSurfaceNormal(
    std::uint32_t column, std::uint32_t row, float alpha) const {
  if (column >= config_.columns || row >= config_.rows) {
    throw std::out_of_range("water sample is out of range");
  }
  if (!Finite(alpha)) {
    throw std::invalid_argument("water interpolation alpha is not finite");
  }
  const float blend = std::clamp(alpha, 0.0f, 1.0f);
  const auto displacement = [&](std::uint32_t sample_column,
                                std::uint32_t sample_row) {
    const std::size_t index = CellIndex(sample_column, sample_row);
    return previous_height_[index] +
           (height_[index] - previous_height_[index]) * blend;
  };
  const std::uint32_t left = column == 0 ? column : column - 1;
  const std::uint32_t right =
      std::min(column + 1, config_.columns - 1);
  const std::uint32_t near_row = row == 0 ? row : row - 1;
  const std::uint32_t far_row =
      std::min(row + 1, config_.rows - 1);
  const float span_x =
      static_cast<float>(right - left) * cell_size_x_;
  const float span_z =
      static_cast<float>(far_row - near_row) * cell_size_z_;
  const float slope_x =
      span_x > 0.0f
          ? (displacement(right, row) -
             displacement(left, row)) /
                span_x
          : 0.0f;
  const float slope_z =
      span_z > 0.0f
          ? (displacement(column, far_row) -
             displacement(column, near_row)) /
                span_z
          : 0.0f;
  return Normalize({-slope_x, 1.0f, -slope_z});
}

bool ShallowWaterSimulation::Solid(
    std::uint32_t column, std::uint32_t row) const {
  if (column >= config_.columns || row >= config_.rows) {
    return true;
  }
  return ObstacleCoverage(column, row) >= 0.999f;
}

float ShallowWaterSimulation::ObstacleCoverage(
    std::uint32_t column, std::uint32_t row) const {
  if (column >= config_.columns || row >= config_.rows) {
    return 1.0f;
  }
  return obstacle_coverage_[CellIndex(column, row)];
}

Vec2 ShallowWaterSimulation::SamplePosition(
    std::uint32_t column, std::uint32_t row) const {
  if (column >= config_.columns || row >= config_.rows) {
    throw std::out_of_range("water sample is out of range");
  }
  return {
      config_.minimum.x + static_cast<float>(column) * cell_size_x_,
      config_.minimum.y + static_cast<float>(row) * cell_size_z_,
  };
}

WaterStatistics ShallowWaterSimulation::Statistics() const {
  WaterStatistics statistics;
  statistics.minimum_displacement =
      std::numeric_limits<float>::infinity();
  statistics.maximum_displacement =
      -std::numeric_limits<float>::infinity();
  double sum = 0.0;
  double fluid_weight = 0.0;
  for (std::size_t index = 0; index < height_.size(); ++index) {
    if (obstacle_coverage_[index] >= 0.5f) {
      ++statistics.solid_samples;
    }
    const float fluid_fraction =
        1.0f - obstacle_coverage_[index];
    if (fluid_fraction <= 0.001f) continue;
    statistics.minimum_displacement =
        std::min(statistics.minimum_displacement, height_[index]);
    statistics.maximum_displacement =
        std::max(statistics.maximum_displacement, height_[index]);
    sum += height_[index] * fluid_fraction;
    fluid_weight += fluid_fraction;
  }
  if (fluid_weight <= 0.0) {
    statistics.minimum_displacement = 0.0f;
    statistics.maximum_displacement = 0.0f;
  } else {
    statistics.mean_displacement =
        static_cast<float>(sum / fluid_weight);
  }
  for (float velocity : velocity_x_) {
    statistics.kinetic_energy += velocity * velocity;
  }
  for (float velocity : velocity_z_) {
    statistics.kinetic_energy += velocity * velocity;
  }
  statistics.kinetic_energy *= 0.5f;
  return statistics;
}

std::size_t ShallowWaterSimulation::CellIndex(
    std::uint32_t column, std::uint32_t row) const {
  return static_cast<std::size_t>(row) * config_.columns + column;
}

std::size_t ShallowWaterSimulation::UIndex(
    std::uint32_t face, std::uint32_t row) const {
  return static_cast<std::size_t>(row) *
             (config_.columns + 1) +
         face;
}

std::size_t ShallowWaterSimulation::VIndex(
    std::uint32_t column, std::uint32_t face) const {
  return static_cast<std::size_t>(face) * config_.columns + column;
}

bool ShallowWaterSimulation::ValidObstacle(
    const WaterObstacle& obstacle) const {
  return Finite(obstacle.center) && Finite(obstacle.half_extents) &&
         Finite(obstacle.velocity) &&
         obstacle.half_extents.x > 0.0f &&
         obstacle.half_extents.y > 0.0f;
}

void ShallowWaterSimulation::UpdateObstacleCoverage(
    const WaterObstacle& obstacle) {
  const float obstacle_min_x =
      obstacle.center.x - obstacle.half_extents.x;
  const float obstacle_max_x =
      obstacle.center.x + obstacle.half_extents.x;
  const float obstacle_min_z =
      obstacle.center.y - obstacle.half_extents.y;
  const float obstacle_max_z =
      obstacle.center.y + obstacle.half_extents.y;
  const float inverse_sample_area =
      1.0f / (cell_size_x_ * cell_size_z_);
  for (std::uint32_t row = 0; row < config_.rows; ++row) {
    for (std::uint32_t column = 0;
         column < config_.columns; ++column) {
      const Vec2 position = SamplePosition(column, row);
      const std::size_t index = CellIndex(column, row);
      const float sample_min_x = position.x - cell_size_x_ * 0.5f;
      const float sample_max_x = position.x + cell_size_x_ * 0.5f;
      const float sample_min_z = position.y - cell_size_z_ * 0.5f;
      const float sample_max_z = position.y + cell_size_z_ * 0.5f;
      const float overlap_x = std::max(
          0.0f, std::min(sample_max_x, obstacle_max_x) -
                    std::max(sample_min_x, obstacle_min_x));
      const float overlap_z = std::max(
          0.0f, std::min(sample_max_z, obstacle_max_z) -
                    std::max(sample_min_z, obstacle_min_z));
      obstacle_coverage_[index] = std::clamp(
          overlap_x * overlap_z * inverse_sample_area, 0.0f, 1.0f);
      if (obstacle_coverage_[index] >= 0.999f) {
        height_[index] = 0.0f;
      }
    }
  }
}

void ShallowWaterSimulation::RemoveMeanHeightDrift() {
  double sum = 0.0;
  double weight = 0.0;
  for (std::size_t index = 0; index < height_.size(); ++index) {
    const float fluid_fraction =
        1.0f - obstacle_coverage_[index];
    if (fluid_fraction > 0.001f) {
      sum += height_[index] * fluid_fraction;
      weight += fluid_fraction;
    }
  }
  if (weight <= 0.0) {
    return;
  }
  const float mean = static_cast<float>(sum / weight);
  for (std::size_t index = 0; index < height_.size(); ++index) {
    if (obstacle_coverage_[index] < 0.999f) {
      height_[index] = std::clamp(
          height_[index] - mean,
          -config_.maximum_displacement,
          config_.maximum_displacement);
    }
  }
}

}  // namespace skate::world
