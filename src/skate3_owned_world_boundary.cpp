#include "skate3_owned_world_boundary.h"

#include "generated/skate3_init.h"
#include "skate3_mechanics_sandbox.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_native_collision.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <ostream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_strip_retail_world_actors, true, "Skate 3",
    "While the owned custom-world sandbox is requested, prevent retail scene "
    "DMOs and Living World pedestrians and vehicles from spawning behind it. "
    "Generic dynamic presentation remains active because it also owns "
    "player/board/on-foot transitions.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_owned_npc_paths, true, "Skate 3",
    "Replace retail AI skater navigation with Blender-authored OW_NPC_PATHS "
    "while retaining Skate 3's native steering, board physics, animation, "
    "collision response, tricks, and bails.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace skate3::owned_world_boundary {
namespace {

std::atomic<uint64_t> g_scene_dmo_queries_suppressed{0};
std::atomic<uint64_t> g_static_dmo_spawns_suppressed{0};
std::atomic<uint64_t> g_pedestrian_spawns_suppressed{0};
std::atomic<uint64_t> g_vehicle_spawns_suppressed{0};
std::atomic<uint64_t> g_dynamic_object_spawns_suppressed{0};
std::atomic<uint64_t> g_dynamic_presentation_calls_allowed{0};
std::atomic<uint64_t> g_ai_spawn_requests{0};
std::atomic<uint64_t> g_ai_spawns_owned{0};
std::atomic<uint64_t> g_ai_spawns_suppressed{0};
std::atomic<uint64_t> g_ai_path_overrides{0};
std::atomic<uint64_t> g_ai_actors_assigned{0};
std::atomic<bool> g_announced{false};
std::atomic<std::uint32_t> g_ai_spawn_slot{0};
std::atomic<std::uint32_t> g_ai_actor_slot{0};
std::mutex g_ai_mutex;

struct NpcActorState {
  std::size_t route_index = 0;
  std::uint32_t route_ordinal = 0;
};

std::unordered_map<std::uint32_t, NpcActorState> g_ai_actors;

void AnnounceOnce() {
  bool expected = false;
  if (g_announced.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
    REXLOG_INFO(
        "owned-world-boundary: retail scene DMOs, pedestrians, vehicles, "
        "and AI spawns are disabled; generic player/board/on-foot dynamic "
        "presentation remains active");
  }
}

void RecordSuppression(std::atomic<uint64_t>& counter) {
  counter.fetch_add(1, std::memory_order_relaxed);
  AnnounceOnce();
}

bool CustomNpcPathsEnabled() {
  return mechanics_sandbox::Requested() &&
         REXCVAR_GET(skate3_mechanics_sandbox_owned_npc_paths);
}

std::uint32_t RequestedNpcCount() {
  std::uint32_t count = 0;
  for (const auto& route :
       mechanics_sandbox::map::ActiveDefinition().npc_routes) {
    count += route.skater_count;
  }
  return count;
}

std::pair<std::size_t, std::uint32_t> RouteSlot(
    std::uint32_t flat_slot) {
  const auto& routes =
      mechanics_sandbox::map::ActiveDefinition().npc_routes;
  for (std::size_t route_index = 0; route_index < routes.size();
       ++route_index) {
    if (flat_slot < routes[route_index].skater_count) {
      return {route_index, flat_slot};
    }
    flat_slot -= routes[route_index].skater_count;
  }
  return {0, 0};
}

bool MapOrigin(skate::world::Vec3& result) {
  float origin[3] = {};
  if (!native_collision::MapWorldOrigin(origin) &&
      !mechanics_sandbox::SandboxMapOrigin(origin)) {
    return false;
  }
  result = {origin[0], origin[1], origin[2]};
  return std::isfinite(result.x) && std::isfinite(result.y) &&
         std::isfinite(result.z);
}

float RouteLength(const skate::world::NpcRoute& route) {
  float length = 0.0f;
  for (std::size_t index = 1; index < route.points.size(); ++index) {
    length += skate::world::Length(
        route.points[index] - route.points[index - 1]);
  }
  if (route.closed) {
    length += skate::world::Length(
        route.points.front() - route.points.back());
  }
  return length;
}

skate::world::Vec3 SampleRoute(
    const skate::world::NpcRoute& route, float distance,
    skate::world::Vec3* out_tangent = nullptr) {
  const float length = RouteLength(route);
  if (length <= 1.0e-4f) {
    if (out_tangent != nullptr) {
      *out_tangent = {1.0f, 0.0f, 0.0f};
    }
    return route.points.front();
  }

  if (route.closed) {
    distance = std::fmod(std::max(0.0f, distance), length);
  } else {
    const float period = length * 2.0f;
    distance = std::fmod(std::max(0.0f, distance), period);
    if (distance > length) {
      distance = period - distance;
    }
  }

  const std::size_t segment_count =
      route.points.size() - 1 + (route.closed ? 1 : 0);
  for (std::size_t segment = 0; segment < segment_count; ++segment) {
    const skate::world::Vec3 start = route.points[segment];
    const skate::world::Vec3 end =
        route.points[(segment + 1) % route.points.size()];
    const float segment_length = skate::world::Length(end - start);
    if (distance <= segment_length || segment + 1 == segment_count) {
      const float alpha =
          segment_length > 1.0e-4f ? distance / segment_length : 0.0f;
      if (out_tangent != nullptr) {
        *out_tangent =
            segment_length > 1.0e-4f
                ? (end - start) * (1.0f / segment_length)
                : skate::world::Vec3{1.0f, 0.0f, 0.0f};
      }
      return start + (end - start) * std::clamp(alpha, 0.0f, 1.0f);
    }
    distance -= segment_length;
  }
  return route.points.back();
}

float ClosestRouteDistance(const skate::world::NpcRoute& route,
                           skate::world::Vec3 position) {
  float best_distance_squared = std::numeric_limits<float>::infinity();
  float best_route_distance = 0.0f;
  float accumulated = 0.0f;
  const std::size_t segment_count =
      route.points.size() - 1 + (route.closed ? 1 : 0);
  for (std::size_t segment = 0; segment < segment_count; ++segment) {
    const skate::world::Vec3 start = route.points[segment];
    const skate::world::Vec3 end =
        route.points[(segment + 1) % route.points.size()];
    const skate::world::Vec3 delta = end - start;
    const float length_squared = skate::world::LengthSquared(delta);
    if (length_squared <= 1.0e-8f) {
      continue;
    }
    const float alpha = std::clamp(
        skate::world::Dot(position - start, delta) / length_squared,
        0.0f, 1.0f);
    const skate::world::Vec3 closest = start + delta * alpha;
    const float distance_squared =
        skate::world::LengthSquared(position - closest);
    const float segment_length = std::sqrt(length_squared);
    if (distance_squared < best_distance_squared) {
      best_distance_squared = distance_squared;
      best_route_distance = accumulated + segment_length * alpha;
    }
    accumulated += segment_length;
  }
  return best_route_distance;
}

float LoadGuestFloat(std::uint8_t* base, std::uint32_t address) {
  return std::bit_cast<float>(REX_LOAD_U32(address));
}

void StoreGuestFloat(std::uint8_t* base, std::uint32_t address,
                     float value) {
  REX_STORE_U32(address, std::bit_cast<std::uint32_t>(value));
}

bool ActorPosition(PPCContext& ctx, std::uint8_t* base,
                   std::uint32_t actor,
                   skate::world::Vec3& position) {
  const std::uint32_t old_stack = ctx.r1.u32;
  const std::uint32_t new_stack = old_stack - 0x80u;
  const std::uint32_t matrix = new_stack + 0x20u;
  REX_STORE_U32(new_stack, old_stack);
  PPCContext probe = ctx;
  probe.r1.u64 = new_stack;
  probe.r3.u64 = matrix;
  probe.r4.u64 = actor;
  sub_82592990(probe, base);
  position = {
      LoadGuestFloat(base, matrix + 48),
      LoadGuestFloat(base, matrix + 52),
      LoadGuestFloat(base, matrix + 56),
  };
  return std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z);
}

template <typename Callback>
void WithGuestCopy(PPCContext& ctx, std::uint8_t* base,
                   std::uint32_t source,
                   std::uint32_t byte_count, std::uint32_t& argument,
                   Callback&& callback) {
  const std::uint32_t old_stack = ctx.r1.u32;
  const std::uint32_t reserved =
      (byte_count + 0x4Fu) & ~std::uint32_t{0xFu};
  const std::uint32_t new_stack = old_stack - reserved;
  const std::uint32_t copy = new_stack + 0x20u;
  REX_STORE_U32(new_stack, old_stack);
  for (std::uint32_t offset = 0; offset < byte_count; offset += 4) {
    REX_STORE_U32(copy + offset, REX_LOAD_U32(source + offset));
  }
  ctx.r1.u64 = new_stack;
  argument = copy;
  callback(copy);
  ctx.r1.u64 = old_stack;
}

}  // namespace

bool SuppressingRetailWorldActors() {
  // Requested(), rather than Active(), is intentional. Streamed scene actors
  // are created before the local skater is observed and the presentation
  // sandbox reaches Active.
  return mechanics_sandbox::Requested() &&
         REXCVAR_GET(skate3_mechanics_sandbox_strip_retail_world_actors);
}

void AppendTelemetry(std::ostream& out) {
  out << " owned_world_retail_actors_suppressed="
      << (SuppressingRetailWorldActors() ? 1 : 0)
      << " owned_world_scene_dmo_queries_suppressed="
      << g_scene_dmo_queries_suppressed.load(std::memory_order_relaxed)
      << " owned_world_static_dmo_spawns_suppressed="
      << g_static_dmo_spawns_suppressed.load(std::memory_order_relaxed)
      << " owned_world_pedestrian_spawns_suppressed="
      << g_pedestrian_spawns_suppressed.load(std::memory_order_relaxed)
      << " owned_world_vehicle_spawns_suppressed="
      << g_vehicle_spawns_suppressed.load(std::memory_order_relaxed)
      << " owned_world_dynamic_object_spawns_suppressed="
      << g_dynamic_object_spawns_suppressed.load(std::memory_order_relaxed)
      << " owned_world_dynamic_presentation_calls_allowed="
      << g_dynamic_presentation_calls_allowed.load(
             std::memory_order_relaxed)
      << " owned_world_npc_paths="
      << (CustomNpcPathsEnabled() ? 1 : 0)
      << " owned_world_npc_routes="
      << mechanics_sandbox::map::ActiveDefinition().npc_routes.size()
      << " owned_world_npc_requested=" << RequestedNpcCount()
      << " owned_world_ai_spawn_requests="
      << g_ai_spawn_requests.load(std::memory_order_relaxed)
      << " owned_world_ai_spawns_owned="
      << g_ai_spawns_owned.load(std::memory_order_relaxed)
      << " owned_world_ai_spawns_suppressed="
      << g_ai_spawns_suppressed.load(std::memory_order_relaxed)
      << " owned_world_ai_actors_assigned="
      << g_ai_actors_assigned.load(std::memory_order_relaxed)
      << " owned_world_ai_path_overrides="
      << g_ai_path_overrides.load(std::memory_order_relaxed);
}

}  // namespace skate3::owned_world_boundary

// SceneBinDirectory::IsSceneWithDMOsEnabled. Returning false here prevents
// streamed retail scenes from scheduling their DMO collections at all.
extern "C" REX_FUNC(sub_828235F0) {
  if (skate3::owned_world_boundary::SuppressingRetailWorldActors()) {
    skate3::owned_world_boundary::RecordSuppression(
        skate3::owned_world_boundary::g_scene_dmo_queries_suppressed);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828235F0(ctx, base);
}

// Dmo::SimManager::SpawnStatic. This second boundary catches DMO bodies
// requested by paths which do not consult the scene-directory capability.
extern "C" REX_FUNC(sub_825876D0) {
  if (skate3::owned_world_boundary::SuppressingRetailWorldActors()) {
    skate3::owned_world_boundary::RecordSuppression(
        skate3::owned_world_boundary::g_static_dmo_spawns_suppressed);
    return;
  }
  __imp__sub_825876D0(ctx, base);
}

// LivingWorld census spawn boundaries. Each original routine already has
// normal null-return paths, so callers are designed to tolerate no actor.
extern "C" REX_FUNC(sub_826B8038) {
  if (skate3::owned_world_boundary::SuppressingRetailWorldActors()) {
    skate3::owned_world_boundary::RecordSuppression(
        skate3::owned_world_boundary::g_pedestrian_spawns_suppressed);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_826B8038(ctx, base);
}

extern "C" REX_FUNC(sub_82C36300) {
  if (skate3::owned_world_boundary::SuppressingRetailWorldActors()) {
    skate3::owned_world_boundary::RecordSuppression(
        skate3::owned_world_boundary::g_vehicle_spawns_suppressed);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82C36300(ctx, base);
}

extern "C" REX_FUNC(sub_82C4D440) {
  if (skate3::owned_world_boundary::SuppressingRetailWorldActors()) {
    // This boundary is not retail-prop-specific. Runtime evidence showed it
    // is exercised tens of thousands of times even with scene DMO scheduling
    // disabled, and suppressing it makes the on-foot state roll back after
    // exactly 16 frames. Keep the precise scene/static/pedestrian/vehicle
    // boundaries above, but preserve this shared dynamic presentation path.
    skate3::owned_world_boundary::g_dynamic_presentation_calls_allowed
        .fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82C4D440(ctx, base);
}

// Sk8::SkaterManager::SpawnAi. The retail population manager may still ask
// for AI skaters, but the owned map controls how many exist and where they
// enter the world. The matrix copy keeps caller-owned guest memory immutable.
extern "C" REX_FUNC(sub_82598600) {
  using namespace skate3::owned_world_boundary;
  if (!CustomNpcPathsEnabled()) {
    __imp__sub_82598600(ctx, base);
    return;
  }

  g_ai_spawn_requests.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t desired = RequestedNpcCount();
  if (desired == 0) {
    g_ai_spawns_suppressed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const std::uint32_t slot =
      g_ai_spawn_slot.fetch_add(1, std::memory_order_acq_rel);
  if (slot >= desired) {
    g_ai_spawns_suppressed.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const std::uint32_t matrix = ctx.r6.u32;
  const auto [route_index, ordinal] = RouteSlot(slot);
  const auto& route =
      skate3::mechanics_sandbox::map::ActiveDefinition()
          .npc_routes[route_index];
  skate::world::Vec3 origin{};
  if (matrix >= 0x00010000u && matrix < 0x80000000u &&
      MapOrigin(origin)) {
    const skate::world::Vec3 local =
        SampleRoute(route, ordinal * route.spawn_spacing);
    WithGuestCopy(
        ctx, base, matrix, 64, ctx.r6.u32,
        [&](std::uint32_t copy) {
          StoreGuestFloat(base, copy + 48, origin.x + local.x);
          StoreGuestFloat(base, copy + 52, origin.y + local.y);
          StoreGuestFloat(base, copy + 56, origin.z + local.z);
          __imp__sub_82598600(ctx, base);
        });
  } else {
    __imp__sub_82598600(ctx, base);
  }
  g_ai_spawns_owned.fetch_add(1, std::memory_order_relaxed);
}

// Sk8::AI::PathController::GeneratePhysicsInput. The native AI stack still
// turns this future node into controller input; only the retail navigation
// target is replaced. This keeps contacts, acceleration, animation, tricks,
// and falls inside the original mechanics rather than moving an NPC transform.
extern "C" REX_FUNC(sub_8246DE38) {
  using namespace skate3::owned_world_boundary;
  if (!CustomNpcPathsEnabled() ||
      skate3::mechanics_sandbox::map::ActiveDefinition()
          .npc_routes.empty()) {
    __imp__sub_8246DE38(ctx, base);
    return;
  }

  const std::uint32_t actor = ctx.r4.u32;
  const std::uint32_t node = ctx.r5.u32;
  skate::world::Vec3 origin{};
  if (actor < 0x00010000u || actor >= 0x80000000u ||
      node < 0x00010000u || node >= 0x80000000u ||
      !MapOrigin(origin)) {
    __imp__sub_8246DE38(ctx, base);
    return;
  }

  NpcActorState state;
  {
    std::scoped_lock lock(g_ai_mutex);
    auto found = g_ai_actors.find(actor);
    if (found == g_ai_actors.end()) {
      const std::uint32_t desired = std::max(1u, RequestedNpcCount());
      const std::uint32_t flat_slot =
          g_ai_actor_slot.fetch_add(1, std::memory_order_acq_rel) % desired;
      const auto [route_index, ordinal] = RouteSlot(flat_slot);
      const auto& route =
          skate3::mechanics_sandbox::map::ActiveDefinition()
              .npc_routes[route_index];
      found = g_ai_actors.emplace(
          actor,
          NpcActorState{
              route_index,
              ordinal,
          }).first;
      g_ai_actors_assigned.fetch_add(1, std::memory_order_relaxed);
    }
    state = found->second;
  }

  const auto& route =
      skate3::mechanics_sandbox::map::ActiveDefinition()
          .npc_routes[state.route_index];
  skate::world::Vec3 actor_world{};
  if (!ActorPosition(ctx, base, actor, actor_world)) {
    __imp__sub_8246DE38(ctx, base);
    return;
  }
  const skate::world::Vec3 actor_local = actor_world - origin;
  const float route_distance = ClosestRouteDistance(route, actor_local);
  const float look_ahead =
      std::clamp(route.speed * 0.7f, 2.5f, 5.5f);
  skate::world::Vec3 tangent{};
  const skate::world::Vec3 target =
      SampleRoute(route, route_distance + look_ahead, &tangent);
  WithGuestCopy(
      ctx, base, node, 128, ctx.r5.u32,
      [&](std::uint32_t copy) {
        StoreGuestFloat(base, copy, origin.x + target.x);
        StoreGuestFloat(base, copy + 4, origin.y + target.y);
        StoreGuestFloat(base, copy + 8, origin.z + target.z);
        StoreGuestFloat(base, copy + 12, tangent.x);
        StoreGuestFloat(base, copy + 16, tangent.y);
        StoreGuestFloat(base, copy + 20, tangent.z);
        __imp__sub_8246DE38(ctx, base);
      });
  g_ai_path_overrides.fetch_add(1, std::memory_order_relaxed);
}
