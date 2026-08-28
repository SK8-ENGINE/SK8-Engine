#include "skate3_multiplayer_player_collision.h"

#include "generated/skate3_init.h"
#include "skate3_map_editor.h"
#include "skate3_mechanics_sandbox.h"
#include "skate3_trick_pipeline.h"

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_BOOL(
    skate3_multiplayer_player_collision_test_spawn, false,
    "Skate 3/Multiplayer",
    "Two-client visual-check helper: place the two local skaters eight metres "
    "apart facing one another once per multiplayer session and map. This is "
    "disabled during normal play.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::multiplayer::player_collision {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kMaximumPresentationAgeUs = 500'000;
constexpr std::uint64_t kMaximumSourceAgeUs = 500'000;

struct PublishedPresentation {
  bool enabled = false;
  bool world_valid = false;
  std::uint32_t local_role = 0;
  std::uint32_t local_session = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t sequence = 0;
  std::uint64_t published_at_us = 0;
  std::uint64_t source_advanced_at_us = 0;
  std::vector<RemoteProxySample> samples;
};

std::mutex g_presentation_mutex;
PublishedPresentation g_presentation;

std::mutex g_model_mutex;
ProxySet g_proxies;
std::uint64_t g_last_log_us = 0;
std::uint64_t g_last_logged_contacts = 0;
std::uint64_t g_last_logged_stale_cleanups = 0;

struct FacingTestSpawnState {
  std::uint32_t local_role = 0;
  std::uint32_t local_session = 0;
  std::uint32_t map_hash = 0;
  bool applied = false;
};

FacingTestSpawnState g_facing_test_spawn;

[[nodiscard]] std::uint64_t NowMicroseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          Clock::now().time_since_epoch())
          .count());
}

[[nodiscard]] std::uint32_t HashMapName(const char *map_name) {
  std::uint32_t hash = 2166136261u;
  const std::string_view name =
      map_name == nullptr ? std::string_view{} : std::string_view(map_name);
  for (const unsigned char value : name) {
    hash ^= value;
    hash *= 16777619u;
  }
  return hash;
}

[[nodiscard]] bool Finite3(const float value[3]) {
  return value != nullptr && std::isfinite(value[0]) &&
         std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] bool IsGuestHeapAddress(std::uint32_t address) {
  constexpr std::uint32_t kGuestHeapStart = 0x40000000u;
  constexpr std::uint32_t kGuestHeapEnd = 0x72000000u;
  return address >= kGuestHeapStart && address < kGuestHeapEnd;
}

[[nodiscard]] std::uint32_t LoadGuestU32(std::uint8_t *base,
                                         std::uint32_t address) {
  return address == 0 ? 0 : REX_LOAD_U32(address);
}

[[nodiscard]] float LoadGuestF32(std::uint8_t *base, std::uint32_t address) {
  return std::bit_cast<float>(LoadGuestU32(base, address));
}

[[nodiscard]] const char *DisabledReasonName(DisabledReason reason) {
  switch (reason) {
  case DisabledReason::kNone:
    return "none";
  case DisabledReason::kMultiplayerInactive:
    return "multiplayer-inactive";
  case DisabledReason::kInvalidLocalIdentity:
    return "invalid-local-identity";
  case DisabledReason::kInvalidWorld:
    return "invalid-world";
  case DisabledReason::kPresentationStale:
    return "presentation-stale";
  case DisabledReason::kMenu:
    return "menu";
  case DisabledReason::kMapEditor:
    return "map-editor";
  case DisabledReason::kLocalNotPlaying:
    return "local-not-playing";
  case DisabledReason::kGuestStateInvalid:
    return "guest-state-invalid";
  }
  return "unknown";
}

void Disable(DisabledReason reason, bool stale_cleanup = false) {
  std::scoped_lock lock(g_model_mutex);
  g_proxies.Disable(reason, stale_cleanup);
}

void MaybeLog(std::uint64_t now_us) {
  if (g_last_log_us != 0 && now_us - g_last_log_us < 5'000'000) {
    return;
  }
  const Counters &counters = g_proxies.counters();
  if (g_proxies.size() == 0 && counters.contacts == g_last_logged_contacts &&
      counters.stale_cleanups == g_last_logged_stale_cleanups) {
    return;
  }
  REXLOG_INFO("multiplayer-player-collision: proxies={} contacts={} "
              "stale_cleanup={} disabled={}",
              g_proxies.size(), counters.contacts, counters.stale_cleanups,
              DisabledReasonName(g_proxies.disabled_reason()));
  g_last_logged_contacts = counters.contacts;
  g_last_logged_stale_cleanups = counters.stale_cleanups;
  g_last_log_us = now_us;
}

[[nodiscard]] std::array<float, 16> LoadTransform(std::uint8_t *base,
                                                  std::uint32_t transform) {
  std::array<float, 16> matrix{};
  for (std::uint32_t component = 0; component < matrix.size(); ++component) {
    matrix[component] =
        LoadGuestF32(base, transform + component * sizeof(float));
  }
  return matrix;
}

[[nodiscard]] bool StoreBoardTransform(PPCContext &ctx, std::uint8_t *base,
                                       std::uint32_t skateboard,
                                       std::uint32_t transform,
                                       const std::array<float, 16> &matrix) {
  const std::uint32_t skateboard_body = LoadGuestU32(base, skateboard + 12);
  if (!IsGuestHeapAddress(skateboard_body) || ctx.r1.u32 < 512u) {
    return false;
  }
  const std::uint32_t matrix_address = ctx.r1.u32 - 512u;
  for (std::uint32_t component = 0; component < matrix.size(); ++component) {
    REX_STORE_U32(matrix_address + component * sizeof(float),
                  std::bit_cast<std::uint32_t>(matrix[component]));
  }
  PPCContext correction_ctx = ctx;
  correction_ctx.r3.u64 = skateboard_body;
  correction_ctx.r4.u64 = matrix_address;
  sub_82C0B2C8(correction_ctx, base);
  for (std::uint32_t component = 0; component < matrix.size(); ++component) {
    REX_STORE_U32(transform + component * sizeof(float),
                  std::bit_cast<std::uint32_t>(matrix[component]));
  }
  return true;
}

void StoreProcessedTransform(std::uint8_t *base, std::uint32_t transform,
                             const std::array<float, 16> &matrix) {
  for (std::uint32_t component = 0; component < matrix.size(); ++component) {
    REX_STORE_U32(transform + component * sizeof(float),
                  std::bit_cast<std::uint32_t>(matrix[component]));
  }
}

} // namespace

void PublishRemotePresentation(const char *map_name,
                               const float map_render_origin[3],
                               const RemotePresentationFrame &presentation) {
  const std::uint64_t now_us = NowMicroseconds();
  PublishedPresentation next;
  next.sequence = presentation.sequence;
  next.published_at_us = now_us;
  next.map_hash = HashMapName(map_name);
  next.world_valid = map_name != nullptr && map_name[0] != '\0' &&
                     Finite3(map_render_origin) && next.map_hash != 0;

  if (next.world_valid && presentation.players != nullptr) {
    for (const RemotePlayer &remote : *presentation.players) {
      if (remote.receiver_role < 1 || remote.receiver_role > 100 ||
          remote.receiver_session == 0 ||
          remote.presentation_map_hash != next.map_hash) {
        continue;
      }
      if (next.local_role == 0) {
        next.local_role = remote.receiver_role;
        next.local_session = remote.receiver_session;
      }
      if (remote.receiver_role != next.local_role ||
          remote.receiver_session != next.local_session ||
          remote.role == next.local_role || remote.role < 1 ||
          remote.role > 100 || remote.session == 0 ||
          !Finite3(remote.pose.position)) {
        continue;
      }

      RemoteProxySample sample;
      sample.role = remote.role;
      sample.session = remote.session;
      sample.map_hash = next.map_hash;
      sample.observed_at_us = now_us;
      sample.spatial_valid = true;
      sample.playing = remote.pose.board_state_flags != 0xFFFFFFFFu &&
                       (remote.pose.board_state_flags & 0x7u) == 0;
      for (std::size_t component = 0; component < 3; ++component) {
        sample.position[component] =
            map_render_origin[component] + remote.pose.position[component];
      }
      if (Finite3(sample.position.data())) {
        next.samples.push_back(sample);
      }
    }
  }
  next.enabled = next.world_valid && next.local_role != 0 &&
                 next.local_session != 0 && !next.samples.empty();

  std::scoped_lock lock(g_presentation_mutex);
  const bool source_advanced =
      next.sequence != g_presentation.sequence ||
      next.local_role != g_presentation.local_role ||
      next.local_session != g_presentation.local_session ||
      next.map_hash != g_presentation.map_hash;
  next.source_advanced_at_us =
      source_advanced || g_presentation.source_advanced_at_us == 0
          ? now_us
          : g_presentation.source_advanced_at_us;
  g_presentation = std::move(next);
}

void ApplyAfterPhysOut(PPCContext &ctx, std::uint8_t *base,
                       std::uint32_t controller, std::uint32_t phys_out) {
  const std::uint64_t now_us = NowMicroseconds();
  if (rex::kernel::guest_presence::GameplayContextValue() != 1) {
    Disable(DisabledReason::kMenu);
    return;
  }
  if (map_editor::Active()) {
    Disable(DisabledReason::kMapEditor);
    return;
  }
  if (!mechanics_sandbox::Active() || !mechanics_sandbox::VisualMapEnabled()) {
    Disable(DisabledReason::kInvalidWorld);
    return;
  }
  if (base == nullptr || controller == 0 || phys_out == 0 ||
      phys_out != trick_pipeline::CurrentLocalPhysOut()) {
    Disable(DisabledReason::kGuestStateInvalid);
    return;
  }

  trick_pipeline::LiveSpatialSnapshot spatial;
  if (!trick_pipeline::CurrentLiveSpatialSnapshot(spatial) ||
      spatial.phys_out != phys_out ||
      spatial.board_state_flags == 0xFFFFFFFFu ||
      (spatial.board_state_flags & 0x7u) != 0) {
    Disable(DisabledReason::kLocalNotPlaying);
    return;
  }

  PublishedPresentation presentation;
  {
    std::scoped_lock lock(g_presentation_mutex);
    presentation = g_presentation;
  }
  if (!presentation.enabled) {
    Disable(presentation.world_valid ? DisabledReason::kMultiplayerInactive
                                     : DisabledReason::kInvalidWorld);
    return;
  }
  if (presentation.published_at_us == 0 ||
      presentation.source_advanced_at_us == 0 ||
      now_us - std::min(now_us, presentation.published_at_us) >
          kMaximumPresentationAgeUs ||
      now_us - std::min(now_us, presentation.source_advanced_at_us) >
          kMaximumSourceAgeUs) {
    Disable(DisabledReason::kPresentationStale, true);
    return;
  }

  const std::uint32_t skateboard = LoadGuestU32(base, controller + 428);
  const std::uint32_t processed_phys_in = LoadGuestU32(base, controller + 436);
  const std::uint32_t transform_state = LoadGuestU32(base, controller + 448);
  if (!IsGuestHeapAddress(skateboard) ||
      !IsGuestHeapAddress(processed_phys_in) || transform_state > 4) {
    Disable(DisabledReason::kGuestStateInvalid);
    return;
  }
  const std::uint32_t transform =
      processed_phys_in + (transform_state == 3 ? 112u : 192u);
  if (!REXCVAR_GET(skate3_multiplayer_player_collision_test_spawn)) {
    g_facing_test_spawn = {};
  } else if (presentation.local_role <= 2) {
    const bool new_generation =
        g_facing_test_spawn.local_role != presentation.local_role ||
        g_facing_test_spawn.local_session != presentation.local_session ||
        g_facing_test_spawn.map_hash != presentation.map_hash;
    if (new_generation) {
      g_facing_test_spawn = {
          .local_role = presentation.local_role,
          .local_session = presentation.local_session,
          .map_hash = presentation.map_hash,
          .applied = false,
      };
    }
    if (!g_facing_test_spawn.applied) {
      const FacingTestSpawn placement = BuildFacingTestSpawn(
          presentation.local_role, LoadTransform(base, transform));
      if (placement.valid) {
        if (!StoreBoardTransform(ctx, base, skateboard, transform,
                                 placement.transform)) {
          Disable(DisabledReason::kGuestStateInvalid);
          return;
        }
        // Skate alternates between two ProcessedPhysIn board transforms. If
        // only the active one is seeded, the next state swap restores the old
        // heading even though the rigid-body translation remains separated.
        const std::uint32_t alternate_transform =
            processed_phys_in + (transform_state == 3 ? 192u : 112u);
        StoreProcessedTransform(base, alternate_transform, placement.transform);
        g_facing_test_spawn.applied = true;
        REXLOG_INFO("multiplayer-player-collision: facing-test-spawn role={} "
                    "spacing={:.1f}m buffers=2 session={} map={:08X}",
                    presentation.local_role, kFacingTestSpawnSpacing,
                    presentation.local_session, presentation.map_hash);
        return;
      }
    }
  }

  LocalSample local;
  local.position = {
      LoadGuestF32(base, transform + 48),
      LoadGuestF32(base, transform + 52),
      LoadGuestF32(base, transform + 56),
  };

  UpdateContext update;
  update.enabled = true;
  update.local_role = presentation.local_role;
  update.local_session = presentation.local_session;
  update.map_hash = presentation.map_hash;
  update.now_us = now_us;
  update.disabled_reason = DisabledReason::kNone;

  ResolveResult result;
  {
    std::scoped_lock lock(g_model_mutex);
    g_proxies.Update(update, presentation.samples);
    result = g_proxies.Resolve(local);
    MaybeLog(now_us);
  }
  if (result.contacts == 0 || (!std::isfinite(result.corrected_position[0]) ||
                               !std::isfinite(result.corrected_position[1]) ||
                               !std::isfinite(result.corrected_position[2]))) {
    return;
  }

  std::array<float, 16> matrix = LoadTransform(base, transform);
  matrix[12] = result.corrected_position[0];
  matrix[13] = result.corrected_position[1];
  matrix[14] = result.corrected_position[2];

  if (!StoreBoardTransform(ctx, base, skateboard, transform, matrix)) {
    Disable(DisabledReason::kGuestStateInvalid);
  }
}

void AppendTelemetry(std::ostream &out) {
  std::scoped_lock lock(g_model_mutex);
  const Counters &counters = g_proxies.counters();
  out << " multiplayer_player_collision_enabled="
      << (g_proxies.disabled_reason() == DisabledReason::kNone ? 1 : 0)
      << " multiplayer_player_collision_disabled_reason="
      << DisabledReasonName(g_proxies.disabled_reason())
      << " multiplayer_player_collision_proxy_count=" << g_proxies.size()
      << " multiplayer_player_collision_created=" << counters.created
      << " multiplayer_player_collision_updated=" << counters.updated
      << " multiplayer_player_collision_removed=" << counters.removed
      << " multiplayer_player_collision_stale_cleanups="
      << counters.stale_cleanups
      << " multiplayer_player_collision_contacts=" << counters.contacts;
}

} // namespace skate3::multiplayer::player_collision
