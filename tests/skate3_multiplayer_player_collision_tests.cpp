#include "skate3_multiplayer_player_collision_model.h"

#include "skate/world/rw_collision_mesh.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace skate3::multiplayer::player_collision;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

void ExpectNear(float actual, float expected, float tolerance,
                std::string_view message) {
  Expect(std::abs(actual - expected) <= tolerance, message);
}

UpdateContext Context(std::uint64_t now_us = 1,
                      std::uint32_t map_hash = 0x1234u,
                      std::uint32_t local_session = 100u) {
  return {
      .enabled = true,
      .local_role = 1,
      .local_session = local_session,
      .map_hash = map_hash,
      .now_us = now_us,
      .disabled_reason = DisabledReason::kNone,
  };
}

RemoteProxySample Remote(std::uint32_t role, float x, float y, float z,
                         std::uint64_t observed_at_us = 1,
                         std::uint32_t session = 200u,
                         std::uint32_t map_hash = 0x1234u) {
  return {
      .role = role,
      .session = session,
      .map_hash = map_hash,
      .position = {x, y, z},
      .observed_at_us = observed_at_us,
      .spatial_valid = true,
      .playing = true,
  };
}

void TestNativeCapsuleGeometry() {
  ExpectNear(kPlayerProxyRadius, 0.30f, 1.0e-6f,
             "player capsule radius drifted from the project scale");
  ExpectNear(kPlayerProxyLowerCenter, -0.20f, 1.0e-6f,
             "player capsule lower centre drifted from the project scale");
  ExpectNear(kPlayerProxyUpperCenter, 1.25f, 1.0e-6f,
             "player capsule upper centre drifted from the project scale");
  Expect(kPlayerProxyUpperCenter > kPlayerProxyLowerCenter,
         "player capsule segment is inverted");
  ExpectNear(kPlayerProxyUpperCenter - kPlayerProxyLowerCenter +
                 kPlayerProxyRadius * 2.0f,
             2.05f, 1.0e-6f,
             "player capsule no longer matches standing skater height");
  ExpectNear(kPlayerProxyRadius * 2.0f, 0.60f, 1.0e-6f,
             "player capsule is wider than the rendered body");

  const std::vector<CapsuleTriangle> triangles =
      BuildPlayerCapsuleTriangles();
  Expect(triangles.size() ==
             kPlayerProxyRadialSegments *
                 kPlayerProxyHemisphereSegments * 4,
         "native capsule has the wrong triangle count");
  float minimum_y = INFINITY;
  float maximum_y = -INFINITY;
  float maximum_radius = 0.0f;
  for (const CapsuleTriangle &triangle : triangles) {
    const float normal_length =
        std::hypot(triangle.normal[0], triangle.normal[1],
                   triangle.normal[2]);
    ExpectNear(normal_length, 1.0f, 1.0e-5f,
               "native capsule contains a degenerate triangle");
    const std::array<float, 3> center{
        (triangle.a[0] + triangle.b[0] + triangle.c[0]) / 3.0f,
        (triangle.a[1] + triangle.b[1] + triangle.c[1]) / 3.0f,
        (triangle.a[2] + triangle.b[2] + triangle.c[2]) / 3.0f,
    };
    const float centerline_y =
        std::clamp(center[1], kPlayerProxyLowerCenter,
                   kPlayerProxyUpperCenter);
    const float outward_dot =
        triangle.normal[0] * center[0] +
        triangle.normal[1] * (center[1] - centerline_y) +
        triangle.normal[2] * center[2];
    Expect(outward_dot > 0.0f,
           "native capsule triangle faces inward");
    for (const auto &vertex :
         {triangle.a, triangle.b, triangle.c}) {
      Expect(std::isfinite(vertex[0]) && std::isfinite(vertex[1]) &&
                 std::isfinite(vertex[2]),
             "native capsule contains a non-finite vertex");
      minimum_y = std::min(minimum_y, vertex[1]);
      maximum_y = std::max(maximum_y, vertex[1]);
      maximum_radius =
          std::max(maximum_radius, std::hypot(vertex[0], vertex[2]));
    }
  }
  ExpectNear(minimum_y, kPlayerProxyLowerCenter - kPlayerProxyRadius,
             1.0e-6f, "native capsule bottom is misplaced");
  ExpectNear(maximum_y, kPlayerProxyUpperCenter + kPlayerProxyRadius,
             1.0e-6f, "native capsule top is misplaced");
  ExpectNear(maximum_radius, kPlayerProxyRadius, 1.0e-6f,
             "native capsule width is incorrect");

  skate::world::MapDefinition capsule;
  capsule.name = "multiplayer_player_capsule_test";
  for (const CapsuleTriangle &triangle : triangles) {
    capsule.collision_triangles.push_back({
        .a = {triangle.a[0], triangle.a[1], triangle.a[2]},
        .b = {triangle.b[0], triangle.b[1], triangle.b[2]},
        .c = {triangle.c[0], triangle.c[1], triangle.c[2]},
        .normal = {
            triangle.normal[0], triangle.normal[1], triangle.normal[2]},
        .surface = 1,
        .material = 1,
    });
  }
  skate::world::RwCollisionBuildOptions options;
  options.default_surface_id =
      skate::world::EncodeRwSurfaceId(3, 1, 0);
  options.material_surface_ids.emplace(1, options.default_surface_id);
  const skate::world::RwCollisionBuildResult build =
      skate::world::BuildRwCollisionMesh(capsule, options);
  Expect(build.ok && !build.mesh.bytes.empty(),
         "player capsule did not compile as native world collision");
  Expect(build.mesh.triangle_count == triangles.size(),
         "native collision compiler dropped capsule triangles");
}

void TestFacingTwoPlayerSpawnPlacement() {
  const std::array<float, 16> source = {
      1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 7.0f, 2.0f, 9.0f, 1.0f,
  };
  const FacingTestSpawn first = BuildFacingTestSpawn(1, source);
  const FacingTestSpawn second = BuildFacingTestSpawn(2, source);
  Expect(first.valid && second.valid,
         "valid two-player test spawn placement was rejected");
  ExpectNear(first.transform[12], 7.0f, 1.0e-6f,
             "client 1 drifted sideways during test placement");
  ExpectNear(second.transform[12], 7.0f, 1.0e-6f,
             "client 2 drifted sideways during test placement");
  ExpectNear(first.transform[14], 5.0f, 1.0e-6f,
             "client 1 was not moved behind the common spawn");
  ExpectNear(second.transform[14], 13.0f, 1.0e-6f,
             "client 2 was not moved ahead of the common spawn");
  ExpectNear(second.transform[14] - first.transform[14],
             kFacingTestSpawnSpacing, 1.0e-6f,
             "test clients did not receive the intended separation");
  ExpectNear(first.transform[8], -second.transform[8], 1.0e-6f,
             "test clients do not face one another on the X axis");
  ExpectNear(first.transform[10], -second.transform[10], 1.0e-6f,
             "test clients do not face one another on the Z axis");
  ExpectNear(first.transform[13], source[13], 1.0e-6f,
             "test placement changed client 1 height");
  ExpectNear(second.transform[13], source[13], 1.0e-6f,
             "test placement changed client 2 height");
  Expect(!BuildFacingTestSpawn(3, source).valid,
         "test placement accepted a non-two-player role");

  std::array<float, 16> invalid = source;
  invalid[12] = std::nanf("");
  Expect(!BuildFacingTestSpawn(1, invalid).valid,
         "test placement accepted a non-finite transform");
}

void TestCreateUpdateRemoveAndSelfFiltering() {
  ProxySet proxies;
  const std::vector samples{
      Remote(1, 0.0f, 0.0f, 0.0f),
      Remote(2, 2.0f, 0.0f, 0.0f),
  };
  proxies.Update(Context(), samples);
  Expect(proxies.size() == 1 && proxies.Find(1) == nullptr,
         "local player was retained in its own remote proxy set");
  Expect(proxies.Find(2) != nullptr,
         "valid remote player did not create a proxy");

  const std::vector moved{Remote(2, 2.5f, 0.0f, 0.0f, 100'001)};
  proxies.Update(Context(100'001), moved);
  const Proxy *proxy = proxies.Find(2);
  Expect(proxy != nullptr && proxy->position[0] == 2.5f,
         "remote presentation update did not move its proxy");

  proxies.Update(Context(100'002), {});
  Expect(proxies.size() == 0,
         "missing remote presentation did not remove its proxy immediately");
  Expect(proxies.counters().created == 1 && proxies.counters().removed == 1,
         "proxy create/remove telemetry is inconsistent");
}

void TestSessionReplacementAndMapTransitions() {
  ProxySet proxies;
  const std::vector first{Remote(2, 0.0f, 0.0f, 0.0f)};
  proxies.Update(Context(), first);
  const std::vector replacement{Remote(2, 1.0f, 0.0f, 0.0f, 2, 201u)};
  proxies.Update(Context(2), replacement);
  Expect(proxies.Find(2) != nullptr && proxies.Find(2)->session == 201u,
         "reconnected occupant inherited the old proxy generation");

  const std::vector next_map{Remote(3, 0.0f, 0.0f, 0.0f, 3, 300u, 0x5678u)};
  proxies.Update(Context(3, 0x5678u), next_map);
  Expect(proxies.Find(2) == nullptr && proxies.Find(3) != nullptr,
         "map transition retained a proxy from the previous world");

  proxies.Update(Context(4, 0x5678u, 101u), next_map);
  Expect(proxies.size() == 1 && proxies.counters().removed >= 3,
         "local session transition did not rebuild remote proxies");
}

void TestStaleAndNonPlayingCleanup() {
  ProxySet proxies;
  const std::vector live{Remote(2, 0.0f, 0.0f, 0.0f, 100)};
  proxies.Update(Context(100), live);

  const std::vector stale{Remote(2, 0.0f, 0.0f, 0.0f, 100)};
  proxies.Update(Context(100 + kRemoteSampleStaleUs + 1), stale);
  Expect(proxies.size() == 0 && proxies.counters().stale_cleanups == 1,
         "stale remote state left an invisible proxy");

  RemoteProxySample spectator = Remote(3, 0.0f, 0.0f, 0.0f, 2'000'000);
  spectator.playing = false;
  const std::vector spectators{spectator};
  proxies.Update(Context(2'000'000), spectators);
  Expect(proxies.size() == 0, "non-playing peer created a collision proxy");
}

void TestProxyPositionSnapsDirectly() {
  ProxySet proxies;
  const std::vector first{Remote(2, 0.0f, 0.0f, 0.0f, 1)};
  proxies.Update(Context(1), first);

  const std::vector jump{Remote(2, 10.0f, 0.0f, 0.0f, 20'001)};
  proxies.Update(Context(20'001), jump);
  const Proxy *proxy = proxies.Find(2);
  Expect(proxy != nullptr && proxy->position[0] == 10.0f,
         "proxy did not snap directly to the rendered player");
}

void TestPacketStall() {
  ProxySet proxies;
  std::vector remote{Remote(2, 1.0f, 0.0f, 0.0f, 100'001)};
  proxies.Update(Context(100'001), remote);
  remote[0].observed_at_us = 600'001;
  proxies.Update(Context(600'001), remote);
  Expect(proxies.size() == 1,
         "held interpolated pose was removed during packet stall");
  proxies.Update(Context(600'002), {});
  Expect(proxies.size() == 0,
         "peer disappearance after packet stall left a proxy");
}

void TestDisabledStateClearsImmediately() {
  ProxySet proxies;
  const std::vector remote{Remote(2, 0.0f, 0.0f, 0.0f)};
  proxies.Update(Context(), remote);
  UpdateContext offline = Context(2);
  offline.enabled = false;
  offline.disabled_reason = DisabledReason::kMultiplayerInactive;
  proxies.Update(offline, {});
  Expect(proxies.size() == 0 &&
             proxies.disabled_reason() == DisabledReason::kMultiplayerInactive,
         "offline transition retained collision state");
}

} // namespace

int main() {
  TestNativeCapsuleGeometry();
  TestFacingTwoPlayerSpawnPlacement();
  TestCreateUpdateRemoveAndSelfFiltering();
  TestSessionReplacementAndMapTransitions();
  TestStaleAndNonPlayingCleanup();
  TestProxyPositionSnapsDirectly();
  TestPacketStall();
  TestDisabledStateClearsImmediately();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer player collision test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer player collision tests passed\n";
  return 0;
}
