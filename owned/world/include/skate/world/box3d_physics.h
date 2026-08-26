#pragma once

#include "skate/world/world_map.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace skate::world {

// SKATE packages and Box3D both use right-handed, Y-up metres. Keeping the
// conversion explicit protects this boundary if either representation changes.
inline constexpr float kSkateMetresPerBox3DUnit = 1.0f;
inline constexpr float kBox3DFixedTimeStepSeconds = 1.0f / 60.0f;
inline constexpr int kBox3DSolverSubSteps = 4;

struct PhysicsObjectPose {
  bool valid = false;
  std::uint64_t revision = 0;
  Vec3 position;
  Vec3 x_axis{1.0f, 0.0f, 0.0f};
  Vec3 y_axis{0.0f, 1.0f, 0.0f};
  Vec3 z_axis{0.0f, 0.0f, 1.0f};
  Vec3 linear_velocity;
  Vec3 angular_velocity;
  bool awake = false;
};

struct PhysicsTelemetry {
  std::uint64_t world_generation = 0;
  std::uint64_t world_steps = 0;
  std::uint64_t dropped_step_batches = 0;
  std::uint64_t transform_updates = 0;
  std::size_t static_body_count = 0;
  std::size_t dynamic_body_count = 0;
  std::size_t contact_count = 0;
  std::size_t sleeping_body_count = 0;
  std::size_t player_contact_count = 0;
  std::uint64_t player_proxy_updates = 0;
  bool player_proxy_active = false;
  std::size_t breakable_body_count = 0;
  std::size_t broken_group_count = 0;
  std::uint64_t glass_break_events = 0;
  std::uint32_t last_broken_group = 0;
  float last_break_speed = 0.0f;
  double accumulator_seconds = 0.0;
  PhysicsObjectPose representative_dynamic_pose;
};

// Deterministic Box3D ownership for one loaded MapDefinition. Bodies are
// created in map-object order and destroyed in reverse order. Reload tears
// down the previous world before creating the replacement.
class OwnedPhysicsWorld {
 public:
  OwnedPhysicsWorld();
  ~OwnedPhysicsWorld();

  OwnedPhysicsWorld(const OwnedPhysicsWorld&) = delete;
  OwnedPhysicsWorld& operator=(const OwnedPhysicsWorld&) = delete;
  OwnedPhysicsWorld(OwnedPhysicsWorld&&) noexcept;
  OwnedPhysicsWorld& operator=(OwnedPhysicsWorld&&) noexcept;

  void Load(const MapDefinition& map);
  // Adds newly appended map objects without rebuilding existing bodies. The
  // first index must equal the current body-record count.
  void AppendBodies(const MapDefinition& map, std::size_t first_object_index);
  void Reset() noexcept;

  // Advances a fixed 60 Hz simulation through a bounded accumulator. Invalid
  // or negative frame deltas are ignored; long stalls are capped.
  void Step(double frame_seconds);

  // Mirrors Skate's native player into Box3D as a kinematic capsule. This
  // gives dynamic owned objects reciprocal contact without replacing the
  // retail skater/board controller.
  void SetPlayerProxy(Vec3 position, Vec3 linear_velocity, bool active);

  [[nodiscard]] bool IsLoaded() const noexcept;
  [[nodiscard]] bool HasBody(std::size_t object_index) const noexcept;
  [[nodiscard]] PhysicsObjectPose ObjectPose(
      std::size_t object_index) const noexcept;
  [[nodiscard]] PhysicsTelemetry Telemetry() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace skate::world
