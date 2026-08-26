#include "skate/world/box3d_physics.h"
#include "skate/world/maps.h"
#include "skate/world/grind_spline.h"
#include "skate/world/map_editor.h"
#include "skate/world/owned_map_package.h"
#include "skate/world/render_world.h"
#include "skate/world/rw_collision_mesh.h"
#include "skate/world/skate_object_package.h"
#include "skate/world/water_simulation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

bool NearlyEqual(float left, float right, float tolerance = 1.0e-4f) {
  return std::abs(left - right) <= tolerance;
}

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "WORLD_TEST_FAIL " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint16_t ReadBeU16(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes.at(offset)) << 8u) |
      bytes.at(offset + 1));
}

std::uint16_t ReadLeU16(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint16_t>(
      bytes.at(offset) |
      (static_cast<std::uint16_t>(bytes.at(offset + 1)) << 8u));
}

std::uint32_t ReadBeU32(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes.at(offset)) << 24u) |
         (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 16u) |
         (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 8u) |
         bytes.at(offset + 3);
}

void WriteBeU32(std::vector<std::uint8_t>& bytes,
                std::size_t offset,
                std::uint32_t value) {
  bytes.at(offset) = static_cast<std::uint8_t>(value >> 24u);
  bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 16u);
  bytes.at(offset + 2) = static_cast<std::uint8_t>(value >> 8u);
  bytes.at(offset + 3) = static_cast<std::uint8_t>(value);
}

std::uint64_t ReadBeU64(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return (static_cast<std::uint64_t>(ReadBeU32(bytes, offset)) << 32u) |
         ReadBeU32(bytes, offset + 4);
}

float ReadBeF32(const std::vector<std::uint8_t>& bytes,
                std::size_t offset) {
  return std::bit_cast<float>(ReadBeU32(bytes, offset));
}

skate::world::MapDefinition MakePhysicsTestMap(
    std::size_t cube_count) {
  using namespace skate::world;
  MapDefinition map;
  map.name = "box3d_test";
  map.collision_triangles = {
      {
          .a = {-20.0f, 0.0f, -20.0f},
          .b = {20.0f, 0.0f, 20.0f},
          .c = {20.0f, 0.0f, -20.0f},
      },
      {
          .a = {-20.0f, 0.0f, -20.0f},
          .b = {-20.0f, 0.0f, 20.0f},
          .c = {20.0f, 0.0f, 20.0f},
      },
  };
  for (std::size_t index = 0; index < cube_count; ++index) {
    MapObject cube;
    cube.id = static_cast<MapObjectId>(index + 1);
    cube.name = "cube_" + std::to_string(index);
    cube.origin = {
        index % 2 == 0 ? 0.0f : 0.08f,
        0.55f + static_cast<float>(index) * 1.05f,
        0.0f,
    };
    cube.local_bounds_min = {-0.5f, -0.5f, -0.5f};
    cube.local_bounds_max = {0.5f, 0.5f, 0.5f};
    cube.physics.type = ObjectPhysicsType::Dynamic;
    cube.physics.shape = ObjectCollisionShape::Box;
    cube.physics.density = 120.0f;
    cube.physics.friction = 0.7f;
    cube.physics.restitution = 0.0f;
    cube.physics.linear_damping = 0.05f;
    cube.physics.angular_damping = 0.2f;
    map.editable_objects.push_back(std::move(cube));
  }
  return map;
}

}  // namespace

int main() {
  using namespace skate::world;

  {
    const auto future_package =
        std::filesystem::temp_directory_path() /
        "skate_owned_world_future_format_test.skate";
    const std::array<std::uint8_t, 12> header = {
        'S', 'K', 'A', 'T', 'E', '1', '5', '\0',
        0x78, 0x56, 0x34, 0x12};
    {
      std::ofstream output(
          future_package, std::ios::binary | std::ios::trunc);
      output.write(
          reinterpret_cast<const char*>(header.data()), header.size());
    }
    bool rejected_as_future = false;
    try {
      (void)LoadOwnedMapPackage(future_package);
    } catch (const std::exception& error) {
      rejected_as_future =
          std::string_view(error.what()).find("requires a newer") !=
          std::string_view::npos;
    }
    std::error_code ec;
    std::filesystem::remove(future_package, ec);
    Require(rejected_as_future,
            "future SKATE versions must request a newer runtime");
  }

  {
    MapDefinition physics_map = MakePhysicsTestMap(3);
    OwnedPhysicsWorld physics;
    physics.Load(physics_map);
    Require(
        physics.IsLoaded() && physics.HasBody(0) &&
            physics.HasBody(1) && physics.HasBody(2),
        "Box3D did not create one body per enabled owned object");
    PhysicsTelemetry telemetry = physics.Telemetry();
    Require(
        telemetry.static_body_count == 1 &&
            telemetry.dynamic_body_count == 3 &&
            telemetry.world_steps == 0,
        "Box3D initial body telemetry is incorrect");

    const float initial_top = physics.ObjectPose(2).position.y;
    physics.Step(kBox3DFixedTimeStepSeconds * 0.49);
    Require(
        physics.Telemetry().world_steps == 0,
        "Box3D fixed-step accumulator advanced too early");
    physics.Step(kBox3DFixedTimeStepSeconds * 0.51);
    Require(
        physics.Telemetry().world_steps == 1,
        "Box3D fixed-step accumulator did not advance at 60 Hz");

    for (int frame = 0; frame < 12 * 120; ++frame) {
      physics.Step(1.0 / 120.0);
    }
    const PhysicsObjectPose bottom = physics.ObjectPose(0);
    const PhysicsObjectPose middle = physics.ObjectPose(1);
    const PhysicsObjectPose top = physics.ObjectPose(2);
    Require(
        bottom.valid && middle.valid && top.valid &&
            top.position.y < initial_top &&
            bottom.position.y > 0.35f &&
            bottom.position.y < 0.75f &&
            middle.position.y > bottom.position.y + 0.75f &&
            top.position.y > middle.position.y + 0.75f,
        "Box3D cube stack did not fall onto static owned-world collision "
        "and settle independently");
    Require(
        std::abs(Length(bottom.x_axis) - 1.0f) < 0.01f &&
            std::abs(Length(bottom.y_axis) - 1.0f) < 0.01f &&
            bottom.revision > 0 &&
            physics.Telemetry().contact_count >= 3,
        "Box3D transform propagation or contact telemetry is invalid");

    physics.Load(MakePhysicsTestMap(1));
    const float before_player_contact =
        physics.ObjectPose(0).position.x;
    constexpr float player_speed = 4.0f;
    std::size_t maximum_player_contacts = 0;
    for (int frame = 0; frame < 90; ++frame) {
      const float time = static_cast<float>(frame) / 60.0f;
      physics.SetPlayerProxy(
          {-1.4f + player_speed * time, 0.55f, 0.0f},
          {player_speed, 0.0f, 0.0f}, true);
      physics.Step(kBox3DFixedTimeStepSeconds);
      maximum_player_contacts = std::max(
          maximum_player_contacts,
          physics.Telemetry().player_contact_count);
    }
    const PhysicsObjectPose after_player_contact =
        physics.ObjectPose(0);
    Require(
        after_player_contact.position.x >
                before_player_contact + 0.15f &&
            physics.Telemetry().player_proxy_active &&
            physics.Telemetry().player_proxy_updates == 90 &&
            maximum_player_contacts > 0,
        "Box3D native-player proxy did not move a dynamic owned body");
    physics.SetPlayerProxy({}, {}, false);
    Require(
        !physics.Telemetry().player_proxy_active,
        "Box3D native-player proxy did not disable cleanly");

    MapDefinition glass_map = MakePhysicsTestMap(2);
    for (MapObject& shard : glass_map.editable_objects) {
      shard.physics.gravity_scale = 0.0f;
      shard.physics.initially_awake = false;
      shard.physics.break_group = 7;
      shard.physics.break_speed_threshold = 2.0f;
      shard.physics.break_impulse_scale = 0.35f;
      shard.physics.break_angular_impulse = 0.04f;
      shard.physics.break_gravity_scale = 1.0f;
    }
    physics.Load(glass_map);
    const float initial_glass_height =
        physics.ObjectPose(1).position.y;
    for (int frame = 0; frame < 20; ++frame) {
      physics.Step(kBox3DFixedTimeStepSeconds);
    }
    Require(
        NearlyEqual(
            physics.ObjectPose(1).position.y, initial_glass_height, 0.01f) &&
            physics.Telemetry().breakable_body_count == 2 &&
            physics.Telemetry().glass_break_events == 0,
        "breakable bodies did not remain locked before impact");
    for (int frame = 0; frame < 120; ++frame) {
      const float time = static_cast<float>(frame) / 60.0f;
      physics.SetPlayerProxy(
          {-1.6f + 4.5f * time, 0.65f, 0.0f},
          {4.5f, 0.0f, 0.0f}, true);
      physics.Step(kBox3DFixedTimeStepSeconds);
    }
    const PhysicsTelemetry glass_telemetry = physics.Telemetry();
    if (glass_telemetry.glass_break_events != 1 ||
        glass_telemetry.broken_group_count != 1 ||
        glass_telemetry.last_broken_group != 7 ||
        glass_telemetry.last_break_speed < 4.4f ||
        physics.ObjectPose(1).position.y >=
            initial_glass_height - 0.05f) {
      std::cerr
          << "GLASS_DIAGNOSTIC events="
          << glass_telemetry.glass_break_events
          << " groups=" << glass_telemetry.broken_group_count
          << " last_group=" << glass_telemetry.last_broken_group
          << " speed=" << glass_telemetry.last_break_speed
          << " initial_y=" << initial_glass_height
          << " final_y=" << physics.ObjectPose(1).position.y << '\n';
    }
    Require(
        glass_telemetry.glass_break_events == 1 &&
            glass_telemetry.broken_group_count == 1 &&
            glass_telemetry.last_broken_group == 7 &&
            glass_telemetry.last_break_speed >= 4.4f &&
            physics.ObjectPose(1).position.y <
                initial_glass_height - 0.05f,
        "player impact did not release the complete glass break group");

    const std::uint64_t generation =
        physics.Telemetry().world_generation;
    physics.Load(MakePhysicsTestMap(1));
    Require(
        physics.Telemetry().world_generation == generation + 1 &&
            physics.Telemetry().dynamic_body_count == 1 &&
            physics.HasBody(0) && !physics.HasBody(1),
        "Box3D map reload left stale owned bodies behind");
    physics.Reset();
    Require(
        !physics.IsLoaded() && !physics.HasBody(0) &&
            physics.Telemetry().dynamic_body_count == 0,
        "Box3D reset did not destroy the owned world");
  }

  {
    MapDefinition duplicate_faces;
    duplicate_faces.render_mesh.vertices.resize(3);
    duplicate_faces.render_mesh.vertices[0].position = {1.0f, 0.0f, 1.0f};
    duplicate_faces.render_mesh.vertices[1].position = {2.0f, 0.0f, 1.0f};
    duplicate_faces.render_mesh.vertices[2].position = {1.0f, 0.0f, 2.0f};
    for (RenderVertex& vertex :
         duplicate_faces.render_mesh.vertices) {
      vertex.material = 1;
      vertex.normal = {0.0f, 1.0f, 0.0f};
    }
    duplicate_faces.render_mesh.indices = {
        0, 1, 2,  // first oriented face
        0, 1, 2,  // exact same-winding duplicate: remove
        0, 2, 1,  // reverse-wound two-sided partner: retain
    };
    RenderWorldBuildOptions options;
    options.chunk_size = 16.0f;
    const RenderWorld duplicate_world =
        BuildRenderWorld(duplicate_faces, options);
    Require(
        duplicate_world.source_triangle_count == 3 &&
            duplicate_world.output_triangle_count == 2,
        "render compiler did not distinguish duplicate and reverse faces");
    Require(
        duplicate_world.backface_culled_material_count == 1 &&
            duplicate_world.chunks.size() == 1 &&
            duplicate_world.chunks[0].batches.size() == 1 &&
            duplicate_world.chunks[0].batches[0].cull_backfaces,
        "reverse-wound presentation twins did not enable backface culling");
  }

  {
    MapDefinition prefab_package;
    prefab_package.name = "Manual Pad";
    prefab_package.render_mesh.indices = {0, 1, 2};
    prefab_package.collision_triangles.resize(1);
    GrindRail rail;
    rail.name = "Top Edge";
    rail.points = {{10.0f, 1.0f, 0.5f}, {12.0f, 1.0f, 0.5f}};
    prefab_package.grind_rails.push_back(rail);
    MapObject root;
    root.id = 7;
    root.name = "ManualPadRoot";
    root.origin = {10.0f, 0.0f, 0.0f};
    root.source_index_count = 3;
    root.source_collision_triangle_count = 1;
    root.render_mesh.indices = {0, 1, 2};
    root.collision_triangles.resize(1);
    root.grind_rail_indices = {0};
    prefab_package.editable_objects.push_back(root);

    SkateObjectAsset asset =
        ExtractSkateObjectAsset(std::move(prefab_package));
    Require(asset.name == "Manual Pad" &&
                asset.objects.size() == 1 &&
                asset.objects[0].origin.x == 0.0f &&
                asset.objects[0].grind_rail_indices ==
                    std::vector<std::uint32_t>{0} &&
                NearlyEqual(asset.grind_rails[0].points[0].x, 0.0f) &&
                NearlyEqual(asset.grind_rails[0].points[0].z, 0.5f) &&
                NearlyEqual(asset.grind_rails[0].points[1].x, 2.0f) &&
                NearlyEqual(asset.grind_rails[0].points[1].z, 0.5f),
            "SKATEOBJ profile did not normalize its root and grind spline");
  }

  {
    MapDefinition prefab_package;
    prefab_package.package_version = 14;
    prefab_package.map_object_schema_version = 3;
    prefab_package.name = "Box3D Pair";
    prefab_package.spawn.position = {10.0f, 0.0f, -2.0f};
    prefab_package.render_mesh.indices = {0, 1, 2, 3, 4, 5};
    prefab_package.collision_triangles.resize(2);
    for (std::size_t index = 0; index < 2; ++index) {
      MapObject root;
      root.id = static_cast<MapObjectId>(index + 1);
      root.name = "Cube" + std::to_string(index + 1);
      root.origin = {
          10.0f + static_cast<float>(index) * 2.0f, 1.0f, -2.0f};
      root.source_first_index =
          static_cast<std::uint32_t>(index * 3);
      root.source_index_count = 3;
      root.source_first_collision_triangle =
          static_cast<std::uint32_t>(index);
      root.source_collision_triangle_count = 1;
      root.render_mesh.indices = {0, 1, 2};
      root.collision_triangles.resize(1);
      root.physics.type = ObjectPhysicsType::Dynamic;
      root.physics.shape = ObjectCollisionShape::Box;
      prefab_package.editable_objects.push_back(std::move(root));
    }

    SkateObjectAsset asset =
        ExtractSkateObjectAsset(std::move(prefab_package));
    Require(
        asset.format_version == 2 && asset.objects.size() == 2 &&
            NearlyEqual(asset.objects[0].origin.x, 0.0f) &&
            NearlyEqual(asset.objects[0].origin.y, 1.0f) &&
            NearlyEqual(asset.objects[1].origin.x, 2.0f) &&
            asset.objects[0].physics.type ==
                ObjectPhysicsType::Dynamic &&
            asset.objects[1].physics.shape ==
                ObjectCollisionShape::Box,
        "SKATEOBJ v2 did not preserve independent physics roots in "
        "prefab-pivot space");
  }

  {
    MapDefinition retail_map;
    retail_map.name = "retail_collision_identity";
    retail_map.collision_triangles = {
        CollisionTriangle{.a = {0.0f, 0.0f, 0.0f}},
        CollisionTriangle{.a = {1.0f, 0.0f, 0.0f}},
        CollisionTriangle{.a = {2.0f, 0.0f, 0.0f}},
    };
    retail_map.retail_collision_resource_names = {"cell_a#0", "cell_b#0"};
    retail_map.retail_collision_associations = {
        {.triangle_index = 0, .resource_index = 0, .cluster_index = 2},
        {.triangle_index = 1, .resource_index = 0, .cluster_index = 3},
        {.triangle_index = 1, .resource_index = 1, .cluster_index = 0},
        {.triangle_index = 2, .resource_index = 1, .cluster_index = 1},
    };
    retail_map.editable_objects = {
        MapObject{
            .id = 1,
            .name = "Detached",
            .source_first_collision_triangle = 0,
            .source_collision_triangle_count = 1,
        },
        MapObject{
            .id = 2,
            .name = "Unchanged",
            .source_first_collision_triangle = 2,
            .source_collision_triangle_count = 1,
        },
    };
    Require(HasRetailCollisionIdentity(retail_map),
            "valid many-to-many retail collision identity was rejected");
    Require(RetailCollisionResourcesForObject(retail_map,
                                              retail_map.editable_objects[0]) ==
                std::vector<std::uint16_t>{0},
            "editable object did not resolve its exact retail resource");
    const std::array<std::uint8_t, 2> detached{1, 0};
    const MapDefinition fallback_a =
        BuildRetailCollisionResourceFallback(retail_map, 0, detached);
    const MapDefinition fallback_b =
        BuildRetailCollisionResourceFallback(retail_map, 1, detached);
    Require(fallback_a.collision_triangles.size() == 1 &&
                NearlyEqual(fallback_a.collision_triangles.front().a.x, 1.0f),
            "retail fallback retained a detached object's old collision");
    Require(fallback_b.collision_triangles.size() == 2 &&
                NearlyEqual(fallback_b.collision_triangles.back().a.x, 2.0f),
            "retail fallback dropped unchanged resource collision");
    retail_map.retail_collision_associations.erase(
        retail_map.retail_collision_associations.begin());
    Require(!HasRetailCollisionIdentity(retail_map),
            "retail collision identity accepted a triangle coverage gap");
  }

  {
    constexpr float identity_view[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    constexpr float projection[16] = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    constexpr float camera[3] = {0.0f, 0.0f, 0.0f};
    const EditorRay center_ray = BuildEditorCameraRay(
        identity_view, projection, camera, 0.0f, 0.0f);
    Require(NearlyEqual(center_ray.direction.x, 0.0f) &&
                NearlyEqual(center_ray.direction.y, 0.0f) &&
                NearlyEqual(center_ray.direction.z, 1.0f),
            "editor center ray does not follow camera forward");
    const EditorRay corner_ray = BuildEditorCameraRay(
        identity_view, projection, camera, 1.0f, -1.0f);
    Require(corner_ray.direction.x > 0.0f &&
                corner_ray.direction.y < 0.0f &&
                corner_ray.direction.z > 0.0f &&
                NearlyEqual(Length(corner_ray.direction), 1.0f),
            "editor off-center ray is not normalized camera-space motion");

    MapDefinition pick_map;
    MapObject farther;
    farther.id = 11;
    farther.name = "Farther";
    farther.render_mesh.vertices = {
        {{-1.0f, -1.0f, 0.0f}},
        {{1.0f, -1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}},
    };
    farther.render_mesh.indices = {0, 1, 2};
    farther.local_bounds_min = {-1.0f, -1.0f, 0.0f};
    farther.local_bounds_max = {1.0f, 1.0f, 0.0f};
    MapObject nearer = farther;
    nearer.id = 12;
    nearer.name = "Nearer";
    pick_map.editable_objects = {farther, nearer};
    const std::array<EditorObjectTransform, 2> transforms = {
        EditorObjectTransform{.translation = {0.0f, 0.0f, 8.0f}},
        EditorObjectTransform{.translation = {0.0f, 0.0f, 5.0f}},
    };
    const MapObjectHit hit =
        PickMapObject(pick_map, transforms, center_ray);
    Require(hit.hit && hit.object_index == 1 &&
                NearlyEqual(hit.distance, 5.0f) &&
                NearlyEqual(hit.point.z, 5.0f),
            "editor picking did not choose the nearest transformed object");
    std::array<EditorObjectTransform, 2> moved_transforms = transforms;
    moved_transforms[1].translation = {4.0f, 0.0f, 5.0f};
    const MapObjectHit moved_hit =
        PickMapObject(pick_map, moved_transforms, center_ray);
    Require(moved_hit.hit && moved_hit.object_index == 0,
            "editor picking retained an object's old transform");

    std::array<EditorObjectTransform, 2> rotated_transforms = transforms;
    rotated_transforms[1].x_axis = {0.0f, 0.0f, -1.0f};
    rotated_transforms[1].z_axis = {1.0f, 0.0f, 0.0f};
    rotated_transforms[1].translation = {-5.0f, 0.0f, 0.0f};
    const EditorRay side_ray{{0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
    const MapObjectHit rotated_hit =
        PickMapObject(pick_map, rotated_transforms, side_ray);
    Require(rotated_hit.hit && rotated_hit.object_index == 1 &&
                NearlyEqual(rotated_hit.distance, 5.0f),
            "editor picking did not follow object rotation");

    Vec3 plane_hit;
    Require(IntersectEditorDragPlane(
                center_ray, {0.0f, 0.0f, 3.0f},
                {0.0f, 0.0f, 1.0f}, plane_hit) &&
                NearlyEqual(plane_hit.z, 3.0f),
            "editor drag-plane intersection is wrong");

    float axis_parameter = 0.0f;
    Require(EditorAxisDragParameter(
                {{2.0f, 1.0f, -5.0f}, {0.0f, 0.0f, 1.0f}},
                {}, {1.0f, 0.0f, 0.0f}, axis_parameter) &&
                NearlyEqual(axis_parameter, 2.0f),
            "editor axis drag parameter is wrong");
    const Vec3 quarter_turn = RotateEditorVector(
        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        3.14159265358979323846f * 0.5f);
    Require(NearlyEqual(quarter_turn.x, 0.0f) &&
                NearlyEqual(quarter_turn.z, -1.0f),
            "editor axis rotation is wrong");
    Require(NearlyEqual(
                EditorSignedRotation(
                    {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
                    {0.0f, 1.0f, 0.0f}),
                3.14159265358979323846f * 0.5f),
            "editor signed rotation is wrong");
    const EditorObjectTransform point_transform{
        .translation = {4.0f, 2.0f, -3.0f},
        .x_axis = {0.0f, 0.0f, -1.0f},
        .y_axis = {0.0f, 1.0f, 0.0f},
        .z_axis = {1.0f, 0.0f, 0.0f},
    };
    const Vec3 transformed_point =
        TransformEditorPoint(point_transform, {2.0f, 1.0f, 0.5f});
    Require(NearlyEqual(transformed_point.x, 4.5f) &&
                NearlyEqual(transformed_point.y, 3.0f) &&
                NearlyEqual(transformed_point.z, -5.0f),
            "editor point transform diverges from the object pose");
    const CollisionTriangle transformed_collision =
        TransformEditorCollisionTriangle(
            point_transform,
            CollisionTriangle{
                .a = {0.0f, 0.0f, 0.0f},
                .b = {2.0f, 0.0f, 0.0f},
                .c = {0.0f, 1.0f, 0.0f},
                .normal = {1.0f, 0.0f, 0.0f},
                .surface = 7,
                .material = 9,
            });
    Require(
        NearlyEqual(transformed_collision.a.x, 4.0f) &&
            NearlyEqual(transformed_collision.a.y, 2.0f) &&
            NearlyEqual(transformed_collision.a.z, -3.0f) &&
            NearlyEqual(transformed_collision.b.z, -5.0f) &&
            NearlyEqual(transformed_collision.c.y, 3.0f) &&
            NearlyEqual(transformed_collision.normal.x, 0.0f) &&
            NearlyEqual(transformed_collision.normal.z, -1.0f) &&
            transformed_collision.surface == 7 &&
            transformed_collision.material == 9,
        "editor collision bake did not preserve transformed geometry and "
        "surface identity");

    MapDefinition grind_map;
    GrindRail authored_rail;
    authored_rail.id = 31;
    authored_rail.name = "AuthoredRail";
    authored_rail.points = {
        {10.0f, 1.0f, 0.0f}, {12.0f, 1.0f, 0.0f}};
    grind_map.grind_rails.push_back(authored_rail);
    MapObject grind_object;
    grind_object.id = 41;
    grind_object.name = "MovableRail";
    grind_object.origin = {10.0f, 0.0f, 0.0f};
    grind_object.grind_rail_indices = {0};
    grind_map.editable_objects.push_back(grind_object);
    const std::array<EditorObjectTransform, 1> grind_transforms = {
        EditorObjectTransform{
            .translation = {20.0f, 3.0f, 4.0f},
            .x_axis = {0.0f, 0.0f, -1.0f},
            .y_axis = {0.0f, 1.0f, 0.0f},
            .z_axis = {1.0f, 0.0f, 0.0f},
        },
    };
    std::vector<GrindRail> transformed_rails =
        TransformEditorGrindRails(grind_map, grind_transforms);
    Require(transformed_rails.size() == 1 &&
                NearlyEqual(transformed_rails[0].points[0].x, 20.0f) &&
                NearlyEqual(transformed_rails[0].points[0].y, 4.0f) &&
                NearlyEqual(transformed_rails[0].points[0].z, 4.0f) &&
                NearlyEqual(transformed_rails[0].points[1].x, 20.0f) &&
                NearlyEqual(transformed_rails[0].points[1].z, 2.0f),
            "editor grind spline did not follow object translation and "
            "rotation");
    grind_map.grind_rails.push_back(
        GrindRail{
            .id = 32,
            .name = "SpawnedRail",
            .points = {{0.0f, 1.0f, 0.0f}, {2.0f, 1.0f, 0.0f}},
        });
    grind_map.editable_objects.push_back(
        MapObject{
            .id = 42,
            .name = "SpawnedObject",
            .grind_rail_indices = {1},
        });
    const std::array<EditorObjectTransform, 2> spawned_transforms = {
        grind_transforms[0],
        EditorObjectTransform{.translation = {30.0f, 5.0f, 6.0f}},
    };
    transformed_rails =
        TransformEditorGrindRails(grind_map, spawned_transforms);
    const GrindSplineBuildResult spawned_grind =
        BuildGrindSplineData(
            MapDefinition{.grind_rails = transformed_rails}, {});
    Require(spawned_grind.ok &&
                spawned_grind.blob.rail_count == 2 &&
                spawned_grind.blob.segment_count == 2 &&
                NearlyEqual(transformed_rails[1].points[0].x, 30.0f) &&
                NearlyEqual(transformed_rails[1].points[0].y, 6.0f) &&
                NearlyEqual(transformed_rails[1].points[0].z, 6.0f),
            "spawned grind spline topology or placement was not rebuilt");

    const EditorGizmoHit gizmo_hit = PickEditorGizmo(
        {{0.5f, 0.03f, -5.0f}, {0.0f, 0.0f, 1.0f}}, {}, 1.0f);
    Require(gizmo_hit.handle == EditorGizmoHandle::TranslateX,
            "editor gizmo did not pick the X translation arrow");
  }

  {
    MapDefinition split_map;
    split_map.name = "editor_exclusion_sync";
    SurfaceMaterial split_material;
    split_material.id = 1;
    split_material.name = "EditorSplit";
    split_map.materials.push_back(split_material);
    split_map.render_mesh.vertices = {
        {{-1.0f, 0.0f, 0.0f}, {}, {}, 1},
        {{0.0f, 0.0f, 1.0f}, {}, {}, 1},
        {{1.0f, 0.0f, 0.0f}, {}, {}, 1},
        {{-1.0f, 1.0f, 0.0f}, {}, {}, 1},
        {{0.0f, 1.0f, 1.0f}, {}, {}, 1},
        {{1.0f, 1.0f, 0.0f}, {}, {}, 1},
    };
    split_map.render_mesh.indices = {0, 1, 2, 3, 4, 5};
    split_map.collision_triangles = {
        {{-1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, 1.0f},
         {1.0f, 0.0f, 0.0f},
         {},
         0,
         1},
        {{-1.0f, 1.0f, 0.0f},
         {0.0f, 1.0f, 1.0f},
         {1.0f, 1.0f, 0.0f},
         {},
         0,
         1},
    };
    RenderWorldBuildOptions render_options;
    render_options.excluded_index_ranges.push_back({3, 3});
    const RenderWorld static_render =
        BuildRenderWorld(split_map, render_options);
    Require(static_render.source_triangle_count == 1 &&
                static_render.output_triangle_count > 0 &&
                std::all_of(
                    static_render.chunks.begin(),
                    static_render.chunks.end(),
                    [](const RenderChunk& chunk) {
                      return std::all_of(
                          chunk.vertices.begin(), chunk.vertices.end(),
                          [](const RenderVertex& vertex) {
                            return NearlyEqual(vertex.position.y, 0.0f);
                          });
                    }),
            "editable render span remained in the static world");

    MapDefinition fully_editable_map = split_map;
    fully_editable_map.render_mesh.vertices.resize(4);
    fully_editable_map.render_mesh.vertices[0].position =
        {-1.0f, 0.0f, -1.0f};
    fully_editable_map.render_mesh.vertices[1].position =
        {1.0f, 0.0f, -1.0f};
    fully_editable_map.render_mesh.vertices[2].position =
        {1.0f, 0.0f, 1.0f};
    fully_editable_map.render_mesh.vertices[3].position =
        {-1.0f, 0.0f, 1.0f};
    fully_editable_map.render_mesh.indices = {
        0, 1, 2,
        0, 2, 3,
    };
    std::size_t completed_render_triangles = 1;
    std::size_t total_render_triangles = 1;
    RenderWorldBuildOptions fully_editable_options;
    fully_editable_options.excluded_index_ranges.push_back({0, 6});
    fully_editable_options.progress =
        [&](std::size_t completed, std::size_t total) {
          completed_render_triangles = completed;
          total_render_triangles = total;
        };
    const RenderWorld fully_editable_render =
        BuildRenderWorld(fully_editable_map, fully_editable_options);
    Require(
        fully_editable_render.source_triangle_count == 0 &&
            fully_editable_render.output_triangle_count == 0 &&
            fully_editable_render.presentation_surface_count == 0 &&
            fully_editable_render.chunks.empty() &&
            completed_render_triangles == 0 &&
            total_render_triangles == 0,
        "fully editable shared-vertex geometry was not cleanly excluded "
        "from the static render world");

    RwCollisionBuildOptions collision_options;
    collision_options.excluded_triangle_ranges.push_back({1, 1});
    const RwCollisionBuildResult static_collision =
        BuildRwCollisionMesh(split_map, collision_options);
    Require(static_collision.ok &&
                static_collision.mesh.triangle_count == 1,
            "editable collision span remained in the static broadphase");
  }

  ShallowWaterConfig water_config;
  water_config.minimum = {-4.0f, -3.0f};
  water_config.maximum = {4.0f, 3.0f};
  water_config.columns = 41;
  water_config.rows = 31;
  water_config.rest_surface_height = -0.3f;
  water_config.rest_depth = 1.2f;
  ShallowWaterSimulation water(water_config);
  WaterObstacle water_pusher;
  water_pusher.center = {-2.0f, 0.0f};
  water_pusher.half_extents = {0.3f, 1.0f};
  constexpr float water_step = 1.0f / 120.0f;
  for (int step = 0; step < 120; ++step) {
    Require(water.Step(water_step, water_pusher),
            "stationary water step failed");
  }
  WaterStatistics water_stats = water.Statistics();
  Require(NearlyEqual(water_stats.minimum_displacement, 0.0f) &&
              NearlyEqual(water_stats.maximum_displacement, 0.0f) &&
              NearlyEqual(water_stats.kinetic_energy, 0.0f),
          "stationary obstacle disturbed calm water");
  std::vector<float> previous_coverage;
  previous_coverage.reserve(
      static_cast<std::size_t>(water.Columns()) * water.Rows());
  std::size_t fractional_samples = 0;
  for (std::uint32_t row = 0; row < water.Rows(); ++row) {
    for (std::uint32_t column = 0;
         column < water.Columns(); ++column) {
      const float coverage =
          water.ObstacleCoverage(column, row);
      previous_coverage.push_back(coverage);
      if (coverage > 0.001f && coverage < 0.999f) {
        ++fractional_samples;
      }
    }
  }
  Require(fractional_samples > 0,
          "water obstacle coverage still snaps to binary cells");
  water_pusher.center.x += water.CellSizeX() * 0.20f;
  Require(water.Step(water_step, water_pusher),
          "sub-cell water step failed");
  float maximum_coverage_delta = 0.0f;
  std::size_t coverage_index = 0;
  for (std::uint32_t row = 0; row < water.Rows(); ++row) {
    for (std::uint32_t column = 0;
         column < water.Columns(); ++column) {
      maximum_coverage_delta = std::max(
          maximum_coverage_delta,
          std::abs(water.ObstacleCoverage(column, row) -
                   previous_coverage[coverage_index++]));
    }
  }
  Require(maximum_coverage_delta > 0.01f &&
              maximum_coverage_delta < 0.35f,
          "sub-cell obstacle motion produced a grid-sized jump");
  water_pusher.velocity = {0.8f, 0.0f};
  for (int step = 0; step < 479; ++step) {
    water_pusher.center.x +=
        water_pusher.velocity.x * water_step;
    Require(water.Step(water_step, water_pusher),
            "moving water step failed");
  }
  const float previous_probe_height =
      water.SurfaceHeight(20, 15);
  water_pusher.center.x +=
      water_pusher.velocity.x * water_step;
  Require(water.Step(water_step, water_pusher),
          "final moving water step failed");
  water_stats = water.Statistics();
  Require(water_stats.maximum_displacement > 0.01f &&
              water_stats.minimum_displacement < -0.01f &&
              std::abs(water_stats.mean_displacement) < 1.0e-4f &&
              water_stats.kinetic_energy > 0.01f,
          "moving obstacle did not transfer momentum into water");
  const float current_probe_height =
      water.SurfaceHeight(20, 15);
  const float interpolated_start =
      water.InterpolatedSurfaceHeight(20, 15, 0.0f);
  const float interpolated_mid =
      water.InterpolatedSurfaceHeight(20, 15, 0.5f);
  const float interpolated_end =
      water.InterpolatedSurfaceHeight(20, 15, 1.0f);
  Require(NearlyEqual(interpolated_start, previous_probe_height) &&
              NearlyEqual(interpolated_end, current_probe_height) &&
              NearlyEqual(
                  interpolated_mid,
                  (interpolated_start + interpolated_end) * 0.5f),
          "water height interpolation endpoints are wrong");
  for (std::uint32_t row = 0; row < water.Rows(); ++row) {
    for (std::uint32_t column = 0;
         column < water.Columns(); ++column) {
      const float height = water.SurfaceHeight(column, row);
      const Vec3 normal = water.SurfaceNormal(column, row);
      const Vec3 interpolated_normal =
          water.InterpolatedSurfaceNormal(column, row, 0.5f);
      Require(std::isfinite(height) &&
                  std::isfinite(normal.x) &&
                  std::isfinite(normal.y) &&
                  std::isfinite(normal.z) &&
                  normal.y > 0.0f &&
                  std::isfinite(interpolated_normal.x) &&
                  std::isfinite(interpolated_normal.y) &&
                  std::isfinite(interpolated_normal.z) &&
                  interpolated_normal.y > 0.0f,
              "water surface became non-finite");
    }
  }

  WorldMap world(MakeStarterFlatgroundMap());
  const MapDefinition& definition = world.Definition();

  Require(definition.name == "neon_foundry_day_cycle",
          "starter map name changed");
  Require(definition.materials.size() == 22,
          "starter map material palette changed");
  Require(std::count_if(
              definition.materials.begin(), definition.materials.end(),
              [](const SurfaceMaterial& material) {
                return material.emissive_intensity > 0.0f;
              }) == 5,
          "starter map emissive city palette changed");
  Require(definition.grind_rails.size() == 7,
          "starter map grind rail count changed");
  Require(definition.kinematic_boxes.size() == 1,
          "starter map kinematic object count changed");
  Require(definition.water_basins.size() == 1 &&
              definition.water_basins.front().name ==
                  "west_wave_basin" &&
              definition.water_basins.front().columns == 57 &&
              definition.water_basins.front().rows == 57,
          "starter map water basin is missing");
  Require(definition.raytraced_mirrors.size() == 1,
          "starter map raytraced mirror count changed");
  Require(!definition.weather.enabled &&
              NearlyEqual(definition.weather.rain_intensity, 0.0f),
          "starter map weather should be disabled");
  Require(definition.day_night_cycle.enabled &&
              NearlyEqual(
                  definition.day_night_cycle.duration_seconds, 96.0f) &&
              NearlyEqual(
                  definition.day_night_cycle.start_time_hours, 9.0f),
          "starter map day/night contract changed");
  const DayNightState morning =
      EvaluateDayNightCycle(definition.day_night_cycle, 0.0f);
  const DayNightState midnight =
      EvaluateDayNightCycle(definition.day_night_cycle, 60.0f);
  const DayNightState wrapped_morning =
      EvaluateDayNightCycle(definition.day_night_cycle, 96.0f);
  Require(NearlyEqual(morning.time_of_day_hours, 9.0f) &&
              morning.sun_direction_to_light.y > 0.70f &&
              morning.sun_is_key_light &&
              morning.daylight_amount > 0.99f &&
              NearlyEqual(midnight.time_of_day_hours, 0.0f) &&
              midnight.moon_direction_to_light.y > 0.99f &&
              !midnight.sun_is_key_light &&
              midnight.night_amount > 0.99f &&
              NearlyEqual(wrapped_morning.time_of_day_hours, 9.0f),
          "starter map day/night evaluation changed");
  DayNightCycleDefinition frozen_golden_hour =
      definition.day_night_cycle;
  frozen_golden_hour.duration_seconds = 0.0f;
  frozen_golden_hour.start_time_hours = 17.5f;
  const DayNightState frozen_at_start =
      EvaluateDayNightCycle(frozen_golden_hour, 0.0f);
  const DayNightState frozen_much_later =
      EvaluateDayNightCycle(frozen_golden_hour, 100000.0f);
  Require(NearlyEqual(frozen_at_start.time_of_day_hours, 17.5f) &&
              NearlyEqual(frozen_much_later.time_of_day_hours, 17.5f) &&
              NearlyEqual(frozen_much_later.elapsed_seconds, 0.0f) &&
              frozen_at_start.sun_is_key_light &&
              frozen_at_start.twilight_amount > 0.5f,
          "frozen golden-hour evaluation changed");
  DayNightCycleDefinition daylight_ping_pong =
      definition.day_night_cycle;
  daylight_ping_pong.ping_pong = true;
  daylight_ping_pong.duration_seconds = 120.0f;
  daylight_ping_pong.start_time_hours = 17.5f;
  daylight_ping_pong.end_time_hours = 7.0f;
  const DayNightState ping_evening =
      EvaluateDayNightCycle(daylight_ping_pong, 0.0f);
  const DayNightState ping_morning =
      EvaluateDayNightCycle(daylight_ping_pong, 60.0f);
  const DayNightState ping_return =
      EvaluateDayNightCycle(daylight_ping_pong, 120.0f);
  const DayNightState ping_midday =
      EvaluateDayNightCycle(daylight_ping_pong, 30.0f);
  Require(NearlyEqual(ping_evening.time_of_day_hours, 17.5f) &&
              NearlyEqual(ping_morning.time_of_day_hours, 7.0f) &&
              NearlyEqual(ping_return.time_of_day_hours, 17.5f) &&
              ping_midday.time_of_day_hours > 7.0f &&
              ping_midday.time_of_day_hours < 17.5f &&
              ping_evening.sun_is_key_light &&
              ping_morning.sun_is_key_light &&
              ping_midday.sun_is_key_light &&
              ping_evening.night_amount < 0.01f &&
              ping_morning.night_amount < 0.01f,
          "daylight ping-pong evaluation changed");
  DayNightCycleDefinition zero_ambient_cycle =
      definition.day_night_cycle;
  zero_ambient_cycle.day_ambient = 0.0f;
  zero_ambient_cycle.night_ambient = 0.0f;
  for (int second = 0; second < 96; ++second) {
    Require(
        NearlyEqual(
            EvaluateDayNightCycle(
                zero_ambient_cycle, static_cast<float>(second))
                .ambient,
            0.0f),
        "zero day/night ambient settings must produce zero ambient light");
  }
  Require(definition.raytraced_puddles.size() == 3,
          "starter map raytraced puddle count changed");
  const RaytracedPuddle& demo_puddle =
      definition.raytraced_puddles.front();
  Require(demo_puddle.name == "mirror_court_wide_puddle" &&
              NearlyEqual(demo_puddle.center.x, -29.0f) &&
              NearlyEqual(demo_puddle.center.y, 0.061f) &&
              NearlyEqual(demo_puddle.half_width, 15.5f) &&
              NearlyEqual(demo_puddle.reflectivity, 0.72f),
          "starter map raytraced puddle contract changed");
  const RaytracedMirror& demo_mirror =
      definition.raytraced_mirrors.front();
  Require(demo_mirror.name == "spawn_industrial_mirror" &&
              NearlyEqual(demo_mirror.center.x, -14.0f) &&
              NearlyEqual(demo_mirror.center.y, 3.30f) &&
              NearlyEqual(demo_mirror.center.z, -28.0f) &&
              NearlyEqual(demo_mirror.half_width, 6.0f) &&
              NearlyEqual(demo_mirror.half_height, 3.0f) &&
              NearlyEqual(demo_mirror.right.z, 1.0f) &&
              NearlyEqual(demo_mirror.up.y, 1.0f),
          "starter map raytraced mirror contract changed");
  Require(definition.moving_light_orbs.size() == 3,
          "starter map moving light count changed");
  const MovingLightOrb& demo_light =
      definition.moving_light_orbs.front();
  const MovingLightOrbPose light_start =
      EvaluateMovingLightOrb(demo_light, 0.0f);
  const MovingLightOrbPose light_quarter =
      EvaluateMovingLightOrb(
          demo_light, demo_light.period_seconds * 0.25f);
  Require(demo_light.name == "mirror_cyan_orb" &&
              NearlyEqual(light_start.position.x, -18.2f) &&
              NearlyEqual(light_start.position.y, 3.35f) &&
              NearlyEqual(light_quarter.position.y, 4.60f) &&
              light_start.velocity.y > 0.0f,
          "starter map moving light contract changed");
  const WaterBasin& demo_basin = definition.water_basins.front();
  ShallowWaterConfig demo_water_config;
  demo_water_config.minimum = {
      demo_basin.minimum.x, demo_basin.minimum.z};
  demo_water_config.maximum = {
      demo_basin.maximum.x, demo_basin.maximum.z};
  demo_water_config.columns = demo_basin.columns;
  demo_water_config.rows = demo_basin.rows;
  demo_water_config.rest_surface_height =
      demo_basin.rest_surface_height;
  demo_water_config.rest_depth =
      demo_basin.rest_surface_height - demo_basin.minimum.y;
  demo_water_config.linear_damping = demo_basin.damping;
  demo_water_config.maximum_displacement = 0.55f;
  ShallowWaterSimulation demo_water(demo_water_config);
  KinematicBox demo_pusher;
  demo_pusher.local_min = demo_basin.pusher_local_min;
  demo_pusher.local_max = demo_basin.pusher_local_max;
  demo_pusher.path_start = demo_basin.pusher_path_start;
  demo_pusher.path_end = demo_basin.pusher_path_end;
  demo_pusher.travel_seconds =
      demo_basin.pusher_travel_seconds;
  constexpr float demo_water_step = 1.0f / 240.0f;
  for (int step = 0; step < 7200; ++step) {
    const KinematicPose pose = EvaluateKinematicBox(
        demo_pusher,
        static_cast<float>(step) * demo_water_step);
    WaterObstacle obstacle;
    obstacle.center = {pose.position.x, pose.position.z};
    obstacle.half_extents = {
        (demo_pusher.local_max.x - demo_pusher.local_min.x) * 0.5f,
        (demo_pusher.local_max.z - demo_pusher.local_min.z) * 0.5f,
    };
    obstacle.velocity = {pose.velocity.x, pose.velocity.z};
    Require(demo_water.Step(demo_water_step, obstacle),
            "authored basin water step failed");
  }
  const WaterStatistics demo_water_stats =
      demo_water.Statistics();
  Require(demo_water_stats.maximum_displacement > 0.03f &&
              demo_water_stats.maximum_displacement <= 0.551f &&
              demo_water_stats.minimum_displacement >= -0.551f &&
              std::abs(demo_water_stats.mean_displacement) < 1.0e-4f &&
              std::isfinite(demo_water_stats.kinetic_energy),
          "authored basin water is overdriven or unstable");
  Require(definition.kinematic_boxes.front().name ==
              "spawn_shuttle_platform",
          "spawn moving platform is missing");
  Require(definition.grind_rails.front().name ==
              "spawn_practice_rail" &&
              definition.grind_rails.front().points.size() == 2,
          "spawn practice rail is missing");
  Require(NearlyEqual(definition.spawn.position.x, -38.0f) &&
              NearlyEqual(definition.spawn.position.z, -14.0f),
          "grind-test spawn changed");
  Require(definition.materials[0].pattern == MaterialPattern::Concrete &&
              definition.materials[3].pattern == MaterialPattern::Asphalt &&
              definition.materials[9].pattern == MaterialPattern::Wood,
          "starter map procedural material families are missing");
  Require(!definition.render_mesh.vertices.empty(),
          "starter map has no render vertices");
  Require(definition.render_mesh.indices.size() % 3 == 0,
          "render indices are not triangles");
  Require(!definition.collision_triangles.empty(),
          "starter map has no collision triangles");
  Require(definition.sky.enabled, "starter map sky is disabled");
  Require(definition.sky.horizon_color.z > definition.sky.horizon_color.x,
          "starter map sky is not blue");
  Require(NearlyEqual(Length(definition.sun.direction_to_light), 1.0f),
          "starter map sun direction is not normalized");
  Require(definition.sun.intensity > 1.0f &&
              definition.sun.intensity < 1.5f &&
              definition.sun.ambient > 0.25f &&
              definition.sun.ambient < 0.4f,
          "starter map daylight fallback is not configured");

  const KinematicBox& moving_platform =
      definition.kinematic_boxes.front();
  const KinematicPose platform_start =
      EvaluateKinematicBox(moving_platform, 0.0f);
  const KinematicPose platform_middle =
      EvaluateKinematicBox(moving_platform, 2.25f);
  const KinematicPose platform_end =
      EvaluateKinematicBox(moving_platform, 4.5f);
  const KinematicPose platform_return =
      EvaluateKinematicBox(moving_platform, 6.75f);
  Require(NearlyEqual(platform_start.position.x, -42.0f) &&
              NearlyEqual(platform_start.velocity.x, 0.0f),
          "moving platform start pose is wrong");
  Require(NearlyEqual(platform_middle.position.x, -36.0f) &&
              platform_middle.velocity.x > 0.0f,
          "moving platform outbound pose is wrong");
  Require(NearlyEqual(platform_end.position.x, -30.0f) &&
              NearlyEqual(platform_end.velocity.x, 0.0f) &&
              platform_end.returning,
          "moving platform turn pose is wrong");
  Require(NearlyEqual(platform_return.position.x, -36.0f) &&
              platform_return.velocity.x < 0.0f &&
              platform_return.returning,
          "moving platform return pose is wrong");
  Require(NearlyEqual(moving_platform.local_min.y, -0.10f) &&
              NearlyEqual(moving_platform.local_max.y, 1.40f),
          "moving platform height is wrong");

  const RenderWorld render_world = BuildRenderWorld(definition);
  Require(render_world.chunk_size == 64.0f,
          "render-world chunk size changed");
  Require(render_world.chunks.size() > 1,
          "large starter floor was not spatially chunked");
  Require(render_world.source_triangle_count ==
              definition.render_mesh.indices.size() / 3,
          "render-world source triangle count is wrong");
  std::size_t compiled_indices = 0;
  for (const RenderChunk& chunk : render_world.chunks) {
    Require(!chunk.vertices.empty() && !chunk.indices.empty(),
            "render-world emitted an empty chunk");
    Require(chunk.vertices.size() <= 65535,
            "render-world exceeded the 16-bit adapter limit");
    Require(chunk.indices.size() % 3 == 0,
            "render-world chunk indices are not triangles");
    Require(chunk.bounds_max.x - chunk.bounds_min.x <=
                render_world.chunk_size + 1.0e-3f &&
                chunk.bounds_max.z - chunk.bounds_min.z <=
                    render_world.chunk_size + 1.0e-3f,
            "render-world chunk escaped its spatial cell");
    std::size_t batch_end = 0;
    for (const RenderBatch& batch : chunk.batches) {
      Require(batch.first_index == batch_end &&
                  batch.index_count > 0 &&
                  batch.index_count % 3 == 0,
              "render-world material batches are not contiguous triangles");
      for (std::size_t index = batch.first_index;
           index < batch.first_index + batch.index_count; ++index) {
        Require(chunk.indices[index] < chunk.vertices.size(),
                "render-world chunk index is invalid");
        Require(chunk.vertices[chunk.indices[index]].material ==
                    batch.material,
                "render-world batch material does not match its vertices");
      }
      batch_end += batch.index_count;
    }
    Require(batch_end == chunk.indices.size(),
            "render-world batches do not cover the chunk");
    compiled_indices += chunk.indices.size();
  }
  Require(compiled_indices == render_world.output_triangle_count * 3,
          "render-world output triangle accounting is wrong");

  const SurfaceMaterial* concrete = world.FindMaterial(1);
  Require(concrete != nullptr, "concrete material is missing");
  Require(concrete->name == "warm_plaza_concrete",
          "concrete material name changed");
  Require(HasFlag(concrete->flags, SurfaceFlags::Skateable),
          "concrete must remain skateable");

  const RayHit floor =
      world.ProbeGround({-36.0f, 4.0f, -3.0f}, 8.0f);
  Require(floor.hit, "ground probe missed the floor");
  Require(floor.material == 11,
          "ground probe returned wrong spawn-lane material");
  Require(NearlyEqual(floor.point.y, 0.035f),
          "floor height is incorrect");
  Require(floor.normal.y > 0.99f, "floor normal is not upward");

  const RayHit spawn_bank =
      world.ProbeGround({-17.5f, 4.0f, 0.0f}, 8.0f);
  Require(spawn_bank.hit,
          "ground probe missed spawn-line bank");
  Require(spawn_bank.material == 14,
          "spawn-line bank returned wrong material");
  Require(NearlyEqual(spawn_bank.point.y, 0.675f),
          "spawn-line bank interpolation is incorrect");
  Require(spawn_bank.normal.y > 0.9f,
          "spawn-line bank normal is invalid");

  const RayHit manual_pad =
      world.ProbeGround({16.5f, 4.0f, 0.0f}, 8.0f);
  Require(manual_pad.hit && manual_pad.material == 3,
          "ground probe missed central manual pad");
  Require(NearlyEqual(manual_pad.point.y, 0.42f),
          "central manual pad height is incorrect");
  const RayHit ramp_recovery_floor =
      world.ProbeLowestSkateableGround({-17.5f, 4.0f, 0.0f}, 8.0f);
  Require(ramp_recovery_floor.hit,
          "lowest-ground probe missed floor below ramp");
  Require(NearlyEqual(ramp_recovery_floor.point.y, 0.0f),
          "lowest-ground probe did not select floor below ramp");

  const RayHit planter =
      world.ProbeGround({-10.0f, 4.0f, 30.0f}, 8.0f);
  Require(planter.hit && planter.material == 12,
          "ground probe missed planted island");
  Require(NearlyEqual(planter.point.y, 0.82f),
          "planted island height is incorrect");
  const RayHit pad_recovery_floor =
      world.ProbeLowestSkateableGround({-10.0f, 4.0f, 30.0f}, 8.0f);
  Require(pad_recovery_floor.hit,
          "lowest-ground probe missed floor below manual pad");
  Require(NearlyEqual(pad_recovery_floor.point.y, 0.0f),
          "lowest-ground probe selected a box face instead of floor");

  const std::vector<Contact> contacts =
      world.QuerySphere({-36.0f, 0.135f, -3.0f}, 0.2f);
  Require(!contacts.empty(), "sphere query missed the floor");
  Require(contacts.front().material == 11,
          "sphere query returned wrong material");
  Require(NearlyEqual(contacts.front().penetration, 0.1f),
          "sphere penetration is incorrect");

  const RayHit apron =
      world.ProbeGround({150.0f, 4.0f, 150.0f}, 8.0f);
  Require(apron.hit && apron.material == 4,
          "extended district foundation is missing");
  Require(NearlyEqual(apron.point.y, 0.0f),
          "extended apron height is incorrect");

  const RayHit miss =
      world.ProbeGround({1000.0f, 4.0f, 1000.0f}, 8.0f);
  Require(!miss.hit, "ground probe hit outside the map");

  GrindSplineBuildResult grind =
      BuildGrindSplineData(definition, {100.0f, 20.0f, -40.0f});
  Require(grind.ok, grind.error);
  Require(grind.blob.rail_count == definition.grind_rails.size() &&
              grind.blob.segment_count == definition.grind_rails.size(),
          "grind spline rail/segment accounting is wrong");
  Require(grind.blob.bytes.size() ==
              16 + grind.blob.rail_count * 32 +
                  grind.blob.segment_count * 144,
          "grind spline blob size is wrong");
  Require(ReadBeU32(grind.blob.bytes, 0) == 7 &&
              ReadBeU32(grind.blob.bytes, 4) == 7 &&
              ReadBeU32(grind.blob.bytes, 8) == 16 &&
              ReadBeU32(grind.blob.bytes, 12) == 16 + 7 * 32,
          "grind spline header is wrong");
  Require(ReadBeU64(grind.blob.bytes, 16 + 8) ==
              0x2C7017070007004Aull,
          "grind spline type signature is wrong");
  const std::uint32_t first_segment =
      ReadBeU32(grind.blob.bytes, 16 + 20);
  Require(first_segment == 16 + 7 * 32 &&
              ReadBeU32(grind.blob.bytes, 16 + 24) == first_segment,
          "grind spline rail links are wrong");
  Require(NearlyEqual(ReadBeF32(grind.blob.bytes, first_segment),
                      21.0f) &&
              NearlyEqual(ReadBeF32(grind.blob.bytes,
                                    first_segment + 48),
                          72.0f) &&
              NearlyEqual(ReadBeF32(grind.blob.bytes,
                                    first_segment + 52),
                          20.395f) &&
              NearlyEqual(ReadBeF32(grind.blob.bytes,
                                    first_segment + 56),
                          -54.0f),
          "grind spline segment vectors are wrong");
  Require(ReadBeU32(grind.blob.bytes, first_segment + 120) == 16 &&
              ReadBeU32(grind.blob.bytes, first_segment + 124) == 0 &&
              ReadBeU32(grind.blob.bytes, first_segment + 128) == 0,
          "grind spline segment links are wrong");

  const std::uint32_t grind_guest_base = 0x51000000u;
  std::vector<std::uint8_t> fixed_grind = grind.blob.bytes;
  Require(FixupGrindSplineDataForGuest(
              fixed_grind, grind_guest_base),
          "grind spline guest fixup failed");
  Require(ReadBeU32(fixed_grind, 8) == grind_guest_base + 16 &&
              ReadBeU32(fixed_grind, 12) ==
                  grind_guest_base + 16 + 7 * 32 &&
              ReadBeU32(fixed_grind, 16 + 20) ==
                  grind_guest_base + first_segment &&
              ReadBeU32(fixed_grind, first_segment + 120) ==
                  grind_guest_base + 16,
          "grind spline guest pointers are wrong");
  Require(!FixupGrindSplineDataForGuest(
              fixed_grind, grind_guest_base),
          "repeated grind spline fixup was not rejected");

  MapDefinition native_grind_definition;
  GrindRail native_rail;
  native_rail.id = 1;
  native_rail.name = "retail_cubic";
  native_rail.retail_spline_id = 0x1122334455667788ull;
  native_rail.retail_type_signature = 0x8877665544332211ull;
  native_rail.retail_flags = 0x12345678u;
  native_rail.retail_trailing_word = 0x13572468u;
  native_rail.native_segments.resize(1);
  NativeGrindSegment& native_segment =
      native_rail.native_segments.front();
  const auto FloatWord = [](float value) {
    return std::bit_cast<std::uint32_t>(value);
  };
  native_segment.words[0] = FloatWord(3.5f);
  native_segment.words[12] = FloatWord(1.0f);
  native_segment.words[13] = FloatWord(2.0f);
  native_segment.words[14] = FloatWord(3.0f);
  native_segment.words[15] = FloatWord(1.0f);
  native_segment.words[16] = FloatWord(0.25f);
  native_segment.words[20] = FloatWord(-4.0f);
  native_segment.words[21] = FloatWord(-5.0f);
  native_segment.words[22] = FloatWord(-6.0f);
  native_segment.words[24] = FloatWord(7.0f);
  native_segment.words[25] = FloatWord(8.0f);
  native_segment.words[26] = FloatWord(9.0f);
  native_grind_definition.grind_rails.push_back(
      std::move(native_rail));

  GrindSplineBuildResult native_grind = BuildGrindSplineData(
      native_grind_definition, {10.0f, 20.0f, 30.0f});
  Require(native_grind.ok, native_grind.error);
  Require(native_grind.blob.rail_count == 1 &&
              native_grind.blob.segment_count == 1 &&
              native_grind.blob.bytes.size() == 16 + 32 + 144,
          "native grind spline accounting is wrong");
  Require(ReadBeU64(native_grind.blob.bytes, 16) ==
                  0x1122334455667788ull &&
              ReadBeU64(native_grind.blob.bytes, 24) ==
                  0x8877665544332211ull &&
              ReadBeU32(native_grind.blob.bytes, 32) == 0x12345678u &&
              ReadBeU32(native_grind.blob.bytes, 44) == 0x13572468u,
          "native grind rail metadata was not preserved");
  const std::size_t native_segment_offset = 16 + 32;
  Require(
      NearlyEqual(
          ReadBeF32(native_grind.blob.bytes, native_segment_offset),
          3.5f) &&
          NearlyEqual(
              ReadBeF32(
                  native_grind.blob.bytes,
                  native_segment_offset + 48),
              11.0f) &&
          NearlyEqual(
              ReadBeF32(
                  native_grind.blob.bytes,
                  native_segment_offset + 52),
              22.0f) &&
          NearlyEqual(
              ReadBeF32(
                  native_grind.blob.bytes,
                  native_segment_offset + 56),
              33.0f) &&
          NearlyEqual(
              ReadBeF32(
                  native_grind.blob.bytes,
                  native_segment_offset + 64),
              0.25f) &&
          NearlyEqual(
              ReadBeF32(
                  native_grind.blob.bytes,
                  native_segment_offset + 80),
              6.0f) &&
          NearlyEqual(
              ReadBeF32(
                  native_grind.blob.bytes,
                  native_segment_offset + 96),
              17.0f),
      "native grind payload did not preserve coefficients and translate "
      "positions/bounds");
  Require(
      ReadBeU32(
          native_grind.blob.bytes,
          native_segment_offset + 120) == 16 &&
          ReadBeU32(
              native_grind.blob.bytes,
              native_segment_offset + 124) == 0 &&
          ReadBeU32(
              native_grind.blob.bytes,
              native_segment_offset + 128) == 0,
      "native grind links were not regenerated");

  RwCollisionBuildOptions rw_options;
  rw_options.default_surface_id = EncodeRwSurfaceId(0, 1, 0);
  rw_options.material_surface_ids.emplace(
      1, EncodeRwSurfaceId(3, 4, 2));
  RwCollisionBuildResult rw =
      BuildRwCollisionMesh(definition, rw_options);
  Require(rw.ok, rw.error);
  Require(rw.mesh.bytes.size() >= 176,
          "RenderWare collision blob is too small");
  Require(rw.mesh.triangle_count ==
              definition.collision_triangles.size(),
          "RenderWare collision triangle count changed");
  Require(rw.mesh.vertex_count > 255 &&
              rw.mesh.maximum_cluster_vertex_count <= 255,
          "large collision mesh was not partitioned into bounded clusters");
  Require(ReadBeU32(rw.mesh.bytes, 40) ==
              rw.mesh.triangle_count,
          "RenderWare aggregate unit count is wrong");
  Require(ReadBeU32(rw.mesh.bytes, 48) == 96,
          "RenderWare KD-tree offset is wrong");
  const std::uint32_t kd_branch_count =
      ReadBeU32(rw.mesh.bytes, 96 + 4);
  Require(kd_branch_count > 0,
          "large map must have real KD branch nodes");
  Require(ReadBeU32(rw.mesh.bytes, 96) == 96 + 48,
          "RenderWare KD branch-array offset is wrong");
  const std::uint32_t cluster_table_offset =
      ReadBeU32(rw.mesh.bytes, 52);
  Require(cluster_table_offset ==
              ((96 + 48 + kd_branch_count * 32 + 15) & ~15u),
          "RenderWare cluster table offset is wrong");
  const std::uint32_t cluster_count =
      ReadBeU32(rw.mesh.bytes, 64);
  Require(cluster_count == rw.mesh.cluster_count &&
              cluster_count > 1,
          "RenderWare cluster count is wrong");
  Require(ReadBeU32(rw.mesh.bytes, 80) == rw.mesh.bytes.size(),
          "RenderWare mesh size field is wrong");
  Require(rw.mesh.bytes[88] == 0x80,
          "RenderWare one-sided mesh marker is wrong");
  const std::uint32_t first_cluster_offset =
      ReadBeU32(rw.mesh.bytes, cluster_table_offset);
  Require(first_cluster_offset ==
              ((cluster_table_offset + cluster_count * 4 + 15) & ~15u),
          "RenderWare first cluster offset is wrong");
  std::vector<std::uint16_t> cluster_units(cluster_count);
  std::uint32_t serialized_units = 0;
  bool found_concrete_surface = false;
  for (std::uint32_t cluster = 0;
       cluster < cluster_count; ++cluster) {
    const std::uint32_t cluster_offset =
        ReadBeU32(rw.mesh.bytes,
                  cluster_table_offset + cluster * 4);
    Require(cluster_offset < rw.mesh.bytes.size() &&
                (cluster_offset & 0x0fu) == 0,
            "RenderWare cluster offset is invalid");
    const std::uint16_t unit_count =
        ReadBeU16(rw.mesh.bytes, cluster_offset);
    const std::uint16_t unit_bytes =
        ReadBeU16(rw.mesh.bytes, cluster_offset + 2);
    const std::uint16_t vertex_count =
        ReadBeU16(rw.mesh.bytes, cluster_offset + 4);
    const std::uint16_t cluster_bytes =
        ReadBeU16(rw.mesh.bytes, cluster_offset + 8);
    Require(unit_count > 0 &&
                unit_bytes == unit_count * 9 &&
                vertex_count <= 255 &&
                cluster_bytes > 0 &&
                cluster_offset + cluster_bytes <= rw.mesh.bytes.size(),
            "RenderWare cluster header is invalid");
    cluster_units[cluster] = unit_count;
    serialized_units += unit_count;
    const std::size_t first_unit =
        cluster_offset + 16 +
        static_cast<std::size_t>(vertex_count) * 16;
    for (std::uint32_t triangle = 0;
         triangle < unit_count; ++triangle) {
      const std::size_t unit =
          first_unit + static_cast<std::size_t>(triangle) * 9;
      Require(rw.mesh.bytes.at(unit) == 0xa1,
              "RenderWare triangle unit flags are wrong");
      found_concrete_surface |=
          ReadLeU16(rw.mesh.bytes, unit + 7) ==
          EncodeRwSurfaceId(3, 4, 2);
    }
  }
  Require(serialized_units == rw.mesh.triangle_count,
          "RenderWare clusters do not contain every triangle");
  Require(found_concrete_surface,
          "RenderWare material surface ID is wrong");

  MapDefinition retail_edge_definition;
  CollisionTriangle retail_edge_triangle;
  retail_edge_triangle.a = {0.0f, 0.0f, 0.0f};
  retail_edge_triangle.b = {1.0f, 0.0f, 0.0f};
  retail_edge_triangle.c = {0.0f, 0.0f, 1.0f};
  retail_edge_triangle.native_edge_codes = {0x1a, 0x5a, 0x62};
  retail_edge_triangle.has_native_edge_codes = true;
  retail_edge_definition.collision_triangles.push_back(retail_edge_triangle);
  const RwCollisionBuildResult retail_edge_mesh =
      BuildRwCollisionMesh(retail_edge_definition);
  Require(retail_edge_mesh.ok, retail_edge_mesh.error);
  const std::uint32_t retail_cluster_table =
      ReadBeU32(retail_edge_mesh.mesh.bytes, 52);
  const std::uint32_t retail_cluster =
      ReadBeU32(retail_edge_mesh.mesh.bytes, retail_cluster_table);
  const std::uint16_t retail_vertices =
      ReadBeU16(retail_edge_mesh.mesh.bytes, retail_cluster + 4);
  const std::size_t retail_unit =
      retail_cluster + 16 + static_cast<std::size_t>(retail_vertices) * 16;
  Require(retail_edge_mesh.mesh.bytes.at(retail_unit + 4) == 0x1a &&
              retail_edge_mesh.mesh.bytes.at(retail_unit + 5) == 0x5a &&
              retail_edge_mesh.mesh.bytes.at(retail_unit + 6) == 0x62,
          "native retail collision edge codes were regenerated");
  const RwCollisionBuildResult adopted_retail_mesh =
      LoadSerializedRwCollisionMesh(retail_edge_mesh.mesh.bytes);
  Require(adopted_retail_mesh.ok, adopted_retail_mesh.error);
  Require(
      adopted_retail_mesh.mesh.bytes == retail_edge_mesh.mesh.bytes &&
          adopted_retail_mesh.mesh.triangle_count ==
              retail_edge_mesh.mesh.triangle_count &&
          adopted_retail_mesh.mesh.cluster_count ==
              retail_edge_mesh.mesh.cluster_count,
      "serialized retail collision mesh adoption changed the resource");

  RwCollisionMeshBlob translated_retail = rw.mesh;
  const std::vector<std::uint8_t> untranslated_bytes =
      translated_retail.bytes;
  constexpr Vec3 requested_retail_translation{
      10.125f, -3.250f, 7.375f};
  Vec3 applied_retail_translation{};
  std::string retail_translation_error;
  Require(
      TranslateSerializedRwCollisionMesh(
          translated_retail, requested_retail_translation,
          &applied_retail_translation, &retail_translation_error),
      retail_translation_error);
  Require(
      NearlyEqual(applied_retail_translation.x,
                  requested_retail_translation.x) &&
          NearlyEqual(applied_retail_translation.y,
                      requested_retail_translation.y) &&
          NearlyEqual(applied_retail_translation.z,
                      requested_retail_translation.z) &&
          NearlyEqual(
              translated_retail.bounds_min.x,
              rw.mesh.bounds_min.x + applied_retail_translation.x) &&
          NearlyEqual(
              ReadBeF32(translated_retail.bytes, 16),
              ReadBeF32(untranslated_bytes, 16) +
                  applied_retail_translation.x),
      "retail collision translation did not update mesh bounds");
  const std::uint32_t translated_kd =
      ReadBeU32(untranslated_bytes, 48);
  const std::uint32_t translated_branches =
      ReadBeU32(untranslated_bytes, translated_kd);
  const std::uint32_t translated_branch_count =
      ReadBeU32(untranslated_bytes, translated_kd + 4);
  for (std::uint32_t branch = 0;
       branch < translated_branch_count; ++branch) {
    const std::size_t record =
        translated_branches + static_cast<std::size_t>(branch) * 32u;
    const std::uint32_t axis = ReadBeU32(untranslated_bytes, record + 4);
    const float axis_translation =
        axis == 0 ? applied_retail_translation.x
                  : (axis == 1 ? applied_retail_translation.y
                               : applied_retail_translation.z);
    Require(
        NearlyEqual(
            ReadBeF32(translated_retail.bytes, record + 24),
            ReadBeF32(untranslated_bytes, record + 24) +
                axis_translation) &&
            NearlyEqual(
                ReadBeF32(translated_retail.bytes, record + 28),
                ReadBeF32(untranslated_bytes, record + 28) +
                    axis_translation),
        "retail collision translation did not update KD extents");
  }
  for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
    const std::uint32_t cluster_offset =
        ReadBeU32(untranslated_bytes,
                  cluster_table_offset + cluster * 4);
    const std::uint16_t vertex_blocks =
        ReadBeU16(untranslated_bytes, cluster_offset + 4);
    const std::uint8_t vertex_count =
        untranslated_bytes.at(cluster_offset + 10);
    Require(untranslated_bytes.at(cluster_offset + 12) == 0,
            "rebuilt collision test unexpectedly uses compression");
    for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
      const std::size_t vertex_offset =
          cluster_offset + 16u +
          static_cast<std::size_t>(vertex) * 16u;
      Require(
          NearlyEqual(
              ReadBeF32(translated_retail.bytes, vertex_offset),
              ReadBeF32(untranslated_bytes, vertex_offset) +
                  applied_retail_translation.x) &&
              NearlyEqual(
                  ReadBeF32(translated_retail.bytes, vertex_offset + 4),
                  ReadBeF32(untranslated_bytes, vertex_offset + 4) +
                      applied_retail_translation.y) &&
              NearlyEqual(
                  ReadBeF32(translated_retail.bytes, vertex_offset + 8),
                  ReadBeF32(untranslated_bytes, vertex_offset + 8) +
                      applied_retail_translation.z),
          "retail collision translation did not update vertices");
    }
    const std::size_t unit_offset =
        cluster_offset +
        (static_cast<std::size_t>(vertex_blocks) + 1u) * 16u;
    const std::uint16_t unit_bytes =
        ReadBeU16(untranslated_bytes, cluster_offset + 2);
    Require(
        std::equal(
            untranslated_bytes.begin() + unit_offset,
            untranslated_bytes.begin() + unit_offset + unit_bytes,
            translated_retail.bytes.begin() + unit_offset),
        "retail collision translation changed native unit metadata");
  }

  for (std::uint8_t compression : {std::uint8_t{1},
                                   std::uint8_t{2}}) {
    RwCollisionMeshBlob compressed = retail_edge_mesh.mesh;
    const std::uint32_t compressed_table =
        ReadBeU32(compressed.bytes, 52);
    const std::uint32_t compressed_cluster =
        ReadBeU32(compressed.bytes, compressed_table);
    const std::uint8_t compressed_vertices =
        compressed.bytes.at(compressed_cluster + 10);
    compressed.bytes.at(compressed_cluster + 12) = compression;
    if (compression == 1) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        WriteBeU32(compressed.bytes,
                   compressed_cluster + 16u + axis * 4u,
                   static_cast<std::uint32_t>(100 + axis));
      }
    } else {
      for (std::uint32_t vertex = 0;
           vertex < compressed_vertices; ++vertex) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
          WriteBeU32(
              compressed.bytes,
              compressed_cluster + 16u +
                  static_cast<std::size_t>(vertex) * 12u + axis * 4u,
              static_cast<std::uint32_t>(
                  100 + vertex * 10u + axis));
        }
      }
    }
    Vec3 compressed_translation{};
    Require(
        TranslateSerializedRwCollisionMesh(
            compressed, {2.0f, -1.0f, 3.0f},
            &compressed_translation, &retail_translation_error),
        retail_translation_error);
    const std::array<std::int32_t, 3> quantized_translation{
        2000, -1000, 3000};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const std::size_t offset =
          compressed_cluster + 16u + axis * 4u;
      const std::int32_t expected =
          static_cast<std::int32_t>(100 + axis) +
          quantized_translation[axis];
      Require(
          std::bit_cast<std::int32_t>(
              ReadBeU32(compressed.bytes, offset)) == expected,
          compression == 1
              ? "16-bit compressed retail base was not translated"
              : "32-bit compressed retail vertex was not translated");
    }
  }

  std::vector<std::uint8_t> branchless_retail =
      adopted_retail_mesh.mesh.bytes;
  const std::uint32_t branchless_kd =
      ReadBeU32(branchless_retail, 48);
  Require(ReadBeU32(branchless_retail, branchless_kd + 4) == 0,
          "single-triangle retail test mesh unexpectedly has KD branches");
  constexpr std::uint32_t kUnusedRetailBranchPointer = 0xfe720db0u;
  branchless_retail.at(branchless_kd) =
      static_cast<std::uint8_t>(kUnusedRetailBranchPointer >> 24u);
  branchless_retail.at(branchless_kd + 1) =
      static_cast<std::uint8_t>(kUnusedRetailBranchPointer >> 16u);
  branchless_retail.at(branchless_kd + 2) =
      static_cast<std::uint8_t>(kUnusedRetailBranchPointer >> 8u);
  branchless_retail.at(branchless_kd + 3) =
      static_cast<std::uint8_t>(kUnusedRetailBranchPointer);
  Require(FixupRwCollisionMeshForGuest(branchless_retail, 0x51000000u),
          "retail branchless KD tree guest fixup failed");
  Require(ReadBeU32(branchless_retail, branchless_kd) ==
              kUnusedRetailBranchPointer,
          "unused retail branchless pointer was modified");
  Require(ReadBeU32(branchless_retail, 48) ==
              0x51000000u + branchless_kd,
          "retail branchless KD header pointer was not fixed up");

  std::uint32_t kd_leaf_triangles = 0;
  for (std::uint32_t branch = 0; branch < kd_branch_count; ++branch) {
    const std::size_t record =
        96 + 48 + static_cast<std::size_t>(branch) * 32;
    for (std::size_t child_offset : {8u, 16u}) {
      const std::uint32_t content =
          ReadBeU32(rw.mesh.bytes, record + child_offset);
      const std::uint32_t index =
          ReadBeU32(rw.mesh.bytes, record + child_offset + 4);
      if (content == std::numeric_limits<std::uint32_t>::max()) {
        Require(index < kd_branch_count,
                "KD branch child index is out of range");
      } else {
        const std::uint32_t cluster = index >> 16u;
        const std::uint32_t unit_offset = index & 0xffffu;
        Require(content > 0 &&
                    cluster < cluster_count &&
                    unit_offset % 9 == 0 &&
                    unit_offset / 9 + content <= cluster_units[cluster],
                "KD leaf packed cluster/unit range is invalid");
        kd_leaf_triangles += content;
      }
    }
  }
  Require(kd_leaf_triangles == rw.mesh.triangle_count,
          "KD leaves do not cover every triangle exactly once");

  // The on-disk blob is relocatable. Skate's asset loader fixes up the
  // top-level pointers, while cluster table elements stay mesh-relative.
  const std::uint32_t guest_base = 0x50000000u;
  Require(ReadBeU32(rw.mesh.bytes, 48) < rw.mesh.bytes.size() &&
              ReadBeU32(rw.mesh.bytes, 52) < rw.mesh.bytes.size() &&
              ReadBeU32(rw.mesh.bytes, cluster_table_offset) <
                  rw.mesh.bytes.size(),
          "serialized RenderWare offsets are not relocatable");

  std::vector<std::uint8_t> fixed = rw.mesh.bytes;
  Require(FixupRwCollisionMeshForGuest(fixed, guest_base),
          "RenderWare guest fixup failed");
  Require(ReadBeU32(fixed, 48) == guest_base + 96,
          "KD-tree guest pointer was not fixed up");
  Require(ReadBeU32(fixed, 52) ==
              guest_base + cluster_table_offset,
          "cluster-table guest pointer was not fixed up");
  Require(ReadBeU32(fixed, 96) ==
              guest_base + 96 + 48,
          "KD branch-record guest pointer was not fixed up");
  Require(ReadBeU32(fixed, cluster_table_offset) ==
              first_cluster_offset,
          "cluster table element must remain mesh-relative");
  Require(!FixupRwCollisionMeshForGuest(fixed, guest_base),
          "repeated RenderWare guest fixup was not rejected");

  std::cout << "WORLD_TEST_PASS"
            << " vertices=" << definition.render_mesh.vertices.size()
            << " triangles="
            << definition.collision_triangles.size()
            << " rw_bytes=" << rw.mesh.bytes.size()
            << " rw_vertices=" << rw.mesh.vertex_count
            << " grind_rails=" << grind.blob.rail_count
            << " grind_segments=" << grind.blob.segment_count
            << " water_peak=" << water_stats.maximum_displacement
            << " basin_peak="
            << demo_water_stats.maximum_displacement
            << " basin_energy=" << demo_water_stats.kinetic_energy
            << " surfaces=33"
            << '\n';
  return EXIT_SUCCESS;
}
