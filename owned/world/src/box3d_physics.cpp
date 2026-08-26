#include "skate/world/box3d_physics.h"

#include <box3d/box3d.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace skate::world {
namespace {

constexpr double kMaximumAccumulatedSeconds = 0.25;
constexpr int kMaximumStepsPerAdvance = 8;
constexpr float kMinimumHalfExtent = 0.005f;
constexpr int kMaximumHullVertices = 64;
constexpr float kPlayerProxyRadius = 0.52f;
constexpr float kPlayerProxyLowerCenter = -0.20f;
constexpr float kPlayerProxyUpperCenter = 1.25f;

b3Vec3 ToBox3D(const Vec3& value) {
  return {
      value.x / kSkateMetresPerBox3DUnit,
      value.y / kSkateMetresPerBox3DUnit,
      value.z / kSkateMetresPerBox3DUnit,
  };
}

Vec3 FromBox3D(const b3Vec3& value) {
  return {
      value.x * kSkateMetresPerBox3DUnit,
      value.y * kSkateMetresPerBox3DUnit,
      value.z * kSkateMetresPerBox3DUnit,
  };
}

Vec3 FromBox3DPosition(const b3Pos& value) {
  return {
      static_cast<float>(value.x * kSkateMetresPerBox3DUnit),
      static_cast<float>(value.y * kSkateMetresPerBox3DUnit),
      static_cast<float>(value.z * kSkateMetresPerBox3DUnit),
  };
}

Vec3 FromBox3DDirection(const b3Vec3& value) {
  return {value.x, value.y, value.z};
}

bool SamePoint(const b3Vec3& left, const b3Vec3& right) {
  constexpr float kTolerance = 1.0e-5f;
  return std::abs(left.x - right.x) <= kTolerance &&
         std::abs(left.y - right.y) <= kTolerance &&
         std::abs(left.z - right.z) <= kTolerance;
}

std::array<Vec3, 2> SafeLocalBounds(const MapObject& object) {
  Vec3 minimum = object.local_bounds_min;
  Vec3 maximum = object.local_bounds_max;
  if (minimum.x > maximum.x || minimum.y > maximum.y ||
      minimum.z > maximum.z) {
    minimum = {-kMinimumHalfExtent, -kMinimumHalfExtent, -kMinimumHalfExtent};
    maximum = {kMinimumHalfExtent, kMinimumHalfExtent, kMinimumHalfExtent};
  }

  auto widen = [](float& low, float& high) {
    if (high - low < 2.0f * kMinimumHalfExtent) {
      const float center = 0.5f * (low + high);
      low = center - kMinimumHalfExtent;
      high = center + kMinimumHalfExtent;
    }
  };
  widen(minimum.x, maximum.x);
  widen(minimum.y, maximum.y);
  widen(minimum.z, maximum.z);
  return {minimum, maximum};
}

std::vector<b3Vec3> ConvexPoints(const MapObject& object) {
  std::vector<b3Vec3> result;
  result.reserve(std::min<std::size_t>(
      object.collision_triangles.size() * 3, kMaximumHullVertices));

  const auto append = [&result](const Vec3& point) {
    const b3Vec3 converted = ToBox3D(point);
    if (std::none_of(result.begin(), result.end(),
                     [&converted](const b3Vec3& existing) {
                       return SamePoint(existing, converted);
                     })) {
      result.push_back(converted);
    }
  };

  for (const CollisionTriangle& triangle : object.collision_triangles) {
    append(triangle.a);
    append(triangle.b);
    append(triangle.c);
    if (result.size() >= kMaximumHullVertices) {
      break;
    }
  }
  if (result.size() < 4) {
    result.clear();
    const auto bounds = SafeLocalBounds(object);
    const Vec3& low = bounds[0];
    const Vec3& high = bounds[1];
    for (const float x : {low.x, high.x}) {
      for (const float y : {low.y, high.y}) {
        for (const float z : {low.z, high.z}) {
          result.push_back(ToBox3D({x, y, z}));
        }
      }
    }
  }
  return result;
}

}  // namespace

struct OwnedPhysicsWorld::Impl {
  struct BodyRecord {
    b3BodyId id = b3_nullBodyId;
    ObjectPhysicsType type = ObjectPhysicsType::Disabled;
    std::string name;
    PhysicsObjectPose last_pose;
    std::uint64_t pose_revision = 0;
    std::uint32_t break_group = 0;
    float break_speed_threshold = 2.5f;
    float break_impulse_scale = 0.45f;
    float break_angular_impulse = 0.08f;
    float break_gravity_scale = 1.0f;
  };

  b3WorldId world = b3_nullWorldId;
  b3BodyId static_world_body = b3_nullBodyId;
  b3BodyId player_proxy_body = b3_nullBodyId;
  b3MeshData* static_world_mesh = nullptr;
  std::vector<BodyRecord> bodies;
  PhysicsTelemetry telemetry;
  double accumulator_seconds = 0.0;
  Vec3 player_proxy_velocity;
  std::set<std::uint32_t> broken_groups;

  void Reset() noexcept {
    if (B3_IS_NON_NULL(world)) {
      for (auto iterator = bodies.rbegin(); iterator != bodies.rend();
           ++iterator) {
        if (B3_IS_NON_NULL(iterator->id) && b3Body_IsValid(iterator->id)) {
          b3DestroyBody(iterator->id);
        }
      }
      if (B3_IS_NON_NULL(player_proxy_body) &&
          b3Body_IsValid(player_proxy_body)) {
        b3DestroyBody(player_proxy_body);
      }
      if (B3_IS_NON_NULL(static_world_body) &&
          b3Body_IsValid(static_world_body)) {
        b3DestroyBody(static_world_body);
      }
      b3DestroyWorld(world);
    }
    if (static_world_mesh != nullptr) {
      b3DestroyMesh(static_world_mesh);
    }

    world = b3_nullWorldId;
    static_world_body = b3_nullBodyId;
    player_proxy_body = b3_nullBodyId;
    static_world_mesh = nullptr;
    bodies.clear();
    accumulator_seconds = 0.0;
    player_proxy_velocity = {};
    broken_groups.clear();
    telemetry.world_steps = 0;
    telemetry.dropped_step_batches = 0;
    telemetry.transform_updates = 0;
    telemetry.static_body_count = 0;
    telemetry.dynamic_body_count = 0;
    telemetry.contact_count = 0;
    telemetry.sleeping_body_count = 0;
    telemetry.player_contact_count = 0;
    telemetry.player_proxy_updates = 0;
    telemetry.player_proxy_active = false;
    telemetry.breakable_body_count = 0;
    telemetry.broken_group_count = 0;
    telemetry.glass_break_events = 0;
    telemetry.last_broken_group = 0;
    telemetry.last_break_speed = 0.0f;
    telemetry.accumulator_seconds = 0.0;
    telemetry.representative_dynamic_pose = {};
  }

  void CreatePlayerProxy() {
    b3BodyDef body_definition = b3DefaultBodyDef();
    body_definition.type = b3_kinematicBody;
    body_definition.name = "native-player-proxy";
    body_definition.isEnabled = false;
    body_definition.enableContactRecycling = false;
    player_proxy_body = b3CreateBody(world, &body_definition);
    if (B3_IS_NULL(player_proxy_body)) {
      throw std::runtime_error(
          "Box3D failed to create the native player proxy");
    }

    b3ShapeDef shape_definition = b3DefaultShapeDef();
    shape_definition.baseMaterial.friction = 0.45f;
    shape_definition.baseMaterial.restitution = 0.0f;
    shape_definition.enableContactEvents = true;
    const b3Capsule capsule{
        .center1 = {0.0f, kPlayerProxyLowerCenter, 0.0f},
        .center2 = {0.0f, kPlayerProxyUpperCenter, 0.0f},
        .radius = kPlayerProxyRadius,
    };
    const b3ShapeId shape = b3CreateCapsuleShape(
        player_proxy_body, &shape_definition, &capsule);
    if (B3_IS_NULL(shape)) {
      throw std::runtime_error(
          "Box3D failed to attach the native player proxy capsule");
    }
  }

  void CreateStaticWorldMesh(const MapDefinition& map) {
    if (map.collision_triangles.empty()) {
      return;
    }

    std::vector<bool> excluded(map.collision_triangles.size(), false);
    for (const MapObject& object : map.editable_objects) {
      if (object.physics.type == ObjectPhysicsType::Disabled) {
        continue;
      }
      const std::size_t begin =
          object.source_first_collision_triangle;
      const std::size_t count =
          object.source_collision_triangle_count;
      if (begin > excluded.size() || count > excluded.size() - begin) {
        throw std::runtime_error(
            "Box3D object collision range exceeds the map collision table");
      }
      std::fill(excluded.begin() + static_cast<std::ptrdiff_t>(begin),
                excluded.begin() +
                    static_cast<std::ptrdiff_t>(begin + count),
                true);
    }

    std::vector<b3Vec3> vertices;
    std::vector<std::int32_t> indices;
    vertices.reserve(map.collision_triangles.size() * 3);
    indices.reserve(map.collision_triangles.size() * 3);
    for (std::size_t index = 0; index < map.collision_triangles.size();
         ++index) {
      if (excluded[index]) {
        continue;
      }
      const CollisionTriangle& triangle = map.collision_triangles[index];
      if (vertices.size() >
          static_cast<std::size_t>(
              std::numeric_limits<std::int32_t>::max() - 3)) {
        throw std::runtime_error("Box3D static collision mesh is too large");
      }
      const std::int32_t first = static_cast<std::int32_t>(vertices.size());
      vertices.push_back(ToBox3D(triangle.a));
      vertices.push_back(ToBox3D(triangle.b));
      vertices.push_back(ToBox3D(triangle.c));
      indices.push_back(first);
      indices.push_back(first + 1);
      indices.push_back(first + 2);
    }
    if (vertices.empty()) {
      return;
    }

    b3MeshDef mesh_definition{};
    mesh_definition.vertices = vertices.data();
    mesh_definition.indices = indices.data();
    mesh_definition.vertexCount = static_cast<int>(vertices.size());
    mesh_definition.triangleCount = static_cast<int>(indices.size() / 3);
    mesh_definition.weldTolerance =
        0.001f / kSkateMetresPerBox3DUnit;
    mesh_definition.weldVertices = true;
    mesh_definition.useMedianSplit = false;
    mesh_definition.identifyEdges = true;
    static_world_mesh = b3CreateMesh(&mesh_definition, nullptr, 0);
    if (static_world_mesh == nullptr) {
      throw std::runtime_error(
          "Box3D rejected the owned-world static collision mesh");
    }

    b3BodyDef body_definition = b3DefaultBodyDef();
    body_definition.type = b3_staticBody;
    body_definition.name = "owned-world";
    static_world_body = b3CreateBody(world, &body_definition);
    if (B3_IS_NULL(static_world_body)) {
      throw std::runtime_error(
          "Box3D failed to create the owned-world static body");
    }

    b3ShapeDef shape_definition = b3DefaultShapeDef();
    shape_definition.baseMaterial.friction = 0.8f;
    shape_definition.baseMaterial.restitution = 0.0f;
    const b3ShapeId shape = b3CreateMeshShape(
        static_world_body, &shape_definition, static_world_mesh, b3Vec3_one);
    if (B3_IS_NULL(shape)) {
      throw std::runtime_error(
          "Box3D failed to attach the owned-world static collision mesh");
    }
    ++telemetry.static_body_count;
  }

  void AttachObjectShape(
      const MapObject& object,
      b3BodyId body,
      b3ShapeDef* shape_definition) {
    const auto bounds = SafeLocalBounds(object);
    const Vec3 center = (bounds[0] + bounds[1]) * 0.5f;
    const Vec3 half_extent = (bounds[1] - bounds[0]) * 0.5f;
    b3ShapeId shape = b3_nullShapeId;

    switch (object.physics.shape) {
      case ObjectCollisionShape::Box: {
        const b3BoxHull box = b3MakeOffsetBoxHull(
            half_extent.x / kSkateMetresPerBox3DUnit,
            half_extent.y / kSkateMetresPerBox3DUnit,
            half_extent.z / kSkateMetresPerBox3DUnit, ToBox3D(center));
        shape = b3CreateHullShape(body, shape_definition, &box.base);
        break;
      }
      case ObjectCollisionShape::Sphere: {
        const b3Sphere sphere{
            ToBox3D(center),
            std::max({half_extent.x, half_extent.y, half_extent.z}) /
                kSkateMetresPerBox3DUnit,
        };
        shape = b3CreateSphereShape(body, shape_definition, &sphere);
        break;
      }
      case ObjectCollisionShape::ConvexHull: {
        std::vector<b3Vec3> points = ConvexPoints(object);
        b3HullData* hull = b3CreateHull(
            points.data(), static_cast<int>(points.size()),
            kMaximumHullVertices);
        if (hull == nullptr) {
          throw std::runtime_error(
              "Box3D could not construct a convex hull for object '" +
              object.name + "'");
        }
        shape = b3CreateHullShape(body, shape_definition, hull);
        b3DestroyHull(hull);
        break;
      }
    }
    if (B3_IS_NULL(shape)) {
      throw std::runtime_error(
          "Box3D failed to attach collision shape for object '" +
          object.name + "'");
    }
  }

  void CreateObjectBodies(const MapDefinition& map) {
    bodies.resize(map.editable_objects.size());
    for (std::size_t index = 0; index < map.editable_objects.size(); ++index) {
      const MapObject& object = map.editable_objects[index];
      BodyRecord& record = bodies[index];
      record.type = object.physics.type;
      record.name = object.name.substr(0, B3_BODY_NAME_LENGTH - 1);
      record.break_group = object.physics.break_group;
      record.break_speed_threshold =
          object.physics.break_speed_threshold;
      record.break_impulse_scale = object.physics.break_impulse_scale;
      record.break_angular_impulse =
          object.physics.break_angular_impulse;
      record.break_gravity_scale =
          object.physics.break_gravity_scale;
      if (object.physics.type == ObjectPhysicsType::Disabled) {
        continue;
      }

      b3BodyDef body_definition = b3DefaultBodyDef();
      body_definition.type =
          object.physics.type == ObjectPhysicsType::Dynamic
              ? b3_dynamicBody
              : b3_staticBody;
      body_definition.position = ToBox3D(object.origin);
      body_definition.linearDamping = object.physics.linear_damping;
      body_definition.angularDamping = object.physics.angular_damping;
      body_definition.gravityScale = object.physics.gravity_scale;
      body_definition.enableSleep = object.physics.enable_sleep;
      body_definition.isAwake = object.physics.initially_awake;
      body_definition.name = record.name.c_str();
      body_definition.userData =
          reinterpret_cast<void*>(static_cast<std::uintptr_t>(index + 1));
      record.id = b3CreateBody(world, &body_definition);
      if (B3_IS_NULL(record.id)) {
        throw std::runtime_error(
            "Box3D failed to create body for object '" + object.name + "'");
      }

      b3ShapeDef shape_definition = b3DefaultShapeDef();
      shape_definition.density = object.physics.density;
      shape_definition.baseMaterial.friction = object.physics.friction;
      shape_definition.baseMaterial.restitution =
          object.physics.restitution;
      shape_definition.enableContactEvents = true;
      AttachObjectShape(object, record.id, &shape_definition);

      if (object.physics.type == ObjectPhysicsType::Dynamic) {
        ++telemetry.dynamic_body_count;
        if (record.break_group != 0) {
          ++telemetry.breakable_body_count;
        }
      } else {
        ++telemetry.static_body_count;
      }
    }
  }

  void BreakGroup(std::uint32_t group, float impact_speed) {
    if (group == 0 || broken_groups.contains(group)) {
      return;
    }
    broken_groups.insert(group);
    const b3Vec3 player_impulse = ToBox3D(player_proxy_velocity);
    for (std::size_t index = 0; index < bodies.size(); ++index) {
      BodyRecord& record = bodies[index];
      if (record.break_group != group ||
          B3_IS_NULL(record.id) || !b3Body_IsValid(record.id)) {
        continue;
      }
      b3Body_SetGravityScale(record.id, record.break_gravity_scale);
      b3Body_SetAwake(record.id, true);
      b3Body_ApplyLinearImpulseToCenter(
          record.id, player_impulse * record.break_impulse_scale, true);
      const float sign = (index & 1u) == 0u ? 1.0f : -1.0f;
      const b3Vec3 angular_impulse{
          sign * record.break_angular_impulse,
          (0.35f + 0.07f * static_cast<float>(index % 5)) *
              record.break_angular_impulse,
          -sign * 0.65f * record.break_angular_impulse,
      };
      b3Body_ApplyAngularImpulse(
          record.id, angular_impulse, true);
    }
    telemetry.broken_group_count = broken_groups.size();
    ++telemetry.glass_break_events;
    telemetry.last_broken_group = group;
    telemetry.last_break_speed = impact_speed;
  }

  void ProcessPlayerBreakContacts() {
    if (!telemetry.player_proxy_active ||
        B3_IS_NULL(player_proxy_body) ||
        !b3Body_IsValid(player_proxy_body) ||
        !b3Body_IsEnabled(player_proxy_body)) {
      return;
    }
    const float impact_speed = Length(player_proxy_velocity);
    if (impact_speed <= 0.0f) {
      return;
    }
    const int capacity = b3Body_GetContactCapacity(player_proxy_body);
    if (capacity <= 0) {
      return;
    }
    std::vector<b3ContactData> contacts(
        static_cast<std::size_t>(capacity));
    const int count = b3Body_GetContactData(
        player_proxy_body, contacts.data(), capacity);
    std::set<std::uint32_t> triggered_groups;
    for (int index = 0; index < count; ++index) {
      const b3ContactData& contact = contacts[index];
      const b3BodyId body_a = b3Shape_GetBody(contact.shapeIdA);
      const b3BodyId body_b = b3Shape_GetBody(contact.shapeIdB);
      b3BodyId other = b3_nullBodyId;
      if (B3_ID_EQUALS(body_a, player_proxy_body)) {
        other = body_b;
      } else if (B3_ID_EQUALS(body_b, player_proxy_body)) {
        other = body_a;
      }
      if (B3_IS_NULL(other) || !b3Body_IsValid(other)) {
        continue;
      }
      const std::uintptr_t user_data =
          reinterpret_cast<std::uintptr_t>(b3Body_GetUserData(other));
      if (user_data == 0 || user_data > bodies.size()) {
        continue;
      }
      const BodyRecord& record = bodies[user_data - 1];
      if (record.break_group != 0 &&
          impact_speed >= record.break_speed_threshold &&
          !broken_groups.contains(record.break_group)) {
        triggered_groups.insert(record.break_group);
      }
    }
    for (const std::uint32_t group : triggered_groups) {
      BreakGroup(group, impact_speed);
    }
  }

  PhysicsObjectPose ObjectPose(std::size_t object_index) const noexcept {
    PhysicsObjectPose result;
    if (object_index >= bodies.size()) {
      return result;
    }
    const BodyRecord& record = bodies[object_index];
    if (B3_IS_NULL(record.id) || !b3Body_IsValid(record.id)) {
      return result;
    }

    const b3WorldTransform transform = b3Body_GetTransform(record.id);
    const b3Matrix3 matrix = b3MakeMatrixFromQuat(transform.q);
    result.valid = true;
    result.revision = record.pose_revision;
    result.position = FromBox3DPosition(transform.p);
    result.x_axis = FromBox3DDirection(matrix.cx);
    result.y_axis = FromBox3DDirection(matrix.cy);
    result.z_axis = FromBox3DDirection(matrix.cz);
    result.linear_velocity =
        FromBox3D(b3Body_GetLinearVelocity(record.id));
    result.angular_velocity =
        FromBox3DDirection(b3Body_GetAngularVelocity(record.id));
    result.awake = b3Body_IsAwake(record.id);
    return result;
  }

  void RefreshBodyPoseRevisions() {
    constexpr float kPositionToleranceSquared = 1.0e-10f;
    constexpr float kAxisToleranceSquared = 1.0e-10f;
    for (std::size_t index = 0; index < bodies.size(); ++index) {
      BodyRecord& record = bodies[index];
      if (record.type == ObjectPhysicsType::Disabled ||
          B3_IS_NULL(record.id) || !b3Body_IsValid(record.id)) {
        continue;
      }
      PhysicsObjectPose pose = ObjectPose(index);
      const bool changed =
          !record.last_pose.valid ||
          LengthSquared(pose.position - record.last_pose.position) >
              kPositionToleranceSquared ||
          LengthSquared(pose.x_axis - record.last_pose.x_axis) >
              kAxisToleranceSquared ||
          LengthSquared(pose.y_axis - record.last_pose.y_axis) >
              kAxisToleranceSquared ||
          LengthSquared(pose.z_axis - record.last_pose.z_axis) >
              kAxisToleranceSquared;
      if (changed) {
        ++record.pose_revision;
      }
      pose.revision = record.pose_revision;
      record.last_pose = pose;
    }
  }

  void RefreshTelemetry() {
    if (B3_IS_NULL(world)) {
      return;
    }
    const b3Counters counters = b3World_GetCounters(world);
    telemetry.contact_count =
        static_cast<std::size_t>(std::max(counters.contactCount, 0));
    telemetry.sleeping_body_count = 0;
    telemetry.player_contact_count = 0;
    telemetry.representative_dynamic_pose = {};
    if (B3_IS_NON_NULL(player_proxy_body) &&
        b3Body_IsValid(player_proxy_body) &&
        b3Body_IsEnabled(player_proxy_body)) {
      const int capacity =
          b3Body_GetContactCapacity(player_proxy_body);
      if (capacity > 0) {
        std::vector<b3ContactData> contacts(
            static_cast<std::size_t>(capacity));
        telemetry.player_contact_count = static_cast<std::size_t>(
            std::max(
                b3Body_GetContactData(
                    player_proxy_body, contacts.data(), capacity),
                0));
      }
    }
    for (std::size_t index = 0; index < bodies.size(); ++index) {
      const BodyRecord& record = bodies[index];
      if (record.type != ObjectPhysicsType::Dynamic ||
          B3_IS_NULL(record.id) || !b3Body_IsValid(record.id)) {
        continue;
      }
      if (!b3Body_IsAwake(record.id)) {
        ++telemetry.sleeping_body_count;
      }
      if (!telemetry.representative_dynamic_pose.valid) {
        telemetry.representative_dynamic_pose = ObjectPose(index);
      }
    }
    telemetry.accumulator_seconds = accumulator_seconds;
  }
};

OwnedPhysicsWorld::OwnedPhysicsWorld() : impl_(std::make_unique<Impl>()) {}

OwnedPhysicsWorld::~OwnedPhysicsWorld() {
  Reset();
}

OwnedPhysicsWorld::OwnedPhysicsWorld(OwnedPhysicsWorld&&) noexcept = default;

OwnedPhysicsWorld& OwnedPhysicsWorld::operator=(
    OwnedPhysicsWorld&&) noexcept = default;

void OwnedPhysicsWorld::Load(const MapDefinition& map) {
  const std::uint64_t next_generation = impl_->telemetry.world_generation + 1;
  impl_->Reset();
  impl_->telemetry.world_generation = next_generation;

  try {
    b3WorldDef world_definition = b3DefaultWorldDef();
    world_definition.gravity = {0.0f, -10.0f, 0.0f};
    world_definition.workerCount = 1;
    impl_->world = b3CreateWorld(&world_definition);
    if (B3_IS_NULL(impl_->world)) {
      throw std::runtime_error("Box3D failed to create an owned-world");
    }
    impl_->CreateStaticWorldMesh(map);
    impl_->CreatePlayerProxy();
    impl_->CreateObjectBodies(map);
    impl_->RefreshBodyPoseRevisions();
    impl_->RefreshTelemetry();
  } catch (...) {
    impl_->Reset();
    impl_->telemetry.world_generation = next_generation;
    throw;
  }
}

void OwnedPhysicsWorld::Reset() noexcept {
  impl_->Reset();
}

void OwnedPhysicsWorld::Step(double frame_seconds) {
  if (B3_IS_NULL(impl_->world) || !std::isfinite(frame_seconds) ||
      frame_seconds <= 0.0) {
    return;
  }
  impl_->accumulator_seconds = std::min(
      impl_->accumulator_seconds + frame_seconds,
      kMaximumAccumulatedSeconds);

  int step_count = 0;
  while (impl_->accumulator_seconds + 1.0e-12 >=
             kBox3DFixedTimeStepSeconds &&
         step_count < kMaximumStepsPerAdvance) {
    b3World_Step(
        impl_->world, kBox3DFixedTimeStepSeconds, kBox3DSolverSubSteps);
    impl_->accumulator_seconds -= kBox3DFixedTimeStepSeconds;
    ++impl_->telemetry.world_steps;
    ++step_count;
    const b3BodyEvents events = b3World_GetBodyEvents(impl_->world);
    impl_->telemetry.transform_updates +=
        static_cast<std::uint64_t>(std::max(events.moveCount, 0));
    impl_->ProcessPlayerBreakContacts();
    impl_->RefreshBodyPoseRevisions();
  }
  if (step_count == kMaximumStepsPerAdvance &&
      impl_->accumulator_seconds >= kBox3DFixedTimeStepSeconds) {
    ++impl_->telemetry.dropped_step_batches;
    impl_->accumulator_seconds =
        std::fmod(impl_->accumulator_seconds,
                  static_cast<double>(kBox3DFixedTimeStepSeconds));
  }
  impl_->RefreshTelemetry();
}

void OwnedPhysicsWorld::SetPlayerProxy(
    Vec3 position, Vec3 linear_velocity, bool active) {
  if (B3_IS_NULL(impl_->player_proxy_body) ||
      !b3Body_IsValid(impl_->player_proxy_body)) {
    return;
  }
  const bool finite =
      std::isfinite(position.x) && std::isfinite(position.y) &&
      std::isfinite(position.z) && std::isfinite(linear_velocity.x) &&
      std::isfinite(linear_velocity.y) &&
      std::isfinite(linear_velocity.z);
  if (!active || !finite) {
    if (b3Body_IsEnabled(impl_->player_proxy_body)) {
      b3Body_Disable(impl_->player_proxy_body);
    }
    impl_->telemetry.player_proxy_active = false;
    impl_->telemetry.player_contact_count = 0;
    impl_->player_proxy_velocity = {};
    return;
  }

  if (!b3Body_IsEnabled(impl_->player_proxy_body)) {
    b3Body_Enable(impl_->player_proxy_body);
  }
  b3Body_SetTransform(
      impl_->player_proxy_body, ToBox3D(position), b3Quat_identity);
  b3Body_SetLinearVelocity(
      impl_->player_proxy_body, ToBox3D(linear_velocity));
  impl_->player_proxy_velocity = linear_velocity;
  impl_->telemetry.player_proxy_active = true;
  ++impl_->telemetry.player_proxy_updates;
}

bool OwnedPhysicsWorld::IsLoaded() const noexcept {
  return B3_IS_NON_NULL(impl_->world);
}

bool OwnedPhysicsWorld::HasBody(std::size_t object_index) const noexcept {
  return object_index < impl_->bodies.size() &&
         B3_IS_NON_NULL(impl_->bodies[object_index].id) &&
         b3Body_IsValid(impl_->bodies[object_index].id);
}

PhysicsObjectPose OwnedPhysicsWorld::ObjectPose(
    std::size_t object_index) const noexcept {
  return impl_->ObjectPose(object_index);
}

PhysicsTelemetry OwnedPhysicsWorld::Telemetry() const noexcept {
  return impl_->telemetry;
}

}  // namespace skate::world
