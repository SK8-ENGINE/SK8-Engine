#include "skate3_multiplayer_player_collision_model.h"

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
      .discontinuity = false,
      .playing = true,
  };
}

LocalSample Local(float x, float y, float z, std::uint64_t observed_at_us,
                  float step_seconds = 1.0f / 60.0f) {
  return {
      .position = {x, y, z},
      .observed_at_us = observed_at_us,
      .step_seconds = step_seconds,
  };
}

void TestDimensionsAndCollisionLayers() {
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
  Expect(LayersCollide(kLocalPlayerLayer, kLocalPlayerMask, kRemotePlayerLayer,
                       kRemotePlayerMask),
         "local and remote player masks do not collide");
  Expect(!LayersCollide(kLocalPlayerLayer, 0, kRemotePlayerLayer,
                        kRemotePlayerMask),
         "a disabled local mask still collided");
  Expect((kRemotePlayerMask & kRemotePlayerLayer) == 0,
         "remote proxies collide with one another");
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
  Expect(proxy != nullptr && proxy->velocity[0] > 4.9f &&
             proxy->velocity[0] < 5.1f,
         "remote proxy velocity did not use presentation cadence");

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

void TestTeleportSnapsAndStartsGrace() {
  ProxySet proxies;
  const std::vector first{Remote(2, 0.0f, 0.0f, 0.0f, 1)};
  proxies.Update(Context(1), first);

  RemoteProxySample teleported = Remote(2, 10.0f, 0.0f, 0.0f, 20'001);
  teleported.discontinuity = true;
  const std::vector jump{teleported};
  proxies.Update(Context(20'001), jump);
  const Proxy *proxy = proxies.Find(2);
  Expect(proxy != nullptr && proxy->position[0] == 10.0f,
         "teleport did not snap the proxy to the rendered player");
  Expect(proxy != nullptr && proxy->velocity[0] == 0.0f &&
             proxy->grace_until_us == 20'001 + kOverlapGraceUs,
         "teleport swept velocity or omitted overlap grace");
  Expect(proxies.counters().teleports == 1,
         "teleport telemetry did not increment");

  teleported.position[0] = 10.1f;
  teleported.observed_at_us = 24'001;
  const std::vector continuing_jump{teleported};
  proxies.Update(Context(24'001), continuing_jump);
  Expect(proxies.counters().teleports == 1 &&
             proxies.Find(2)->grace_until_us == 24'001 + kOverlapGraceUs,
         "one discontinuity segment spammed telemetry or lost grace");
}

void TestSpawnOverlapGraceAndDepenetration() {
  ProxySet proxies;
  const std::vector remote{Remote(2, 0.0f, 0.0f, 0.0f, 1)};
  proxies.Update(Context(1), remote);
  const ResolveResult spawn = proxies.Resolve(Local(0.0f, 0.0f, 0.0f, 1));
  Expect(spawn.contacts == 1 && spawn.grace_contact,
         "spawn overlap did not use grace contact");
  Expect(spawn.maximum_contact_correction <=
             kGraceCorrectionSpeed / 60.0f + 1.0e-6f,
         "spawn overlap depenetrated too violently");
  Expect(spawn.maximum_equivalent_impulse == 0.0f,
         "spawn overlap generated an impulse during grace");

  const std::vector settled_remote{Remote(2, 0.0f, 0.0f, 0.0f, 500'001)};
  proxies.Update(Context(500'001), settled_remote);
  const ResolveResult settled =
      proxies.Resolve(Local(0.0f, 0.0f, 0.0f, 500'001));
  Expect(settled.contacts == 1 && !settled.grace_contact,
         "overlap remained permanently pinned in spawn grace");
  Expect(settled.maximum_contact_correction <=
             kMaximumCorrectionSpeed / 60.0f + 1.0e-6f,
         "normal overlap correction exceeded its fixed-step bound");
}

void TestBoundedClosingResponse() {
  ProxySet proxies;
  const std::vector remote_at_start{Remote(2, 0.0f, 0.0f, 0.0f, 1)};
  proxies.Update(Context(1), remote_at_start);
  (void)proxies.Resolve(Local(-2.0f, 0.0f, 0.0f, 1));

  const std::vector remote_next{Remote(2, 0.0f, 0.0f, 0.0f, 500'001)};
  proxies.Update(Context(500'001), remote_next);
  (void)proxies.Resolve(Local(-2.0f, 0.0f, 0.0f, 500'001));

  const std::vector remote_contact{Remote(2, 0.0f, 0.0f, 0.0f, 516'668)};
  proxies.Update(Context(516'668), remote_contact);
  const ResolveResult impact =
      proxies.Resolve(Local(-0.5f, 0.0f, 0.0f, 516'668));
  Expect(impact.contacts == 1,
         "representative closing skaters did not contact");
  Expect(impact.maximum_contact_correction <=
             kMaximumCorrectionSpeed / 60.0f + 1.0e-5f,
         "closing response exceeded its per-step correction limit");
  Expect(impact.maximum_equivalent_impulse > 0.0f &&
             impact.maximum_equivalent_impulse <= kMaximumEquivalentImpulse,
         "closing response impulse was absent or unbounded");
  Expect(std::abs(impact.correction[1]) < 1.0e-6f,
         "player collision launched the local skater vertically");
}

void TestVerticalSeparationAndPacketStall() {
  ProxySet proxies;
  std::vector remote{Remote(2, 0.0f, 3.0f, 0.0f, 1)};
  proxies.Update(Context(1), remote);
  const ResolveResult vertical = proxies.Resolve(Local(0.0f, 0.0f, 0.0f, 1));
  Expect(vertical.contacts == 0,
         "vertically separated capsules blocked one another");

  remote[0] = Remote(2, 1.0f, 0.0f, 0.0f, 100'001);
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
  TestDimensionsAndCollisionLayers();
  TestFacingTwoPlayerSpawnPlacement();
  TestCreateUpdateRemoveAndSelfFiltering();
  TestSessionReplacementAndMapTransitions();
  TestStaleAndNonPlayingCleanup();
  TestTeleportSnapsAndStartsGrace();
  TestSpawnOverlapGraceAndDepenetration();
  TestBoundedClosingResponse();
  TestVerticalSeparationAndPacketStall();
  TestDisabledStateClearsImmediately();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer player collision test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer player collision tests passed\n";
  return 0;
}
