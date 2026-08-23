#include "skate3_native_collision.h"

#include "generated/skate3_init.h"
#include "skate/world/rw_collision_mesh.h"
#include "skate3_input_history_watch.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_trick_pipeline.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_native_collision, true, "Skate 3",
    "Compile the project-owned map to rw::collision::ClusteredMesh and "
    "register it in Skate 3's authoritative static-world collision "
    "collection. This does not override board transforms or grounded state.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_native_collision_replace_retail, true, "Skate 3",
    "After the owned ClusteredMesh is registered and verified in the native "
    "collection, remove the previously streamed retail static volumes. The "
    "owned mesh remains the sole static-world collision provider.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace skate3::native_collision {
namespace {

enum class State : std::uint8_t {
  Disabled = 0,
  WaitingForCollection,
  WaitingForPlacement,
  Compiling,
  AllocationFailed,
  BuildFailed,
  CollectionFull,
  InitializeFailed,
  RegistrationFailed,
  InstalledAdditive,
  InstalledExclusive,
  ReplacementFailed,
};

enum class KinematicState : std::uint8_t {
  Disabled = 0,
  WaitingForStaticWorld,
  Compiling,
  AllocationFailed,
  BuildFailed,
  CollectionFull,
  InitializeFailed,
  RegistrationFailed,
  MissingFromCollection,
  Installed,
};

enum class DoorState : std::uint8_t {
  Disabled = 0,
  WaitingForStaticWorld,
  Compiling,
  AllocationFailed,
  BuildFailed,
  CollectionFull,
  InitializeFailed,
  RegistrationFailed,
  MissingFromCollection,
  Installed,
};

constexpr std::uint32_t kMaximumReasonableCollectionCapacity = 4096;
constexpr std::uint32_t kCollectionEntrySize = 192;
constexpr std::uint32_t kAggregateVolumeSize = 96;
constexpr std::uint32_t kAuxiliaryAllocationSize = 192;
constexpr std::uint32_t kResourceOffset = 96;
constexpr std::uint32_t kMatrixOffset = 112;
constexpr std::uint32_t kGroundResultOffset = 176;
constexpr std::size_t kMaximumOwnedStaticChunks = 96;
constexpr std::size_t kMaximumHingedDoors = 32;
// University occupies 140 non-empty cells at 128 m, which exceeds the fixed
// collection capacity. At 256 m it occupies 44 cells, and its largest cell
// remains below kMaximumTrianglesPerOwnedChunk without dropping triangles.
constexpr float kOwnedCollisionCellSize = 256.0f;
constexpr std::size_t kMaximumTrianglesPerOwnedChunk = 400000;

std::atomic<State> g_state{State::Disabled};
std::atomic<std::uint32_t> g_world_streamer_view{0};
std::atomic<std::uint32_t> g_collection{0};
std::atomic<std::uint32_t> g_collection_capacity{0};
std::atomic<std::uint32_t> g_collection_read_entries{0};
std::atomic<std::uint32_t> g_collection_write_entries{0};
std::atomic<std::uint32_t> g_collection_count_before{0};
std::atomic<std::uint32_t> g_collection_count_after{0};
std::atomic<std::uint32_t> g_mesh_address{0};
std::atomic<std::uint32_t> g_volume_address{0};
std::atomic<std::uint32_t> g_mesh_bytes{0};
std::atomic<std::uint32_t> g_mesh_triangles{0};
std::atomic<std::uint32_t> g_mesh_vertices{0};
std::atomic<std::uint32_t> g_static_mesh_count{0};
std::array<std::atomic<std::uint32_t>, kMaximumOwnedStaticChunks>
    g_static_mesh_addresses{};
std::array<std::atomic<std::uint32_t>, kMaximumOwnedStaticChunks>
    g_static_volume_addresses{};
std::atomic<std::uint32_t> g_removed_retail_volumes{0};
std::atomic<std::uint64_t> g_exclusive_reconciliations{0};
std::atomic<std::uint64_t> g_reintroduced_retail_removed{0};
std::atomic<std::uint64_t> g_owned_static_readded{0};
std::atomic<std::uint64_t> g_exclusive_reconcile_failures{0};
std::atomic<std::uint64_t> g_suppressed_retail_batches{0};
std::atomic<std::uint64_t> g_suppressed_retail_volumes{0};
std::atomic<std::uint32_t> g_ground_x_bits{0};
std::atomic<std::uint32_t> g_ground_y_bits{0};
std::atomic<std::uint32_t> g_ground_z_bits{0};
std::atomic<std::uint32_t> g_world_origin_x_bits{0};
std::atomic<std::uint32_t> g_world_origin_y_bits{0};
std::atomic<std::uint32_t> g_world_origin_z_bits{0};
std::atomic<bool> g_world_origin_valid{false};
std::atomic<std::uint64_t> g_install_attempts{0};
std::atomic<std::uint32_t> g_live_collection_count{0};
std::atomic<std::uint32_t> g_live_read_owned_index{UINT32_MAX};
std::atomic<std::uint32_t> g_live_write_owned_index{UINT32_MAX};
std::atomic<std::uint32_t> g_live_owned_bbox_min_x_bits{0};
std::atomic<std::uint32_t> g_live_owned_bbox_min_y_bits{0};
std::atomic<std::uint32_t> g_live_owned_bbox_min_z_bits{0};
std::atomic<std::uint32_t> g_live_owned_bbox_max_x_bits{0};
std::atomic<std::uint32_t> g_live_owned_bbox_max_y_bits{0};
std::atomic<std::uint32_t> g_live_owned_bbox_max_z_bits{0};
std::atomic<std::uint64_t> g_native_line_workers{0};
std::atomic<std::uint64_t> g_native_owned_line_workers{0};
std::atomic<std::uint64_t> g_native_box_workers{0};
std::atomic<std::uint64_t> g_native_owned_box_workers{0};
std::atomic<std::uint64_t> g_native_iterators{0};
std::atomic<std::uint64_t> g_native_owned_iterators{0};
std::atomic<std::uint64_t> g_native_query_candidates{0};
std::atomic<std::uint32_t> g_native_last_candidate_mesh{0};
std::atomic<std::uint64_t> g_native_query_entries{0};
std::atomic<std::uint64_t> g_native_cluster_decodes{0};
std::atomic<std::uint64_t> g_native_decoded_triangles{0};
std::atomic<std::uint64_t> g_native_triangle_tests{0};
std::atomic<std::uint64_t> g_native_triangle_hits{0};
std::atomic<std::uint32_t> g_native_last_hit_mesh{0};
std::atomic<KinematicState> g_kinematic_state{
    KinematicState::Disabled};
std::atomic<std::uint32_t> g_kinematic_mesh_address{0};
std::atomic<std::uint32_t> g_kinematic_volume_address{0};
std::atomic<std::uint32_t> g_kinematic_auxiliary_address{0};
std::atomic<std::uint32_t> g_kinematic_matrix_address{0};
std::atomic<std::uint32_t> g_kinematic_mesh_bytes{0};
std::atomic<std::uint32_t> g_kinematic_mesh_triangles{0};
std::atomic<std::uint64_t> g_kinematic_epoch_frame{0};
std::atomic<std::uint64_t> g_kinematic_last_update_frame{0};
std::atomic<std::uint64_t> g_kinematic_updates{0};
std::atomic<std::uint64_t> g_kinematic_line_workers{0};
std::atomic<std::uint64_t> g_kinematic_box_workers{0};
std::atomic<std::uint64_t> g_kinematic_iterators{0};
std::atomic<std::uint64_t> g_kinematic_line_batches{0};
std::atomic<std::uint64_t> g_kinematic_visible_line_batches{0};
std::atomic<std::uint64_t> g_kinematic_linear_line_batches{0};
std::atomic<std::uint64_t> g_kinematic_linear_box_batches{0};
std::atomic<std::uint64_t> g_kinematic_batch_entry_refreshes{0};
std::atomic<std::uint32_t> g_kinematic_last_batch_entries{0};
std::atomic<std::uint32_t> g_kinematic_last_batch_count{0};
std::atomic<std::uint32_t> g_kinematic_last_batch_accelerator{0};
std::atomic<std::uint64_t> g_kinematic_query_entries{0};
std::atomic<std::uint64_t> g_kinematic_triangle_tests{0};
std::atomic<std::uint64_t> g_kinematic_triangle_hits{0};
std::atomic<std::uint32_t> g_kinematic_read_index{UINT32_MAX};
std::atomic<std::uint32_t> g_kinematic_write_index{UINT32_MAX};
std::atomic<std::uint32_t> g_kinematic_forward_x_bits{0};
std::atomic<std::uint32_t> g_kinematic_forward_y_bits{0};
std::atomic<std::uint32_t> g_kinematic_forward_z_bits{0};
std::atomic<std::uint32_t> g_kinematic_inverse_x_bits{0};
std::atomic<std::uint32_t> g_kinematic_inverse_y_bits{0};
std::atomic<std::uint32_t> g_kinematic_inverse_z_bits{0};
std::atomic<std::uint32_t> g_kinematic_bbox_min_x_bits{0};
std::atomic<std::uint32_t> g_kinematic_bbox_min_y_bits{0};
std::atomic<std::uint32_t> g_kinematic_bbox_min_z_bits{0};
std::atomic<std::uint32_t> g_kinematic_bbox_max_x_bits{0};
std::atomic<std::uint32_t> g_kinematic_bbox_max_y_bits{0};
std::atomic<std::uint32_t> g_kinematic_bbox_max_z_bits{0};
std::atomic<std::uint64_t> g_kinematic_pose_revision{0};
std::atomic<std::uint32_t> g_kinematic_position_x_bits{0};
std::atomic<std::uint32_t> g_kinematic_position_y_bits{0};
std::atomic<std::uint32_t> g_kinematic_position_z_bits{0};
std::atomic<std::uint32_t> g_kinematic_velocity_x_bits{0};
std::atomic<std::uint32_t> g_kinematic_velocity_y_bits{0};
std::atomic<std::uint32_t> g_kinematic_velocity_z_bits{0};
std::atomic<bool> g_kinematic_pose_valid{false};
std::atomic<DoorState> g_door_state{DoorState::Disabled};
std::atomic<std::uint32_t> g_door_count{0};
std::array<std::atomic<std::uint32_t>, kMaximumHingedDoors>
    g_door_mesh_addresses{};
std::array<std::atomic<std::uint32_t>, kMaximumHingedDoors>
    g_door_volume_addresses{};
std::array<std::atomic<std::uint32_t>, kMaximumHingedDoors>
    g_door_auxiliary_addresses{};
std::array<std::atomic<std::uint32_t>, kMaximumHingedDoors>
    g_door_matrix_addresses{};
std::array<std::atomic<std::uint32_t>, kMaximumHingedDoors>
    g_door_angle_bits{};
std::array<std::atomic<std::uint32_t>, kMaximumHingedDoors>
    g_door_velocity_bits{};
std::array<std::atomic<std::uint64_t>, kMaximumHingedDoors>
    g_door_pose_frames{};
std::array<std::atomic<std::uint64_t>, kMaximumHingedDoors>
    g_door_native_triangle_hits{};
std::array<std::uint64_t, kMaximumHingedDoors>
    g_door_consumed_triangle_hits{};
std::array<float, kMaximumHingedDoors> g_door_angles{};
std::array<float, kMaximumHingedDoors> g_door_angular_velocities{};
std::atomic<std::uint64_t> g_door_updates{0};
std::atomic<std::uint64_t> g_door_contact_impulses{0};
std::atomic<std::uint64_t> g_door_capsule_overlaps{0};
std::atomic<std::uint64_t> g_door_native_confirmed_contacts{0};
std::atomic<std::uint64_t> g_door_player_samples{0};
std::atomic<std::uint64_t> g_door_limit_hits{0};
std::atomic<std::uint64_t> g_door_triangle_hits{0};
std::uint64_t g_door_last_update_frame = 0;
float g_door_previous_player_position[3] = {};
bool g_door_previous_player_valid = false;
std::mutex g_install_mutex;
std::vector<std::uint32_t> g_original_volumes;
thread_local bool g_querying_owned_mesh = false;
thread_local bool g_querying_kinematic_mesh = false;
thread_local std::int32_t g_querying_door_index = -1;
thread_local std::uint32_t g_querying_mesh = 0;

bool IsGuestDataAddress(std::uint32_t address) {
  return address >= 0x00010000u && address < 0x80000000u;
}

bool IsOwnedStaticMesh(std::uint32_t mesh) {
  if (mesh == 0) {
    return false;
  }
  const std::uint32_t count = std::min<std::uint32_t>(
      g_static_mesh_count.load(std::memory_order_acquire),
      static_cast<std::uint32_t>(kMaximumOwnedStaticChunks));
  for (std::uint32_t index = 0; index < count; ++index) {
    if (g_static_mesh_addresses[index].load(
            std::memory_order_acquire) == mesh) {
      return true;
    }
  }
  return false;
}

std::int32_t OwnedDoorIndex(std::uint32_t mesh) {
  if (mesh == 0) {
    return -1;
  }
  const std::uint32_t count = std::min<std::uint32_t>(
      g_door_count.load(std::memory_order_acquire),
      static_cast<std::uint32_t>(kMaximumHingedDoors));
  for (std::uint32_t index = 0; index < count; ++index) {
    if (g_door_mesh_addresses[index].load(
            std::memory_order_acquire) == mesh) {
      return static_cast<std::int32_t>(index);
    }
  }
  return -1;
}

std::uint32_t LoadU32(std::uint8_t* base, std::uint32_t address) {
  if (!base || !IsGuestDataAddress(address)) {
    return 0;
  }
  return REX_LOAD_U32(address);
}

void StoreU32(std::uint8_t* base,
              std::uint32_t address,
              std::uint32_t value) {
  REX_STORE_U32(address, value);
}

void StoreF32(std::uint8_t* base, std::uint32_t address, float value) {
  StoreU32(base, address, std::bit_cast<std::uint32_t>(value));
}

const char* StateName(State state) {
  switch (state) {
    case State::Disabled:
      return "disabled";
    case State::WaitingForCollection:
      return "waiting_collection";
    case State::WaitingForPlacement:
      return "waiting_placement";
    case State::Compiling:
      return "compiling";
    case State::AllocationFailed:
      return "allocation_failed";
    case State::BuildFailed:
      return "build_failed";
    case State::CollectionFull:
      return "collection_full";
    case State::InitializeFailed:
      return "initialize_failed";
    case State::RegistrationFailed:
      return "registration_failed";
    case State::InstalledAdditive:
      return "installed_additive";
    case State::InstalledExclusive:
      return "installed_exclusive";
    case State::ReplacementFailed:
      return "replacement_failed";
  }
  return "unknown";
}

const char* KinematicStateName(KinematicState state) {
  switch (state) {
    case KinematicState::Disabled:
      return "disabled";
    case KinematicState::WaitingForStaticWorld:
      return "waiting_static_world";
    case KinematicState::Compiling:
      return "compiling";
    case KinematicState::AllocationFailed:
      return "allocation_failed";
    case KinematicState::BuildFailed:
      return "build_failed";
    case KinematicState::CollectionFull:
      return "collection_full";
    case KinematicState::InitializeFailed:
      return "initialize_failed";
    case KinematicState::RegistrationFailed:
      return "registration_failed";
    case KinematicState::MissingFromCollection:
      return "missing_from_collection";
    case KinematicState::Installed:
      return "installed";
  }
  return "unknown";
}

const char* DoorStateName(DoorState state) {
  switch (state) {
    case DoorState::Disabled:
      return "disabled";
    case DoorState::WaitingForStaticWorld:
      return "waiting_static_world";
    case DoorState::Compiling:
      return "compiling";
    case DoorState::AllocationFailed:
      return "allocation_failed";
    case DoorState::BuildFailed:
      return "build_failed";
    case DoorState::CollectionFull:
      return "collection_full";
    case DoorState::InitializeFailed:
      return "initialize_failed";
    case DoorState::RegistrationFailed:
      return "registration_failed";
    case DoorState::MissingFromCollection:
      return "missing_from_collection";
    case DoorState::Installed:
      return "installed";
  }
  return "unknown";
}

bool IsTerminal(State state) {
  return state == State::AllocationFailed || state == State::BuildFailed ||
         state == State::CollectionFull ||
         state == State::InitializeFailed ||
         state == State::RegistrationFailed ||
         state == State::InstalledAdditive ||
         state == State::InstalledExclusive ||
         state == State::ReplacementFailed;
}

bool CalculateNativeGroundPoint(PPCContext& source,
                                std::uint8_t* base,
                                std::uint32_t skateboard,
                                std::uint32_t result,
                                float out[3]) {
  if (!IsGuestDataAddress(skateboard) || !IsGuestDataAddress(result)) {
    return false;
  }

  PPCContext query = source;
  query.r3.u64 = result;
  query.r4.u64 = skateboard;
  sub_82C02840(query, base);

  out[0] = std::bit_cast<float>(LoadU32(base, result));
  out[1] = std::bit_cast<float>(LoadU32(base, result + 4));
  out[2] = std::bit_cast<float>(LoadU32(base, result + 8));
  return std::isfinite(out[0]) && std::isfinite(out[1]) &&
         std::isfinite(out[2]);
}

void WaitForCollectionJobs(PPCContext& source,
                           std::uint8_t* base,
                           std::uint32_t collection) {
  PPCContext wait = source;
  wait.r3.u64 = collection;
  sub_82775CB8(wait, base);
}

void PublishWriteEntries(std::uint8_t* base,
                         std::uint32_t read_entries,
                         std::uint32_t write_entries,
                         std::uint32_t count) {
  if (read_entries == write_entries || count == 0) {
    return;
  }
  std::memcpy(base + read_entries, base + write_entries,
              static_cast<std::size_t>(count) * kCollectionEntrySize);
}

std::uint32_t FindOwnedEntry(std::uint8_t* base,
                             std::uint32_t entries,
                             std::uint32_t count,
                             std::uint32_t owned_volume,
                             std::uint32_t owned_mesh) {
  if (!IsGuestDataAddress(entries) || owned_volume == 0 || owned_mesh == 0) {
    return UINT32_MAX;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t entry = entries + index * kCollectionEntrySize;
    if (LoadU32(base, entry) == owned_volume &&
        LoadU32(base, entry + 4) == owned_mesh) {
      return index;
    }
  }
  return UINT32_MAX;
}

void ObserveLiveCollection(std::uint8_t* base) {
  const std::uint32_t collection =
      g_collection.load(std::memory_order_acquire);
  const std::uint32_t owned_volume =
      g_volume_address.load(std::memory_order_acquire);
  const std::uint32_t owned_mesh =
      g_mesh_address.load(std::memory_order_acquire);
  if (!base || !IsGuestDataAddress(collection)) {
    return;
  }

  const std::uint32_t count = LoadU32(base, collection + 20);
  const std::uint32_t capacity = LoadU32(base, collection + 8);
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  const std::uint32_t write_entries = LoadU32(base, collection + 32);
  if (count > capacity || capacity > kMaximumReasonableCollectionCapacity) {
    return;
  }

  const std::uint32_t read_owned = FindOwnedEntry(
      base, read_entries, count, owned_volume, owned_mesh);
  const std::uint32_t write_owned = FindOwnedEntry(
      base, write_entries, count, owned_volume, owned_mesh);
  g_live_collection_count.store(count, std::memory_order_release);
  g_live_read_owned_index.store(read_owned, std::memory_order_release);
  g_live_write_owned_index.store(write_owned, std::memory_order_release);

  if (read_owned != UINT32_MAX) {
    const std::uint32_t entry =
        read_entries + read_owned * kCollectionEntrySize;
    g_live_owned_bbox_min_x_bits.store(LoadU32(base, entry + 144),
                                       std::memory_order_release);
    g_live_owned_bbox_min_y_bits.store(LoadU32(base, entry + 148),
                                       std::memory_order_release);
    g_live_owned_bbox_min_z_bits.store(LoadU32(base, entry + 152),
                                       std::memory_order_release);
    g_live_owned_bbox_max_x_bits.store(LoadU32(base, entry + 160),
                                       std::memory_order_release);
    g_live_owned_bbox_max_y_bits.store(LoadU32(base, entry + 164),
                                       std::memory_order_release);
    g_live_owned_bbox_max_z_bits.store(LoadU32(base, entry + 168),
                                       std::memory_order_release);
  }
}

void WriteMapTransform(std::uint8_t* base,
                       std::uint32_t matrix,
                       const float translation[3]) {
  for (std::uint32_t component = 0; component < 16; ++component) {
    StoreF32(base, matrix + component * sizeof(float), 0.0f);
  }
  StoreF32(base, matrix + 0, 1.0f);
  StoreF32(base, matrix + 20, 1.0f);
  StoreF32(base, matrix + 40, 1.0f);
  StoreF32(base, matrix + 48, translation[0]);
  StoreF32(base, matrix + 52, translation[1]);
  StoreF32(base, matrix + 56, translation[2]);
  StoreF32(base, matrix + 60, 1.0f);
}

skate::world::Vec3 RotateAroundAxis(skate::world::Vec3 value,
                                   skate::world::Vec3 axis,
                                   float angle) {
  const float cosine = std::cos(angle);
  const float sine = std::sin(angle);
  return value * cosine +
         skate::world::Cross(axis, value) * sine +
         axis * (skate::world::Dot(axis, value) * (1.0f - cosine));
}

void WriteBasisTransform(std::uint8_t* base,
                         std::uint32_t matrix,
                         skate::world::Vec3 x_axis,
                         skate::world::Vec3 y_axis,
                         skate::world::Vec3 z_axis,
                         skate::world::Vec3 translation) {
  for (std::uint32_t component = 0; component < 16; ++component) {
    StoreF32(base, matrix + component * sizeof(float), 0.0f);
  }
  StoreF32(base, matrix + 0, x_axis.x);
  StoreF32(base, matrix + 4, x_axis.y);
  StoreF32(base, matrix + 8, x_axis.z);
  StoreF32(base, matrix + 16, y_axis.x);
  StoreF32(base, matrix + 20, y_axis.y);
  StoreF32(base, matrix + 24, y_axis.z);
  StoreF32(base, matrix + 32, z_axis.x);
  StoreF32(base, matrix + 36, z_axis.y);
  StoreF32(base, matrix + 40, z_axis.z);
  StoreF32(base, matrix + 48, translation.x);
  StoreF32(base, matrix + 52, translation.y);
  StoreF32(base, matrix + 56, translation.z);
  StoreF32(base, matrix + 60, 1.0f);
}

void WriteEntryLocalBounds(
    std::uint8_t* base, std::uint32_t entry,
    const skate::world::KinematicBox& object) {
  // In the native linear collection path, the world query is transformed by
  // entry+80 first and then compared with these local-space aggregate bounds.
  StoreF32(base, entry + 144, object.local_min.x);
  StoreF32(base, entry + 148, object.local_min.y);
  StoreF32(base, entry + 152, object.local_min.z);
  StoreF32(base, entry + 160, object.local_max.x);
  StoreF32(base, entry + 164, object.local_max.y);
  StoreF32(base, entry + 168, object.local_max.z);
}

void WriteEntryLocalBounds(
    std::uint8_t* base, std::uint32_t entry,
    const skate::world::HingedDoor& door) {
  StoreF32(base, entry + 144, door.local_min.x);
  StoreF32(base, entry + 148, door.local_min.y);
  StoreF32(base, entry + 152, door.local_min.z);
  StoreF32(base, entry + 160, door.local_max.x);
  StoreF32(base, entry + 164, door.local_max.y);
  StoreF32(base, entry + 168, door.local_max.z);
}

void ObserveKinematicEntry(std::uint8_t* base,
                           std::uint32_t read_index,
                           std::uint32_t write_index,
                           std::uint32_t entry) {
  g_kinematic_read_index.store(read_index, std::memory_order_release);
  g_kinematic_write_index.store(write_index, std::memory_order_release);
  // Collection entries retain both the supplied local-to-world matrix at
  // +16 and the inverse generated by AggregateVolumeRef at +80.
  g_kinematic_forward_x_bits.store(LoadU32(base, entry + 64),
                                   std::memory_order_release);
  g_kinematic_forward_y_bits.store(LoadU32(base, entry + 68),
                                   std::memory_order_release);
  g_kinematic_forward_z_bits.store(LoadU32(base, entry + 72),
                                   std::memory_order_release);
  g_kinematic_inverse_x_bits.store(LoadU32(base, entry + 128),
                                   std::memory_order_release);
  g_kinematic_inverse_y_bits.store(LoadU32(base, entry + 132),
                                   std::memory_order_release);
  g_kinematic_inverse_z_bits.store(LoadU32(base, entry + 136),
                                   std::memory_order_release);
  g_kinematic_bbox_min_x_bits.store(LoadU32(base, entry + 144),
                                    std::memory_order_release);
  g_kinematic_bbox_min_y_bits.store(LoadU32(base, entry + 148),
                                    std::memory_order_release);
  g_kinematic_bbox_min_z_bits.store(LoadU32(base, entry + 152),
                                    std::memory_order_release);
  g_kinematic_bbox_max_x_bits.store(LoadU32(base, entry + 160),
                                    std::memory_order_release);
  g_kinematic_bbox_max_y_bits.store(LoadU32(base, entry + 164),
                                    std::memory_order_release);
  g_kinematic_bbox_max_z_bits.store(LoadU32(base, entry + 168),
                                    std::memory_order_release);
}

void PublishKinematicPose(const skate::world::KinematicPose& pose,
                          std::uint64_t frame) {
  g_kinematic_pose_revision.fetch_add(1, std::memory_order_acq_rel);
  g_kinematic_position_x_bits.store(
      std::bit_cast<std::uint32_t>(pose.position.x),
      std::memory_order_relaxed);
  g_kinematic_position_y_bits.store(
      std::bit_cast<std::uint32_t>(pose.position.y),
      std::memory_order_relaxed);
  g_kinematic_position_z_bits.store(
      std::bit_cast<std::uint32_t>(pose.position.z),
      std::memory_order_relaxed);
  g_kinematic_velocity_x_bits.store(
      std::bit_cast<std::uint32_t>(pose.velocity.x),
      std::memory_order_relaxed);
  g_kinematic_velocity_y_bits.store(
      std::bit_cast<std::uint32_t>(pose.velocity.y),
      std::memory_order_relaxed);
  g_kinematic_velocity_z_bits.store(
      std::bit_cast<std::uint32_t>(pose.velocity.z),
      std::memory_order_relaxed);
  g_kinematic_last_update_frame.store(frame, std::memory_order_relaxed);
  g_kinematic_pose_valid.store(true, std::memory_order_relaxed);
  g_kinematic_pose_revision.fetch_add(1, std::memory_order_release);
}

struct OwnedCollisionBuildSet {
  std::vector<skate::world::RwCollisionBuildResult> chunks;
  std::string error;
};

OwnedCollisionBuildSet CompileOwnedMapChunks(
    const float world_translation[3]) {
  OwnedCollisionBuildSet result;
  skate::world::RwCollisionBuildOptions options;
  options.translation = {
      world_translation[0], world_translation[1], world_translation[2]};
  const std::uint16_t concrete =
      skate::world::EncodeRwSurfaceId(3, 1, 0);
  options.default_surface_id = concrete;
  const skate::world::MapDefinition& source =
      mechanics_sandbox::map::ActiveDefinition();
  std::unordered_set<skate::world::MaterialId> used_materials;
  used_materials.reserve(source.collision_triangles.size() / 8 + 1);
  for (const skate::world::CollisionTriangle& triangle :
       source.collision_triangles) {
    used_materials.insert(triangle.material);
  }
  for (const skate::world::SurfaceMaterial& material :
       source.materials) {
    const std::uint16_t native_surface =
        skate::world::EncodeRwSurfaceId(
            material.skate_audio_surface,
            material.skate_physics_surface,
            material.skate_surface_pattern);
    options.material_surface_ids.emplace(
        material.id, native_surface);
    if (used_materials.contains(material.id)) {
      REXLOG_INFO(
          "native-collision: owned material {} '{}' -> surface=0x{:04X} "
          "audio={} physics={} pattern={}",
          material.id, material.name, native_surface,
          material.skate_audio_surface,
          material.skate_physics_surface,
          material.skate_surface_pattern);
    }
  }

  // Preserve triangle adjacency across the whole authored map whenever the
  // native format can represent it. The ClusteredMesh already contains its
  // own KD tree, so dividing an ordinary map into arbitrary 128 m top-level
  // volumes only destroys edge adjacency at cell boundaries. Those invisible
  // seams can make a board hop, stutter, or bail while crossing an otherwise
  // continuous floor or ramp.
  skate::world::RwCollisionBuildResult unified =
      skate::world::BuildRwCollisionMesh(source, options);
  if (unified.ok && !unified.mesh.bytes.empty()) {
    REXLOG_INFO(
        "native-collision: compiled continuous map triangles={} vertices={} "
        "clusters={} bytes={}",
        unified.mesh.triangle_count, unified.mesh.vertex_count,
        unified.mesh.cluster_count, unified.mesh.bytes.size());
    result.chunks.push_back(std::move(unified));
    return result;
  }

  // Extremely large maps may exceed a ClusteredMesh format limit. Retain a
  // spatial fallback so they still load, while making the loss of cross-cell
  // adjacency explicit in the log for diagnosis.
  REXLOG_WARN(
      "native-collision: continuous build failed ({}); falling back to "
      "{} m spatial chunks",
      unified.error.empty() ? "unknown error" : unified.error,
      kOwnedCollisionCellSize);

  using Cell = std::pair<std::int32_t, std::int32_t>;
  std::map<Cell, std::vector<skate::world::CollisionTriangle>> cells;
  for (const skate::world::CollisionTriangle& triangle :
       source.collision_triangles) {
    const float center_x =
        (triangle.a.x + triangle.b.x + triangle.c.x) / 3.0f;
    const float center_z =
        (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0f;
    const Cell cell{
        static_cast<std::int32_t>(
            std::floor(center_x / kOwnedCollisionCellSize)),
        static_cast<std::int32_t>(
            std::floor(center_z / kOwnedCollisionCellSize))};
    cells[cell].push_back(triangle);
  }

  std::size_t expected_chunks = 0;
  for (const auto& [cell, triangles] : cells) {
    (void)cell;
    expected_chunks +=
        (triangles.size() + kMaximumTrianglesPerOwnedChunk - 1) /
        kMaximumTrianglesPerOwnedChunk;
  }
  if (expected_chunks == 0 ||
      expected_chunks > kMaximumOwnedStaticChunks) {
    result.error =
        "spatial partition requires " +
        std::to_string(expected_chunks) +
        " native meshes; maximum is " +
        std::to_string(kMaximumOwnedStaticChunks);
    return result;
  }
  result.chunks.reserve(expected_chunks);

  for (auto& [cell, triangles] : cells) {
    for (std::size_t first = 0; first < triangles.size();
         first += kMaximumTrianglesPerOwnedChunk) {
      const std::size_t count = std::min(
          kMaximumTrianglesPerOwnedChunk,
          triangles.size() - first);
      skate::world::MapDefinition chunk;
      chunk.name =
          "owned_collision_" + std::to_string(cell.first) + "_" +
          std::to_string(cell.second) + "_" +
          std::to_string(first / kMaximumTrianglesPerOwnedChunk);
      chunk.collision_triangles.insert(
          chunk.collision_triangles.end(),
          triangles.begin() + static_cast<std::ptrdiff_t>(first),
          triangles.begin() +
              static_cast<std::ptrdiff_t>(first + count));
      skate::world::RwCollisionBuildResult build =
          skate::world::BuildRwCollisionMesh(chunk, options);
      if (!build.ok || build.mesh.bytes.empty()) {
        result.error =
            chunk.name + ": " +
            (build.error.empty() ? "empty native mesh" : build.error);
        result.chunks.clear();
        return result;
      }
      REXLOG_INFO(
          "native-collision: compiled chunk '{}' triangles={} vertices={} "
          "clusters={} bytes={}",
          chunk.name, build.mesh.triangle_count,
          build.mesh.vertex_count, build.mesh.cluster_count,
          build.mesh.bytes.size());
      result.chunks.push_back(std::move(build));
    }
  }
  return result;
}

skate::world::RwCollisionBuildResult CompileKinematicBox(
    const skate::world::KinematicBox& object) {
  skate::world::MapBuilder builder(object.name + "_collision");
  const skate::world::MaterialId material = builder.AddMaterial(
      "kinematic_surface", 0.82f, 0.01f,
      skate::world::SurfaceFlags::Skateable);
  builder.AddBox(object.surface, material,
                 object.local_min, object.local_max);

  skate::world::RwCollisionBuildOptions options;
  options.default_surface_id =
      skate::world::EncodeRwSurfaceId(3, 1, 0);
  options.material_surface_ids.emplace(
      material, options.default_surface_id);
  return skate::world::BuildRwCollisionMesh(
      std::move(builder).Build(), options);
}

skate::world::RwCollisionBuildResult CompileHingedDoor(
    const skate::world::HingedDoor& door,
    const skate::world::MapDefinition& source) {
  skate::world::MapDefinition local;
  local.name = door.name + "_collision";
  local.collision_triangles = door.collision_triangles;
  skate::world::RwCollisionBuildOptions options;
  options.default_surface_id =
      skate::world::EncodeRwSurfaceId(42, 1, 0);
  for (const skate::world::SurfaceMaterial& material :
       source.materials) {
    options.material_surface_ids.emplace(
        material.id,
        skate::world::EncodeRwSurfaceId(
            material.skate_audio_surface,
            material.skate_physics_surface,
            material.skate_surface_pattern));
  }
  return skate::world::BuildRwCollisionMesh(local, options);
}

bool RemoveOriginalVolumes(PPCContext& source,
                           std::uint8_t* base,
                           std::uint32_t collection,
                           std::uint32_t owned_volume) {
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  const std::uint32_t write_entries = LoadU32(base, collection + 32);
  if (!IsGuestDataAddress(read_entries) ||
      !IsGuestDataAddress(write_entries)) {
    return false;
  }

  std::uint32_t removed = 0;
  for (std::uint32_t volume : g_original_volumes) {
    if (!IsGuestDataAddress(volume) || volume == owned_volume) {
      continue;
    }
    const std::uint32_t before = LoadU32(base, collection + 20);
    PPCContext remove = source;
    remove.r3.u64 = collection;
    remove.r4.u64 = volume;
    sub_82775FC8(remove, base);
    const std::uint32_t after = LoadU32(base, collection + 20);
    if (after + 1 != before) {
      g_removed_retail_volumes.store(removed, std::memory_order_release);
      return false;
    }
    // RemoveVolume drains active batches and reconstructs the compacted
    // write buffer. Publish it before the next removal, whose lookup walks
    // the read buffer.
    PublishWriteEntries(base, read_entries, write_entries, after);
    ++removed;
  }
  g_removed_retail_volumes.store(removed, std::memory_order_release);
  return true;
}

std::uint32_t OwnedStaticEntryIndex(std::uint32_t volume,
                                    std::uint32_t mesh) {
  const std::uint32_t count = std::min<std::uint32_t>(
      g_static_mesh_count.load(std::memory_order_acquire),
      static_cast<std::uint32_t>(kMaximumOwnedStaticChunks));
  for (std::uint32_t index = 0; index < count; ++index) {
    if (g_static_volume_addresses[index].load(
            std::memory_order_acquire) == volume &&
        g_static_mesh_addresses[index].load(
            std::memory_order_acquire) == mesh) {
      return index;
    }
  }
  return UINT32_MAX;
}

bool IsOwnedKinematicEntry(std::uint32_t volume, std::uint32_t mesh) {
  return volume != 0 && mesh != 0 &&
         volume ==
             g_kinematic_volume_address.load(std::memory_order_acquire) &&
         mesh == g_kinematic_mesh_address.load(std::memory_order_acquire);
}

bool IsOwnedDoorEntry(std::uint32_t volume, std::uint32_t mesh) {
  const std::int32_t index = OwnedDoorIndex(mesh);
  return index >= 0 &&
         volume ==
             g_door_volume_addresses[
                 static_cast<std::size_t>(index)]
                 .load(std::memory_order_acquire);
}

bool ExclusiveCollectionDrifted(std::uint8_t* base,
                                std::uint32_t collection) {
  if (!base || !IsGuestDataAddress(collection)) {
    return false;
  }
  const std::uint32_t count = LoadU32(base, collection + 20);
  const std::uint32_t capacity = LoadU32(base, collection + 8);
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  if (count > capacity || capacity > kMaximumReasonableCollectionCapacity ||
      !IsGuestDataAddress(read_entries)) {
    return false;
  }

  const std::uint32_t owned_count = std::min<std::uint32_t>(
      g_static_mesh_count.load(std::memory_order_acquire),
      static_cast<std::uint32_t>(kMaximumOwnedStaticChunks));
  std::array<bool, kMaximumOwnedStaticChunks> seen{};
  bool unexpected = false;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t entry =
        read_entries + index * kCollectionEntrySize;
    const std::uint32_t volume = LoadU32(base, entry);
    const std::uint32_t mesh = LoadU32(base, entry + 4);
    const std::uint32_t owned_index =
        OwnedStaticEntryIndex(volume, mesh);
    if (owned_index != UINT32_MAX) {
      seen[owned_index] = true;
    } else if (!IsOwnedKinematicEntry(volume, mesh) &&
               !IsOwnedDoorEntry(volume, mesh)) {
      unexpected = true;
    }
  }
  if (unexpected) {
    return true;
  }
  for (std::uint32_t index = 0; index < owned_count; ++index) {
    if (!seen[index]) {
      return true;
    }
  }
  return false;
}

bool ReconcileExclusiveCollection(PPCContext& source,
                                  std::uint8_t* base,
                                  std::uint32_t collection) {
  if (!base || !IsGuestDataAddress(collection)) {
    return false;
  }

  // Return-to-marker/session lifecycle work can republish retail static
  // volumes after the original owned-world replacement. Drain active native
  // queries before mutating the authoritative double-buffered collection.
  WaitForCollectionJobs(source, base, collection);

  const std::uint32_t capacity = LoadU32(base, collection + 8);
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  const std::uint32_t write_entries = LoadU32(base, collection + 32);
  std::uint32_t count = LoadU32(base, collection + 20);
  if (capacity == 0 || capacity > kMaximumReasonableCollectionCapacity ||
      count > capacity || !IsGuestDataAddress(read_entries) ||
      !IsGuestDataAddress(write_entries)) {
    return false;
  }

  std::vector<std::uint32_t> unexpected_volumes;
  unexpected_volumes.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t entry =
        read_entries + index * kCollectionEntrySize;
    const std::uint32_t volume = LoadU32(base, entry);
    const std::uint32_t mesh = LoadU32(base, entry + 4);
    if (OwnedStaticEntryIndex(volume, mesh) == UINT32_MAX &&
        !IsOwnedKinematicEntry(volume, mesh) &&
        !IsOwnedDoorEntry(volume, mesh)) {
      if (!IsGuestDataAddress(volume)) {
        return false;
      }
      unexpected_volumes.push_back(volume);
    }
  }

  std::uint32_t removed = 0;
  for (std::uint32_t volume : unexpected_volumes) {
    const std::uint32_t before = LoadU32(base, collection + 20);
    PPCContext remove = source;
    remove.r3.u64 = collection;
    remove.r4.u64 = volume;
    sub_82775FC8(remove, base);
    const std::uint32_t after = LoadU32(base, collection + 20);
    if (after + 1 != before) {
      return false;
    }
    PublishWriteEntries(base, read_entries, write_entries, after);
    ++removed;
  }

  count = LoadU32(base, collection + 20);
  const std::uint32_t owned_count = std::min<std::uint32_t>(
      g_static_mesh_count.load(std::memory_order_acquire),
      static_cast<std::uint32_t>(kMaximumOwnedStaticChunks));
  std::uint32_t readded = 0;
  for (std::uint32_t index = 0; index < owned_count; ++index) {
    const std::uint32_t volume =
        g_static_volume_addresses[index].load(std::memory_order_acquire);
    const std::uint32_t mesh =
        g_static_mesh_addresses[index].load(std::memory_order_acquire);
    if (!IsGuestDataAddress(volume) || !IsGuestDataAddress(mesh)) {
      return false;
    }
    if (FindOwnedEntry(base, read_entries, count, volume, mesh) !=
        UINT32_MAX) {
      continue;
    }
    if (count >= capacity) {
      return false;
    }

    PPCContext add = source;
    add.r3.u64 = collection;
    add.r4.u64 = volume;
    add.r5.u64 = volume + kMatrixOffset;
    add.r6.u64 = 0;
    add.r7.u64 = 0;
    sub_82775F58(add, base);

    const std::uint32_t after = LoadU32(base, collection + 20);
    if (after != count + 1 ||
        LoadU32(base, write_entries + count * kCollectionEntrySize) !=
            volume ||
        LoadU32(base, write_entries + count * kCollectionEntrySize + 4) !=
            mesh) {
      return false;
    }
    PublishWriteEntries(base, read_entries, write_entries, after);
    count = after;
    ++readded;
  }

  g_collection_count_after.store(count, std::memory_order_release);
  g_removed_retail_volumes.fetch_add(removed, std::memory_order_relaxed);
  g_reintroduced_retail_removed.fetch_add(
      removed, std::memory_order_relaxed);
  g_owned_static_readded.fetch_add(readded, std::memory_order_relaxed);
  g_exclusive_reconciliations.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO(
      "native-collision: reconciled exclusive collection "
      "(removed_reintroduced={} readded_owned={} count={})",
      removed, readded, count);
  return true;
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(skate3_mechanics_sandbox_native_collision);
}

void ObserveNativeLineWorker(std::uint32_t mesh) noexcept {
  g_native_line_workers.fetch_add(1, std::memory_order_relaxed);
  const bool owned_static = IsOwnedStaticMesh(mesh);
  const bool owned_kinematic =
      mesh != 0 &&
      mesh == g_kinematic_mesh_address.load(std::memory_order_acquire);
  const bool owned_door = OwnedDoorIndex(mesh) >= 0;
  if (owned_static || owned_kinematic || owned_door) {
    g_native_owned_line_workers.fetch_add(1, std::memory_order_relaxed);
  }
  if (owned_kinematic) {
    g_kinematic_line_workers.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObserveNativeBoxWorker(std::uint32_t mesh) noexcept {
  g_native_box_workers.fetch_add(1, std::memory_order_relaxed);
  const bool owned_static = IsOwnedStaticMesh(mesh);
  const bool owned_kinematic =
      mesh != 0 &&
      mesh == g_kinematic_mesh_address.load(std::memory_order_acquire);
  const bool owned_door = OwnedDoorIndex(mesh) >= 0;
  if (owned_static || owned_kinematic || owned_door) {
    g_native_owned_box_workers.fetch_add(1, std::memory_order_relaxed);
  }
  if (owned_kinematic) {
    g_kinematic_box_workers.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObserveNativeIteratorMesh(std::uint32_t mesh) noexcept {
  g_native_iterators.fetch_add(1, std::memory_order_relaxed);
  const bool owned_static = IsOwnedStaticMesh(mesh);
  const bool owned_kinematic =
      mesh != 0 &&
      mesh == g_kinematic_mesh_address.load(std::memory_order_acquire);
  const bool owned_door = OwnedDoorIndex(mesh) >= 0;
  if (owned_static || owned_kinematic || owned_door) {
    g_native_owned_iterators.fetch_add(1, std::memory_order_relaxed);
  }
  if (owned_kinematic) {
    g_kinematic_iterators.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObserveNativeQueryMesh(std::uint32_t mesh) noexcept {
  g_native_query_candidates.fetch_add(1, std::memory_order_relaxed);
  g_native_last_candidate_mesh.store(mesh, std::memory_order_relaxed);
  const std::uint32_t kinematic_mesh =
      g_kinematic_mesh_address.load(std::memory_order_acquire);
  g_querying_kinematic_mesh =
      kinematic_mesh != 0 && mesh == kinematic_mesh;
  g_querying_door_index = OwnedDoorIndex(mesh);
  g_querying_mesh = mesh;
  g_querying_owned_mesh =
      IsOwnedStaticMesh(mesh) ||
      g_querying_kinematic_mesh ||
      g_querying_door_index >= 0;
  if (g_querying_owned_mesh) {
    g_native_query_entries.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_querying_kinematic_mesh) {
    g_kinematic_query_entries.fetch_add(1, std::memory_order_relaxed);
  }
}

bool PrepareKinematicQueryBatch(std::uint32_t batch,
                                std::uint8_t* base,
                                bool record_line_telemetry) noexcept {
  if (base == nullptr || !IsGuestDataAddress(batch)) {
    return false;
  }
  const std::uint32_t entries = LoadU32(base, batch);
  const std::uint32_t count = LoadU32(base, batch + 4);
  const std::uint32_t accelerator = LoadU32(base, batch + 8);
  if (record_line_telemetry) {
    g_kinematic_last_batch_entries.store(entries,
                                         std::memory_order_relaxed);
    g_kinematic_last_batch_count.store(count, std::memory_order_relaxed);
    g_kinematic_last_batch_accelerator.store(
        accelerator, std::memory_order_relaxed);
  }
  if (!IsGuestDataAddress(entries) ||
      count > kMaximumReasonableCollectionCapacity) {
    return false;
  }
  bool found_dynamic = false;
  const std::uint32_t collection =
      g_collection.load(std::memory_order_acquire);
  const std::uint32_t source_entries =
      IsGuestDataAddress(collection)
          ? LoadU32(base, collection + 16)
          : 0;
  const std::uint32_t source_count =
      IsGuestDataAddress(collection)
          ? LoadU32(base, collection + 20)
          : 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t entry = entries + index * kCollectionEntrySize;
    const std::uint32_t volume = LoadU32(base, entry);
    const std::uint32_t mesh = LoadU32(base, entry + 4);
    const bool kinematic =
        volume != 0 && mesh != 0 &&
        volume ==
            g_kinematic_volume_address.load(std::memory_order_acquire) &&
        mesh ==
            g_kinematic_mesh_address.load(std::memory_order_acquire);
    const std::int32_t door_index = OwnedDoorIndex(mesh);
    const bool door =
        door_index >= 0 &&
        volume == g_door_volume_addresses[
                      static_cast<std::size_t>(door_index)]
                      .load(std::memory_order_acquire);
    if (kinematic || door) {
      found_dynamic = true;
      if (record_line_telemetry) {
        g_kinematic_visible_line_batches.fetch_add(
            1, std::memory_order_relaxed);
      }
      // The collection accelerator was built for immutable streamed entries
      // and cannot follow a moving AggregateVolumeRef. Select Skate's native
      // linear top-level fallback for mixed batches. The static map remains
      // one ClusteredMesh entry and retains its internal KD tree, so this is
      // O(static-entry + kinematic-entry count), not O(map triangles).
      if (accelerator != 0) {
        StoreU32(base, batch + 8, 0);
      }
      if (IsGuestDataAddress(source_entries)) {
        const std::uint32_t source_index = FindOwnedEntry(
            base, source_entries, source_count, volume, mesh);
        if (source_index != UINT32_MAX) {
          const std::uint32_t source =
              source_entries +
              source_index * kCollectionEntrySize;
          if (source != entry) {
            // Query batches snapshot complete AggregateVolumeRef entries.
            // Refresh the mutable entry before the native linear broadphase
            // consumes it so bounds, transform, and inverse stay coherent.
            std::memcpy(base + entry, base + source,
                        kCollectionEntrySize);
            g_kinematic_batch_entry_refreshes.fetch_add(
                1, std::memory_order_relaxed);
          }
        }
      }
    }
  }
  return found_dynamic;
}

void ObserveNativeLineQueryBatch(std::uint32_t batch,
                                 std::uint8_t* base) noexcept {
  g_kinematic_line_batches.fetch_add(1, std::memory_order_relaxed);
  if (PrepareKinematicQueryBatch(batch, base, true)) {
    g_kinematic_linear_line_batches.fetch_add(
        1, std::memory_order_relaxed);
  }
}

void PrepareNativeBoxQueryBatch(std::uint32_t batch,
                                std::uint8_t* base) noexcept {
  if (PrepareKinematicQueryBatch(batch, base, false)) {
    g_kinematic_linear_box_batches.fetch_add(
        1, std::memory_order_relaxed);
  }
}

void ObserveNativeClusterDecode(
    std::uint32_t triangle_count) noexcept {
  if (!g_querying_owned_mesh) {
    return;
  }
  g_native_cluster_decodes.fetch_add(1, std::memory_order_relaxed);
  g_native_decoded_triangles.fetch_add(triangle_count,
                                       std::memory_order_relaxed);
}

void ObserveNativeTriangleResult(std::uint32_t hit) noexcept {
  if (!g_querying_owned_mesh) {
    return;
  }
  g_native_triangle_tests.fetch_add(1, std::memory_order_relaxed);
  if (g_querying_kinematic_mesh) {
    g_kinematic_triangle_tests.fetch_add(1,
                                         std::memory_order_relaxed);
  }
  if (hit != 0) {
    g_native_triangle_hits.fetch_add(1, std::memory_order_relaxed);
    g_native_last_hit_mesh.store(g_querying_mesh,
                                 std::memory_order_relaxed);
    if (g_querying_kinematic_mesh) {
      g_kinematic_triangle_hits.fetch_add(1,
                                          std::memory_order_relaxed);
    }
    if (g_querying_door_index >= 0) {
      g_door_triangle_hits.fetch_add(1, std::memory_order_relaxed);
      const std::size_t door_index =
          static_cast<std::size_t>(g_querying_door_index);
      if (door_index < kMaximumHingedDoors) {
        g_door_native_triangle_hits[door_index].fetch_add(
            1, std::memory_order_relaxed);
      }
    }
  }
}

bool MapWorldOrigin(float out_origin[3]) noexcept {
  if (out_origin == nullptr ||
      !g_world_origin_valid.load(std::memory_order_acquire)) {
    return false;
  }
  out_origin[0] = std::bit_cast<float>(
      g_world_origin_x_bits.load(std::memory_order_acquire));
  out_origin[1] = std::bit_cast<float>(
      g_world_origin_y_bits.load(std::memory_order_acquire));
  out_origin[2] = std::bit_cast<float>(
      g_world_origin_z_bits.load(std::memory_order_acquire));
  return std::isfinite(out_origin[0]) && std::isfinite(out_origin[1]) &&
         std::isfinite(out_origin[2]);
}

bool KinematicObjectPose(std::size_t index, float out_position[3],
                         float out_velocity[3],
                         std::uint64_t* out_frame) noexcept {
  if (index != 0 || out_position == nullptr ||
      out_velocity == nullptr ||
      !g_kinematic_pose_valid.load(std::memory_order_acquire)) {
    return false;
  }

  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::uint64_t before =
        g_kinematic_pose_revision.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
      continue;
    }
    const float position[3] = {
        std::bit_cast<float>(
            g_kinematic_position_x_bits.load(std::memory_order_relaxed)),
        std::bit_cast<float>(
            g_kinematic_position_y_bits.load(std::memory_order_relaxed)),
        std::bit_cast<float>(
            g_kinematic_position_z_bits.load(std::memory_order_relaxed)),
    };
    const float velocity[3] = {
        std::bit_cast<float>(
            g_kinematic_velocity_x_bits.load(std::memory_order_relaxed)),
        std::bit_cast<float>(
            g_kinematic_velocity_y_bits.load(std::memory_order_relaxed)),
        std::bit_cast<float>(
            g_kinematic_velocity_z_bits.load(std::memory_order_relaxed)),
    };
    const std::uint64_t frame =
        g_kinematic_last_update_frame.load(std::memory_order_relaxed);
    const std::uint64_t after =
        g_kinematic_pose_revision.load(std::memory_order_acquire);
    if (before != after || (after & 1u) != 0u) {
      continue;
    }
    if (!std::isfinite(position[0]) ||
        !std::isfinite(position[1]) ||
        !std::isfinite(position[2]) ||
        !std::isfinite(velocity[0]) ||
        !std::isfinite(velocity[1]) ||
        !std::isfinite(velocity[2])) {
      return false;
    }
    std::copy(std::begin(position), std::end(position), out_position);
    std::copy(std::begin(velocity), std::end(velocity), out_velocity);
    if (out_frame != nullptr) {
      *out_frame = frame;
    }
    return true;
  }
  return false;
}

bool HingedDoorPose(std::size_t index, float* out_angle_radians,
                    float* out_angular_velocity,
                    std::uint64_t* out_frame) noexcept {
  const std::uint32_t count = g_door_count.load(std::memory_order_acquire);
  if (index >= count || index >= kMaximumHingedDoors ||
      out_angle_radians == nullptr || out_angular_velocity == nullptr) {
    return false;
  }
  const float angle = std::bit_cast<float>(
      g_door_angle_bits[index].load(std::memory_order_acquire));
  const float velocity = std::bit_cast<float>(
      g_door_velocity_bits[index].load(std::memory_order_acquire));
  if (!std::isfinite(angle) || !std::isfinite(velocity)) {
    return false;
  }
  *out_angle_radians = angle;
  *out_angular_velocity = velocity;
  if (out_frame != nullptr) {
    *out_frame =
        g_door_pose_frames[index].load(std::memory_order_acquire);
  }
  return true;
}

bool ShouldSuppressWorldStreamerAddVolume(const PPCContext& ctx,
                                          std::uint8_t* base) noexcept {
  if (!base) {
    return false;
  }
  if (!Enabled()) {
    g_state.store(State::Disabled, std::memory_order_release);
  } else if (g_state.load(std::memory_order_acquire) == State::Disabled) {
    g_state.store(State::WaitingForCollection, std::memory_order_release);
  }

  const std::uint32_t view = ctx.r3.u32;
  const std::uint32_t view_state = LoadU32(base, view + 4);
  const std::uint32_t collection = LoadU32(base, view_state + 44);
  if (!IsGuestDataAddress(view) || !IsGuestDataAddress(view_state) ||
      !IsGuestDataAddress(collection)) {
    return false;
  }

  const std::uint32_t capacity = LoadU32(base, collection + 8);
  const std::uint32_t entries = LoadU32(base, collection + 32);
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  if (capacity == 0 || capacity > kMaximumReasonableCollectionCapacity ||
      !IsGuestDataAddress(read_entries) ||
      !IsGuestDataAddress(entries)) {
    return false;
  }

  g_world_streamer_view.store(view, std::memory_order_release);
  g_collection.store(collection, std::memory_order_release);
  g_collection_capacity.store(capacity, std::memory_order_release);
  g_collection_read_entries.store(read_entries,
                                  std::memory_order_release);
  g_collection_write_entries.store(entries,
                                   std::memory_order_release);
  if (Enabled() &&
      g_state.load(std::memory_order_acquire) ==
          State::WaitingForCollection) {
    g_state.store(State::WaitingForPlacement, std::memory_order_release);
  }

  if (!REXCVAR_GET(
          skate3_mechanics_sandbox_native_collision_replace_retail) ||
      g_state.load(std::memory_order_acquire) !=
          State::InstalledExclusive) {
    return false;
  }

  // WorldStreamerView publishes a batch one function call after this seam.
  // Reconciliation from the later mechanics update was too late: a newly
  // streamed retail volume could participate in one physics step while its
  // matching retail visuals were hidden. Reject the whole retail batch before
  // AddVolume mutates either collection buffer. Owned static/kinematic
  // registration calls the collection function directly and never crosses
  // this retail streamer boundary.
  const std::uint32_t streamer_item = ctx.r4.u32;
  if (!IsGuestDataAddress(streamer_item)) {
    return false;
  }
  const std::uint32_t descriptor =
      LoadU32(base, streamer_item + 132);
  if (!IsGuestDataAddress(descriptor)) {
    return false;
  }
  const std::uint32_t volume_count = LoadU32(base, descriptor + 4);
  const std::uint32_t volume_entries = LoadU32(base, descriptor + 8);
  if (volume_count == 0 ||
      volume_count > kMaximumReasonableCollectionCapacity ||
      !IsGuestDataAddress(volume_entries)) {
    return false;
  }

  const std::uint64_t batch =
      g_suppressed_retail_batches.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  g_suppressed_retail_volumes.fetch_add(
      volume_count, std::memory_order_relaxed);
  if (batch <= 16 || (batch & 255u) == 0) {
    REXLOG_INFO(
        "native-collision: suppressed retail streamer batch before publish "
        "(batch={} volumes={} collection_count={})",
        batch, volume_count, LoadU32(base, collection + 20));
  }
  return true;
}

void ObservePlayerCollisionTelemetry(const float world_position[3],
                                     std::uint64_t frame) noexcept {
  if (world_position == nullptr ||
      g_state.load(std::memory_order_acquire) !=
          State::InstalledExclusive) {
    return;
  }
  float translation[3] = {};
  if (!MapWorldOrigin(translation)) {
    return;
  }
  const float local[3] = {
      world_position[0] - translation[0],
      world_position[1] - translation[1],
      world_position[2] - translation[2],
  };
  if (!std::isfinite(local[0]) || !std::isfinite(local[1]) ||
      !std::isfinite(local[2])) {
    return;
  }

  thread_local std::uint64_t previous_frame = 0;
  thread_local std::uint64_t previous_hits = 0;
  thread_local float previous_position[3] = {};
  thread_local bool previous_valid = false;
  if (previous_frame != 0 && frame > previous_frame &&
      frame - previous_frame < 60) {
    return;
  }

  const std::uint64_t hits =
      g_native_triangle_hits.load(std::memory_order_relaxed);
  const float displacement =
      previous_valid
          ? std::sqrt(
                (local[0] - previous_position[0]) *
                    (local[0] - previous_position[0]) +
                (local[1] - previous_position[1]) *
                    (local[1] - previous_position[1]) +
                (local[2] - previous_position[2]) *
                    (local[2] - previous_position[2]))
          : 0.0f;
  const std::int32_t cell_x = static_cast<std::int32_t>(
      std::floor(local[0] / kOwnedCollisionCellSize));
  const std::int32_t cell_z = static_cast<std::int32_t>(
      std::floor(local[2] / kOwnedCollisionCellSize));
  const float within_x =
      local[0] - static_cast<float>(cell_x) * kOwnedCollisionCellSize;
  const float within_z =
      local[2] - static_cast<float>(cell_z) * kOwnedCollisionCellSize;
  const float seam_distance = std::min(
      {within_x, kOwnedCollisionCellSize - within_x,
       within_z, kOwnedCollisionCellSize - within_z});

  mechanics_sandbox::map::GroundHit ground;
  // Start just above the board, not at the top of the whole diagnostic
  // column. QueryGround returns the first downward hit; a 64 m start selected
  // ceilings and bridge undersides before the actual support beneath the
  // skater, which made underpass telemetry look like floor penetration.
  const bool ground_hit =
      mechanics_sandbox::map::QueryGround(local, 0.75f, 192.0f, ground);
  const float ground_delta =
      ground_hit ? local[1] - ground.point[1] : 0.0f;
  REXLOG_INFO(
      "native-collision-telemetry: frame={} local=({:.3f},{:.3f},{:.3f}) "
      "move={:.3f} package_support={} support_y={:.3f} "
      "support_delta={:.3f} "
      "normal_y={:.3f} legacy_cell=({}, {}) seam_distance={:.3f} "
      "native_hits_delta={} last_hit_mesh=0x{:08X} collection_count={} "
      "retail_suppressed={} retail_reconciled={}",
      frame, local[0], local[1], local[2], displacement,
      ground_hit ? 1 : 0, ground_hit ? ground.point[1] : 0.0f,
      ground_delta, ground_hit ? ground.normal[1] : 0.0f,
      cell_x, cell_z, seam_distance, hits - previous_hits,
      g_native_last_hit_mesh.load(std::memory_order_relaxed),
      g_live_collection_count.load(std::memory_order_acquire),
      g_suppressed_retail_volumes.load(std::memory_order_relaxed),
      g_reintroduced_retail_removed.load(std::memory_order_relaxed));

  previous_frame = frame;
  previous_hits = hits;
  std::copy(std::begin(local), std::end(local), previous_position);
  previous_valid = true;
}

void EnsureInstalled(PPCContext& ctx,
                     std::uint8_t* base,
                     std::uint32_t skateboard,
                     const float map_origin[3]) noexcept {
  if (!Enabled() || !base || map_origin == nullptr ||
      !std::isfinite(map_origin[0]) || !std::isfinite(map_origin[2])) {
    return;
  }

  ObserveLiveCollection(base);
  const State observed_state = g_state.load(std::memory_order_acquire);
  if (IsTerminal(observed_state)) {
    const bool replace_retail = REXCVAR_GET(
        skate3_mechanics_sandbox_native_collision_replace_retail);
    if (observed_state == State::InstalledAdditive && replace_retail) {
      std::scoped_lock lock(g_install_mutex);
      if (g_state.load(std::memory_order_acquire) ==
          State::InstalledAdditive) {
        const bool removed = RemoveOriginalVolumes(
            ctx, base, g_collection.load(std::memory_order_acquire),
            g_volume_address.load(std::memory_order_acquire));
        g_state.store(removed ? State::InstalledExclusive
                              : State::ReplacementFailed,
                      std::memory_order_release);
      }
    } else if (observed_state == State::InstalledExclusive &&
               replace_retail) {
      const std::uint32_t collection =
          g_collection.load(std::memory_order_acquire);
      if (ExclusiveCollectionDrifted(base, collection)) {
        std::scoped_lock lock(g_install_mutex);
        if (g_state.load(std::memory_order_acquire) ==
                State::InstalledExclusive &&
            ExclusiveCollectionDrifted(base, collection)) {
          if (!ReconcileExclusiveCollection(
                  ctx, base, collection)) {
            const std::uint64_t failures =
                g_exclusive_reconcile_failures.fetch_add(
                    1, std::memory_order_relaxed) +
                1;
            if (failures <= 8 || (failures & 255u) == 0) {
              REXLOG_ERROR(
                  "native-collision: exclusive collection reconciliation "
                  "failed (attempt={})",
                  failures);
            }
          } else {
            ObserveLiveCollection(base);
          }
        }
      }
    }
    return;
  }

  std::scoped_lock lock(g_install_mutex);
  if (IsTerminal(g_state.load(std::memory_order_acquire))) {
    return;
  }

  g_install_attempts.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t collection =
      g_collection.load(std::memory_order_acquire);
  if (!IsGuestDataAddress(collection)) {
    g_state.store(State::WaitingForCollection, std::memory_order_release);
    return;
  }

  // The collection is double buffered. Retail AddVolume is normally called
  // during a streamer publish phase; this adapter runs after player
  // ownership is known, so explicitly drain outstanding box/line/trajectory
  // batches before updating and publishing both buffers.
  WaitForCollectionJobs(ctx, base, collection);

  const std::uint32_t capacity = LoadU32(base, collection + 8);
  const std::uint32_t count = LoadU32(base, collection + 20);
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  const std::uint32_t write_entries = LoadU32(base, collection + 32);
  if (!IsGuestDataAddress(collection) ||
      !IsGuestDataAddress(read_entries) ||
      !IsGuestDataAddress(write_entries)) {
    g_state.store(State::WaitingForCollection, std::memory_order_release);
    return;
  }
  if (capacity == 0 || capacity > kMaximumReasonableCollectionCapacity ||
      count >= capacity) {
    g_state.store(State::CollectionFull, std::memory_order_release);
    return;
  }

  g_state.store(State::Compiling, std::memory_order_release);
  const std::uint32_t auxiliary =
      REX_KERNEL_MEMORY()->SystemHeapAlloc(kAuxiliaryAllocationSize, 16);
  if (!auxiliary) {
    g_state.store(State::AllocationFailed, std::memory_order_release);
    return;
  }
  std::memset(base + auxiliary, 0, kAuxiliaryAllocationSize);

  const std::uint32_t ground_result = auxiliary + kGroundResultOffset;
  float ground[3] = {};
  if (!CalculateNativeGroundPoint(ctx, base, skateboard, ground_result,
                                  ground)) {
    REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
    g_state.store(State::WaitingForPlacement, std::memory_order_release);
    return;
  }
  g_ground_x_bits.store(std::bit_cast<std::uint32_t>(ground[0]),
                        std::memory_order_release);
  g_ground_y_bits.store(std::bit_cast<std::uint32_t>(ground[1]),
                        std::memory_order_release);
  g_ground_z_bits.store(std::bit_cast<std::uint32_t>(ground[2]),
                        std::memory_order_release);

  // Static-world broadphase bounds are world-space in Skate's collection.
  // Compile authored local coordinates into world coordinates and register
  // the resulting static mesh with identity. The renderer retains the same
  // translation for its authored local-space draw data.
  const skate::world::SpawnPoint& spawn =
      mechanics_sandbox::map::ActiveDefinition().spawn;
  const float translation[3] = {
      map_origin[0] - spawn.position.x,
      ground[1] - spawn.position.y,
      map_origin[2] - spawn.position.z};
  OwnedCollisionBuildSet builds;
  try {
    builds = CompileOwnedMapChunks(translation);
  } catch (const std::exception& error) {
    REXLOG_ERROR(
        "native-collision: spatial build threw an exception: {}",
        error.what());
    REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
    g_state.store(State::BuildFailed, std::memory_order_release);
    return;
  } catch (...) {
    REXLOG_ERROR(
        "native-collision: spatial build threw an unknown exception");
    REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
    g_state.store(State::BuildFailed, std::memory_order_release);
    return;
  }
  if (builds.chunks.empty()) {
    REXLOG_ERROR(
        "native-collision: spatial build failed: {}",
        builds.error.empty() ? "no chunks produced" : builds.error);
    REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
    g_state.store(State::BuildFailed, std::memory_order_release);
    return;
  }
  if (builds.chunks.size() > kMaximumOwnedStaticChunks ||
      count + builds.chunks.size() > capacity) {
    REXLOG_ERROR(
        "native-collision: {} owned chunks do not fit collection "
        "(count={} capacity={})",
        builds.chunks.size(), count, capacity);
    REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
    g_state.store(State::CollectionFull, std::memory_order_release);
    return;
  }

  struct GuestChunk {
    std::uint32_t auxiliary = 0;
    std::uint32_t volume = 0;
    std::uint32_t resource = 0;
    std::uint32_t matrix = 0;
    std::uint32_t mesh = 0;
  };
  std::vector<GuestChunk> guest_chunks(builds.chunks.size());
  guest_chunks.front().auxiliary = auxiliary;
  auto free_unregistered_chunks = [&] {
    for (const GuestChunk& chunk : guest_chunks) {
      if (chunk.mesh != 0) {
        REX_KERNEL_MEMORY()->SystemHeapFree(chunk.mesh);
      }
      if (chunk.auxiliary != 0) {
        REX_KERNEL_MEMORY()->SystemHeapFree(chunk.auxiliary);
      }
    }
  };

  bool initialize_failed = false;
  for (std::size_t index = 0; index < builds.chunks.size(); ++index) {
    GuestChunk& guest = guest_chunks[index];
    skate::world::RwCollisionBuildResult& build = builds.chunks[index];
    if (index != 0) {
      guest.auxiliary =
          REX_KERNEL_MEMORY()->SystemHeapAlloc(
              kAuxiliaryAllocationSize, 16);
      if (guest.auxiliary == 0) {
        free_unregistered_chunks();
        g_state.store(State::AllocationFailed,
                      std::memory_order_release);
        return;
      }
      std::memset(base + guest.auxiliary, 0,
                  kAuxiliaryAllocationSize);
    }
    guest.volume = guest.auxiliary;
    guest.resource = guest.auxiliary + kResourceOffset;
    guest.matrix = guest.auxiliary + kMatrixOffset;
    guest.mesh = REX_KERNEL_MEMORY()->SystemHeapAlloc(
        static_cast<std::uint32_t>(build.mesh.bytes.size()), 16);
    if (guest.mesh == 0) {
      free_unregistered_chunks();
      g_state.store(State::AllocationFailed,
                    std::memory_order_release);
      return;
    }

    // Match RenderWare's asset-loader fixup contract. Native code
    // dereferences the top-level KD/tree pointers directly, but adds the
    // mesh base to each cluster-table element.
    if (!skate::world::FixupRwCollisionMeshForGuest(
            build.mesh.bytes, guest.mesh)) {
      REXLOG_ERROR(
          "native-collision: chunk {} pointer fixup failed", index);
      free_unregistered_chunks();
      g_state.store(State::BuildFailed, std::memory_order_release);
      return;
    }
    std::memcpy(base + guest.mesh, build.mesh.bytes.data(),
                build.mesh.bytes.size());
    StoreU32(base, guest.resource, guest.volume);

    PPCContext initialize = ctx;
    initialize.r3.u64 = guest.resource;
    initialize.r4.u64 = guest.mesh;
    sub_82AD7740(initialize, base);
    if (LoadU32(base, guest.volume + 68) != guest.mesh ||
        LoadU32(base, guest.volume + 92) != 1) {
      REXLOG_ERROR(
          "native-collision: chunk {} aggregate initialization failed",
          index);
      initialize_failed = true;
      break;
    }
  }
  if (initialize_failed) {
    free_unregistered_chunks();
    g_state.store(State::InitializeFailed,
                  std::memory_order_release);
    return;
  }

  const float identity_translation[3] = {0.0f, 0.0f, 0.0f};
  for (const GuestChunk& guest : guest_chunks) {
    WriteMapTransform(base, guest.matrix, identity_translation);
  }
  g_world_origin_x_bits.store(
      std::bit_cast<std::uint32_t>(translation[0]),
      std::memory_order_relaxed);
  g_world_origin_y_bits.store(
      std::bit_cast<std::uint32_t>(translation[1]),
      std::memory_order_relaxed);
  g_world_origin_z_bits.store(
      std::bit_cast<std::uint32_t>(translation[2]),
      std::memory_order_relaxed);

  g_original_volumes.clear();
  g_original_volumes.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t volume_address =
        LoadU32(base, read_entries + index * kCollectionEntrySize);
    if (IsGuestDataAddress(volume_address)) {
      g_original_volumes.push_back(volume_address);
    }
  }

  g_collection_count_before.store(count, std::memory_order_release);
  std::uint32_t current_count = count;
  for (std::size_t index = 0; index < guest_chunks.size(); ++index) {
    const GuestChunk& guest = guest_chunks[index];
    PPCContext add = ctx;
    add.r3.u64 = collection;
    add.r4.u64 = guest.volume;
    add.r5.u64 = guest.matrix;
    add.r6.u64 = 0;
    add.r7.u64 = 0;
    sub_82775F58(add, base);

    const std::uint32_t count_after =
        LoadU32(base, collection + 20);
    const std::uint32_t write_registered_volume =
        LoadU32(base, write_entries +
                          current_count * kCollectionEntrySize);
    const std::uint32_t write_registered_mesh =
        LoadU32(base, write_entries +
                          current_count * kCollectionEntrySize + 4);
    if (count_after == current_count + 1 &&
        write_registered_volume == guest.volume &&
        write_registered_mesh == guest.mesh) {
      PublishWriteEntries(
          base, read_entries, write_entries, count_after);
    }
    const std::uint32_t read_registered_volume =
        LoadU32(base, read_entries +
                          current_count * kCollectionEntrySize);
    const std::uint32_t read_registered_mesh =
        LoadU32(base, read_entries +
                          current_count * kCollectionEntrySize + 4);
    if (count_after != current_count + 1 ||
        write_registered_volume != guest.volume ||
        write_registered_mesh != guest.mesh ||
        read_registered_volume != guest.volume ||
        read_registered_mesh != guest.mesh) {
      // One or more objects may now be referenced by the native
      // collection, so all allocations intentionally remain alive.
      REXLOG_ERROR(
          "native-collision: chunk {} registration failed "
          "(before={} after={})",
          index, current_count, count_after);
      g_state.store(State::RegistrationFailed,
                    std::memory_order_release);
      return;
    }
    current_count = count_after;
  }
  g_collection_count_after.store(current_count,
                                 std::memory_order_release);

  std::uint64_t total_bytes = 0;
  std::uint64_t total_triangles = 0;
  std::uint64_t total_vertices = 0;
  for (std::size_t index = 0; index < guest_chunks.size(); ++index) {
    g_static_mesh_addresses[index].store(
        guest_chunks[index].mesh, std::memory_order_relaxed);
    g_static_volume_addresses[index].store(
        guest_chunks[index].volume, std::memory_order_relaxed);
    total_bytes += builds.chunks[index].mesh.bytes.size();
    total_triangles += builds.chunks[index].mesh.triangle_count;
    total_vertices += builds.chunks[index].mesh.vertex_count;
  }
  g_mesh_address.store(
      guest_chunks.front().mesh, std::memory_order_release);
  g_volume_address.store(
      guest_chunks.front().volume, std::memory_order_release);
  g_mesh_bytes.store(
      static_cast<std::uint32_t>(total_bytes),
      std::memory_order_release);
  g_mesh_triangles.store(
      static_cast<std::uint32_t>(total_triangles),
      std::memory_order_release);
  g_mesh_vertices.store(
      static_cast<std::uint32_t>(total_vertices),
      std::memory_order_release);
  g_static_mesh_count.store(
      static_cast<std::uint32_t>(guest_chunks.size()),
      std::memory_order_release);
  REXLOG_INFO(
      "native-collision: installed {} spatial chunks "
      "(triangles={} vertices={} bytes={})",
      guest_chunks.size(), total_triangles, total_vertices, total_bytes);
  g_world_origin_valid.store(true, std::memory_order_release);
  g_state.store(State::InstalledAdditive, std::memory_order_release);

  if (REXCVAR_GET(
          skate3_mechanics_sandbox_native_collision_replace_retail)) {
    const bool removed =
        RemoveOriginalVolumes(
            ctx, base, collection, guest_chunks.front().volume);
    g_state.store(removed ? State::InstalledExclusive
                          : State::ReplacementFailed,
                  std::memory_order_release);
  }
}

void UpdateKinematicObjects(PPCContext& ctx,
                            std::uint8_t* base) noexcept {
  const skate::world::MapDefinition& definition =
      mechanics_sandbox::map::ActiveDefinition();
  if (!Enabled() || base == nullptr ||
      definition.kinematic_boxes.empty()) {
    g_kinematic_state.store(KinematicState::Disabled,
                            std::memory_order_release);
    return;
  }

  const State static_state = g_state.load(std::memory_order_acquire);
  const bool static_world_ready =
      static_state == State::InstalledAdditive ||
      static_state == State::InstalledExclusive ||
      static_state == State::ReplacementFailed;
  if (!static_world_ready) {
    g_kinematic_state.store(KinematicState::WaitingForStaticWorld,
                            std::memory_order_release);
    return;
  }

  std::scoped_lock lock(g_install_mutex);
  KinematicState state =
      g_kinematic_state.load(std::memory_order_acquire);
  if (state != KinematicState::Installed &&
      state != KinematicState::Disabled &&
      state != KinematicState::WaitingForStaticWorld) {
    return;
  }

  const skate::world::KinematicBox& object =
      definition.kinematic_boxes.front();
  const std::uint32_t collection =
      g_collection.load(std::memory_order_acquire);
  float map_origin[3] = {};
  if (!IsGuestDataAddress(collection) ||
      !MapWorldOrigin(map_origin)) {
    g_kinematic_state.store(KinematicState::WaitingForStaticWorld,
                            std::memory_order_release);
    return;
  }

  const std::uint64_t frame =
      input_history_watch::CurrentFrameSequence();
  if (state != KinematicState::Installed) {
    g_kinematic_state.store(KinematicState::Compiling,
                            std::memory_order_release);
    WaitForCollectionJobs(ctx, base, collection);

    const std::uint32_t capacity = LoadU32(base, collection + 8);
    const std::uint32_t count = LoadU32(base, collection + 20);
    const std::uint32_t read_entries =
        LoadU32(base, collection + 16);
    const std::uint32_t write_entries =
        LoadU32(base, collection + 32);
    if (!IsGuestDataAddress(read_entries) ||
        !IsGuestDataAddress(write_entries)) {
      g_kinematic_state.store(
          KinematicState::WaitingForStaticWorld,
          std::memory_order_release);
      return;
    }
    if (capacity == 0 ||
        capacity > kMaximumReasonableCollectionCapacity ||
        count >= capacity) {
      g_kinematic_state.store(KinematicState::CollectionFull,
                              std::memory_order_release);
      return;
    }

    skate::world::RwCollisionBuildResult build;
    try {
      build = CompileKinematicBox(object);
    } catch (...) {
      g_kinematic_state.store(KinematicState::BuildFailed,
                              std::memory_order_release);
      return;
    }
    if (!build.ok || build.mesh.bytes.empty()) {
      g_kinematic_state.store(KinematicState::BuildFailed,
                              std::memory_order_release);
      return;
    }

    const std::uint32_t auxiliary =
        REX_KERNEL_MEMORY()->SystemHeapAlloc(
            kAuxiliaryAllocationSize, 16);
    const std::uint32_t mesh =
        REX_KERNEL_MEMORY()->SystemHeapAlloc(
            static_cast<std::uint32_t>(build.mesh.bytes.size()), 16);
    if (!auxiliary || !mesh) {
      if (mesh) {
        REX_KERNEL_MEMORY()->SystemHeapFree(mesh);
      }
      if (auxiliary) {
        REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
      }
      g_kinematic_state.store(KinematicState::AllocationFailed,
                              std::memory_order_release);
      return;
    }
    std::memset(base + auxiliary, 0, kAuxiliaryAllocationSize);
    if (!skate::world::FixupRwCollisionMeshForGuest(
            build.mesh.bytes, mesh)) {
      REX_KERNEL_MEMORY()->SystemHeapFree(mesh);
      REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
      g_kinematic_state.store(KinematicState::BuildFailed,
                              std::memory_order_release);
      return;
    }
    std::memcpy(base + mesh, build.mesh.bytes.data(),
                build.mesh.bytes.size());

    const std::uint32_t volume = auxiliary;
    const std::uint32_t resource = auxiliary + kResourceOffset;
    const std::uint32_t matrix = auxiliary + kMatrixOffset;
    StoreU32(base, resource, volume);
    PPCContext initialize = ctx;
    initialize.r3.u64 = resource;
    initialize.r4.u64 = mesh;
    sub_82AD7740(initialize, base);
    if (LoadU32(base, volume + 68) != mesh ||
        LoadU32(base, volume + 92) != 1) {
      REX_KERNEL_MEMORY()->SystemHeapFree(mesh);
      REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
      g_kinematic_state.store(KinematicState::InitializeFailed,
                              std::memory_order_release);
      return;
    }

    const skate::world::KinematicPose pose =
        skate::world::EvaluateKinematicBox(object, 0.0f);
    const float translation[3] = {
        map_origin[0] + pose.position.x,
        map_origin[1] + pose.position.y,
        map_origin[2] + pose.position.z,
    };
    WriteMapTransform(base, matrix, translation);

    PPCContext add = ctx;
    add.r3.u64 = collection;
    add.r4.u64 = volume;
    add.r5.u64 = matrix;
    add.r6.u64 = 0;
    add.r7.u64 = 0;
    sub_82775F58(add, base);

    const std::uint32_t count_after =
        LoadU32(base, collection + 20);
    const std::uint32_t write_entry =
        write_entries + count * kCollectionEntrySize;
    if (count_after == count + 1 &&
        LoadU32(base, write_entry) == volume &&
        LoadU32(base, write_entry + 4) == mesh) {
      WriteEntryLocalBounds(base, write_entry, object);
      PublishWriteEntries(base, read_entries, write_entries,
                          count_after);
      ObserveKinematicEntry(base, count, count,
                            read_entries + count * kCollectionEntrySize);
    }
    const std::uint32_t read_entry =
        read_entries + count * kCollectionEntrySize;
    if (count_after != count + 1 ||
        LoadU32(base, write_entry) != volume ||
        LoadU32(base, write_entry + 4) != mesh ||
        LoadU32(base, read_entry) != volume ||
        LoadU32(base, read_entry + 4) != mesh) {
      // The native collection may own these allocations after AddVolume.
      g_kinematic_state.store(KinematicState::RegistrationFailed,
                              std::memory_order_release);
      return;
    }

    g_kinematic_auxiliary_address.store(
        auxiliary, std::memory_order_release);
    g_kinematic_matrix_address.store(
        matrix, std::memory_order_release);
    g_kinematic_volume_address.store(
        volume, std::memory_order_release);
    g_kinematic_mesh_address.store(mesh, std::memory_order_release);
    g_kinematic_mesh_bytes.store(
        static_cast<std::uint32_t>(build.mesh.bytes.size()),
        std::memory_order_release);
    g_kinematic_mesh_triangles.store(
        build.mesh.triangle_count, std::memory_order_release);
    g_kinematic_epoch_frame.store(frame, std::memory_order_release);
    PublishKinematicPose(pose, frame);
    g_kinematic_state.store(KinematicState::Installed,
                            std::memory_order_release);
    return;
  }

  const std::uint64_t previous_frame =
      g_kinematic_last_update_frame.load(std::memory_order_acquire);
  if (frame == 0 || frame == previous_frame) {
    return;
  }

  const std::uint64_t epoch =
      g_kinematic_epoch_frame.load(std::memory_order_acquire);
  const float elapsed_seconds =
      frame >= epoch
          ? static_cast<float>(frame - epoch) / 60.0f
          : 0.0f;
  const skate::world::KinematicPose pose =
      skate::world::EvaluateKinematicBox(object, elapsed_seconds);
  const float translation[3] = {
      map_origin[0] + pose.position.x,
      map_origin[1] + pose.position.y,
      map_origin[2] + pose.position.z,
  };

  WaitForCollectionJobs(ctx, base, collection);
  const std::uint32_t count = LoadU32(base, collection + 20);
  const std::uint32_t read_entries =
      LoadU32(base, collection + 16);
  const std::uint32_t write_entries =
      LoadU32(base, collection + 32);
  const std::uint32_t volume =
      g_kinematic_volume_address.load(std::memory_order_acquire);
  const std::uint32_t mesh =
      g_kinematic_mesh_address.load(std::memory_order_acquire);
  const std::uint32_t matrix =
      g_kinematic_matrix_address.load(std::memory_order_acquire);
  const std::uint32_t read_index = FindOwnedEntry(
      base, read_entries, count, volume, mesh);
  const std::uint32_t write_index = FindOwnedEntry(
      base, write_entries, count, volume, mesh);
  if (!IsGuestDataAddress(read_entries) ||
      !IsGuestDataAddress(write_entries) ||
      !IsGuestDataAddress(volume) ||
      !IsGuestDataAddress(mesh) ||
      !IsGuestDataAddress(matrix) ||
      read_index == UINT32_MAX || write_index == UINT32_MAX ||
      read_index != write_index) {
    g_kinematic_state.store(KinematicState::MissingFromCollection,
                            std::memory_order_release);
    return;
  }

  WriteMapTransform(base, matrix, translation);
  const std::uint32_t write_entry =
      write_entries + write_index * kCollectionEntrySize;
  PPCContext rebuild = ctx;
  rebuild.r3.u64 = write_entry;
  rebuild.r4.u64 = volume;
  rebuild.r5.u64 = mesh;
  rebuild.r6.u64 = matrix;
  rebuild.r7.u64 = 0;
  rebuild.r8.u64 = UINT32_MAX;
  rebuild.r9.u64 = 0;
  sub_8276CB18(rebuild, base);
  WriteEntryLocalBounds(base, write_entry, object);

  const std::uint32_t read_entry =
      read_entries + read_index * kCollectionEntrySize;
  if (read_entry != write_entry) {
    std::memcpy(base + read_entry, base + write_entry,
                kCollectionEntrySize);
  }
  ObserveKinematicEntry(base, read_index, write_index, read_entry);
  PublishKinematicPose(pose, frame);
  g_kinematic_updates.fetch_add(1, std::memory_order_relaxed);
}

void UpdateHingedDoors(PPCContext& ctx,
                       std::uint8_t* base) noexcept {
  const skate::world::MapDefinition& definition =
      mechanics_sandbox::map::ActiveDefinition();
  if (!Enabled() || base == nullptr || definition.hinged_doors.empty()) {
    g_door_count.store(0, std::memory_order_release);
    g_door_state.store(DoorState::Disabled, std::memory_order_release);
    return;
  }
  if (definition.hinged_doors.size() > kMaximumHingedDoors) {
    g_door_state.store(DoorState::BuildFailed, std::memory_order_release);
    return;
  }

  const State static_state = g_state.load(std::memory_order_acquire);
  const bool static_world_ready =
      static_state == State::InstalledAdditive ||
      static_state == State::InstalledExclusive ||
      static_state == State::ReplacementFailed;
  if (!static_world_ready) {
    g_door_state.store(DoorState::WaitingForStaticWorld,
                       std::memory_order_release);
    return;
  }

  std::scoped_lock lock(g_install_mutex);
  DoorState state = g_door_state.load(std::memory_order_acquire);
  if (state != DoorState::Installed &&
      state != DoorState::Disabled &&
      state != DoorState::WaitingForStaticWorld) {
    return;
  }
  const std::uint32_t collection =
      g_collection.load(std::memory_order_acquire);
  float map_origin_values[3] = {};
  if (!IsGuestDataAddress(collection) ||
      !MapWorldOrigin(map_origin_values)) {
    g_door_state.store(DoorState::WaitingForStaticWorld,
                       std::memory_order_release);
    return;
  }
  const skate::world::Vec3 map_origin{
      map_origin_values[0], map_origin_values[1], map_origin_values[2]};
  const std::uint64_t frame =
      input_history_watch::CurrentFrameSequence();

  if (state != DoorState::Installed) {
    g_door_state.store(DoorState::Compiling,
                       std::memory_order_release);
    WaitForCollectionJobs(ctx, base, collection);
    const std::uint32_t capacity = LoadU32(base, collection + 8);
    const std::uint32_t count = LoadU32(base, collection + 20);
    const std::uint32_t read_entries = LoadU32(base, collection + 16);
    const std::uint32_t write_entries = LoadU32(base, collection + 32);
    if (!IsGuestDataAddress(read_entries) ||
        !IsGuestDataAddress(write_entries)) {
      g_door_state.store(DoorState::WaitingForStaticWorld,
                         std::memory_order_release);
      return;
    }
    if (capacity == 0 ||
        capacity > kMaximumReasonableCollectionCapacity ||
        definition.hinged_doors.size() > capacity - count) {
      g_door_state.store(DoorState::CollectionFull,
                         std::memory_order_release);
      return;
    }

    for (std::size_t index = 0;
         index < definition.hinged_doors.size(); ++index) {
      const skate::world::HingedDoor& door =
          definition.hinged_doors[index];
      skate::world::RwCollisionBuildResult build;
      try {
        build = CompileHingedDoor(door, definition);
      } catch (...) {
        g_door_state.store(DoorState::BuildFailed,
                           std::memory_order_release);
        return;
      }
      if (!build.ok || build.mesh.bytes.empty()) {
        g_door_state.store(DoorState::BuildFailed,
                           std::memory_order_release);
        return;
      }
      const std::uint32_t auxiliary =
          REX_KERNEL_MEMORY()->SystemHeapAlloc(
              kAuxiliaryAllocationSize, 16);
      const std::uint32_t mesh =
          REX_KERNEL_MEMORY()->SystemHeapAlloc(
              static_cast<std::uint32_t>(build.mesh.bytes.size()), 16);
      if (!auxiliary || !mesh) {
        if (mesh) {
          REX_KERNEL_MEMORY()->SystemHeapFree(mesh);
        }
        if (auxiliary) {
          REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
        }
        g_door_state.store(DoorState::AllocationFailed,
                           std::memory_order_release);
        return;
      }
      std::memset(base + auxiliary, 0, kAuxiliaryAllocationSize);
      if (!skate::world::FixupRwCollisionMeshForGuest(
              build.mesh.bytes, mesh)) {
        REX_KERNEL_MEMORY()->SystemHeapFree(mesh);
        REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
        g_door_state.store(DoorState::BuildFailed,
                           std::memory_order_release);
        return;
      }
      std::memcpy(base + mesh, build.mesh.bytes.data(),
                  build.mesh.bytes.size());
      const std::uint32_t volume = auxiliary;
      const std::uint32_t resource = auxiliary + kResourceOffset;
      const std::uint32_t matrix = auxiliary + kMatrixOffset;
      StoreU32(base, resource, volume);
      PPCContext initialize = ctx;
      initialize.r3.u64 = resource;
      initialize.r4.u64 = mesh;
      sub_82AD7740(initialize, base);
      if (LoadU32(base, volume + 68) != mesh ||
          LoadU32(base, volume + 92) != 1) {
        REX_KERNEL_MEMORY()->SystemHeapFree(mesh);
        REX_KERNEL_MEMORY()->SystemHeapFree(auxiliary);
        g_door_state.store(DoorState::InitializeFailed,
                           std::memory_order_release);
        return;
      }

      const float angle = door.initial_angle_radians;
      const skate::world::Vec3 x_axis = RotateAroundAxis(
          door.closed_width_axis, door.hinge_axis, angle);
      const skate::world::Vec3 z_axis = RotateAroundAxis(
          door.closed_depth_axis, door.hinge_axis, angle);
      WriteBasisTransform(
          base, matrix, x_axis, door.hinge_axis, z_axis,
          map_origin + door.hinge_position);

      const std::uint32_t insertion_index =
          LoadU32(base, collection + 20);
      PPCContext add = ctx;
      add.r3.u64 = collection;
      add.r4.u64 = volume;
      add.r5.u64 = matrix;
      add.r6.u64 = 0;
      add.r7.u64 = 0;
      sub_82775F58(add, base);
      const std::uint32_t count_after =
          LoadU32(base, collection + 20);
      const std::uint32_t write_entry =
          write_entries + insertion_index * kCollectionEntrySize;
      if (count_after != insertion_index + 1 ||
          LoadU32(base, write_entry) != volume ||
          LoadU32(base, write_entry + 4) != mesh) {
        g_door_state.store(DoorState::RegistrationFailed,
                           std::memory_order_release);
        return;
      }
      WriteEntryLocalBounds(base, write_entry, door);
      g_door_auxiliary_addresses[index].store(
          auxiliary, std::memory_order_release);
      g_door_matrix_addresses[index].store(
          matrix, std::memory_order_release);
      g_door_volume_addresses[index].store(
          volume, std::memory_order_release);
      g_door_mesh_addresses[index].store(
          mesh, std::memory_order_release);
      g_door_angles[index] = angle;
      g_door_angular_velocities[index] = 0.0f;
      g_door_angle_bits[index].store(
          std::bit_cast<std::uint32_t>(angle),
          std::memory_order_release);
      g_door_velocity_bits[index].store(
          std::bit_cast<std::uint32_t>(0.0f),
          std::memory_order_release);
      g_door_pose_frames[index].store(frame,
                                      std::memory_order_release);
      g_door_consumed_triangle_hits[index] =
          g_door_native_triangle_hits[index].load(
              std::memory_order_relaxed);
    }

    const std::uint32_t count_after = LoadU32(base, collection + 20);
    PublishWriteEntries(base, read_entries, write_entries, count_after);
    for (std::size_t index = 0;
         index < definition.hinged_doors.size(); ++index) {
      const std::uint32_t volume =
          g_door_volume_addresses[index].load(std::memory_order_acquire);
      const std::uint32_t mesh =
          g_door_mesh_addresses[index].load(std::memory_order_acquire);
      if (FindOwnedEntry(base, read_entries, count_after,
                         volume, mesh) == UINT32_MAX) {
        g_door_state.store(DoorState::RegistrationFailed,
                           std::memory_order_release);
        return;
      }
    }
    g_door_count.store(
        static_cast<std::uint32_t>(definition.hinged_doors.size()),
        std::memory_order_release);
    g_door_last_update_frame = frame;
    g_door_previous_player_valid = false;
    g_door_state.store(DoorState::Installed,
                       std::memory_order_release);
    REXLOG_INFO(
        "native-collision: installed {} contact-driven hinged doors",
        definition.hinged_doors.size());
    return;
  }

  if (frame == 0 || frame == g_door_last_update_frame) {
    return;
  }
  const std::uint64_t frame_delta =
      frame > g_door_last_update_frame
          ? std::min<std::uint64_t>(frame - g_door_last_update_frame, 4)
          : 1;
  const float dt = static_cast<float>(frame_delta) / 60.0f;
  g_door_last_update_frame = frame;

  skate::world::Vec3 player_position{};
  skate::world::Vec3 player_velocity{};
  bool player_valid = false;
  float player_world[3] = {};
  if (trick_pipeline::CurrentLocalBoardPosition(player_world)) {
    player_position = {
        player_world[0] - map_origin.x,
        player_world[1] - map_origin.y,
        player_world[2] - map_origin.z,
    };
    if (g_door_previous_player_valid) {
      const skate::world::Vec3 previous{
          g_door_previous_player_position[0],
          g_door_previous_player_position[1],
          g_door_previous_player_position[2],
      };
      const skate::world::Vec3 displacement =
          player_position - previous;
      if (skate::world::LengthSquared(displacement) < 9.0f) {
        player_velocity = displacement / dt;
        const float speed = skate::world::Length(player_velocity);
        if (speed > 14.0f) {
          player_velocity = player_velocity * (14.0f / speed);
        }
        player_valid = true;
        g_door_player_samples.fetch_add(1, std::memory_order_relaxed);
      }
    }
    g_door_previous_player_position[0] = player_position.x;
    g_door_previous_player_position[1] = player_position.y;
    g_door_previous_player_position[2] = player_position.z;
    g_door_previous_player_valid = true;
  } else {
    g_door_previous_player_valid = false;
  }

  // Skate's character controller prevents the board/feet origin from ever
  // entering a 38 cm sphere around a solid door. Model the whole standing
  // player as a vertical capsule and accept the native door-mesh hit as the
  // authoritative confirmation when the controller stops on the boundary.
  constexpr float kPlayerRadius = 0.72f;
  constexpr float kPlayerBelowOrigin = 0.75f;
  constexpr float kPlayerAboveOrigin = 1.85f;
  constexpr float kNativeContactMargin = 0.58f;
  constexpr float kEffectivePlayerMass = 72.0f;
  for (std::size_t index = 0;
       index < definition.hinged_doors.size(); ++index) {
    const skate::world::HingedDoor& door =
        definition.hinged_doors[index];
    float angle = g_door_angles[index];
    float angular_velocity = g_door_angular_velocities[index];
    skate::world::Vec3 x_axis = RotateAroundAxis(
        door.closed_width_axis, door.hinge_axis, angle);
    skate::world::Vec3 z_axis = RotateAroundAxis(
        door.closed_depth_axis, door.hinge_axis, angle);
    const std::uint64_t native_hits =
        g_door_native_triangle_hits[index].load(
            std::memory_order_relaxed);
    const bool native_hit_since_last_update =
        native_hits != g_door_consumed_triangle_hits[index];
    g_door_consumed_triangle_hits[index] = native_hits;

    if (player_valid) {
      const skate::world::Vec3 delta =
          player_position - door.hinge_position;
      const float local_x = skate::world::Dot(delta, x_axis);
      const float local_y =
          skate::world::Dot(delta, door.hinge_axis);
      const float local_z = skate::world::Dot(delta, z_axis);
      const bool vertical_overlap =
          local_y + kPlayerAboveOrigin >= door.local_min.y &&
          local_y - kPlayerBelowOrigin <= door.local_max.y;
      if (vertical_overlap &&
          local_x >= door.local_min.x -
                         kPlayerRadius - kNativeContactMargin &&
          local_x <= door.local_max.x +
                         kPlayerRadius + kNativeContactMargin &&
          local_z >= door.local_min.z -
                         kPlayerRadius - kNativeContactMargin &&
          local_z <= door.local_max.z +
                         kPlayerRadius + kNativeContactMargin) {
        const float closest_x =
            std::clamp(local_x, door.local_min.x, door.local_max.x);
        const float closest_y =
            std::clamp(local_y, door.local_min.y, door.local_max.y);
        const float closest_z =
            std::clamp(local_z, door.local_min.z, door.local_max.z);
        const float separation_x = local_x - closest_x;
        const float separation_z = local_z - closest_z;
        const float distance = std::sqrt(
            separation_x * separation_x +
            separation_z * separation_z);
        const bool native_contact =
            native_hit_since_last_update &&
            distance <= kPlayerRadius + kNativeContactMargin;
        const bool capsule_overlap = distance <= kPlayerRadius;
        if (capsule_overlap || native_contact) {
          if (capsule_overlap) {
            g_door_capsule_overlaps.fetch_add(
                1, std::memory_order_relaxed);
          }
          if (native_contact) {
            g_door_native_confirmed_contacts.fetch_add(
                1, std::memory_order_relaxed);
          }
          float normal_x = 0.0f;
          float normal_z = 0.0f;
          if (distance > 1.0e-4f) {
            normal_x = separation_x / distance;
            normal_z = separation_z / distance;
          } else {
            const float to_min = std::abs(local_z - door.local_min.z);
            const float to_max = std::abs(door.local_max.z - local_z);
            normal_z = to_min < to_max ? -1.0f : 1.0f;
          }
          const skate::world::Vec3 normal =
              x_axis * normal_x + z_axis * normal_z;
          const float closing_speed = std::max(
              0.0f, -skate::world::Dot(player_velocity, normal));
          const float penetration =
              std::max(0.0f, kPlayerRadius - distance);
          // A native hit means Skate's controller has already cancelled
          // most of the measured motion. Preserve a small contact-speed
          // floor so the cancelled movement still transfers momentum.
          const float effective_closing_speed = std::max(
              closing_speed, native_contact ? 0.32f : 0.0f);
          if (effective_closing_speed > 0.015f ||
              penetration > 0.015f) {
            const float impulse =
                kEffectivePlayerMass * effective_closing_speed +
                penetration * 540.0f * dt;
            const skate::world::Vec3 force_on_door =
                normal * -impulse;
            const skate::world::Vec3 lever =
                x_axis * closest_x +
                door.hinge_axis * closest_y +
                z_axis * closest_z;
            const float torque = skate::world::Dot(
                skate::world::Cross(lever, force_on_door),
                door.hinge_axis);
            const float width = std::max(
                std::abs(door.local_min.x),
                std::abs(door.local_max.x));
            const float depth = std::max(
                std::abs(door.local_min.z),
                std::abs(door.local_max.z));
            const float inertia = std::max(
                0.1f,
                door.mass *
                    (width * width + depth * depth) / 3.0f);
            angular_velocity +=
                (torque / inertia) * door.contact_impulse_scale;
            g_door_contact_impulses.fetch_add(
                1, std::memory_order_relaxed);
          }
        }
      }
    }

    // A torsion spring models an ordinary self-closing hinge. It targets the
    // authored initial angle, while angular damping dissipates energy. This
    // remains a physical one-degree-of-freedom body: player contact can hold
    // it open or push it back against the closer.
    const float angle_from_rest =
        angle - door.initial_angle_radians;
    angular_velocity +=
        -door.return_spring_strength * angle_from_rest * dt;
    angular_velocity *= std::exp(-door.angular_damping * dt);
    angular_velocity = std::clamp(
        angular_velocity, -door.maximum_angular_speed,
        door.maximum_angular_speed);
    angle += angular_velocity * dt;
    if (angle < door.minimum_angle_radians) {
      angle = door.minimum_angle_radians;
      if (angular_velocity < 0.0f) {
        angular_velocity =
            -angular_velocity * std::min(door.restitution, 0.18f);
      }
      g_door_limit_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (angle > door.maximum_angle_radians) {
      angle = door.maximum_angle_radians;
      if (angular_velocity > 0.0f) {
        angular_velocity =
            -angular_velocity * std::min(door.restitution, 0.18f);
      }
      g_door_limit_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (std::abs(angle - door.initial_angle_radians) < 0.0015f &&
        std::abs(angular_velocity) < 0.006f) {
      angle = door.initial_angle_radians;
      angular_velocity = 0.0f;
    }
    g_door_angles[index] = angle;
    g_door_angular_velocities[index] = angular_velocity;
  }

  WaitForCollectionJobs(ctx, base, collection);
  const std::uint32_t count = LoadU32(base, collection + 20);
  const std::uint32_t read_entries = LoadU32(base, collection + 16);
  const std::uint32_t write_entries = LoadU32(base, collection + 32);
  if (!IsGuestDataAddress(read_entries) ||
      !IsGuestDataAddress(write_entries)) {
    g_door_state.store(DoorState::MissingFromCollection,
                       std::memory_order_release);
    return;
  }
  for (std::size_t index = 0;
       index < definition.hinged_doors.size(); ++index) {
    const skate::world::HingedDoor& door =
        definition.hinged_doors[index];
    const std::uint32_t volume =
        g_door_volume_addresses[index].load(std::memory_order_acquire);
    const std::uint32_t mesh =
        g_door_mesh_addresses[index].load(std::memory_order_acquire);
    const std::uint32_t matrix =
        g_door_matrix_addresses[index].load(std::memory_order_acquire);
    const std::uint32_t read_index =
        FindOwnedEntry(base, read_entries, count, volume, mesh);
    const std::uint32_t write_index =
        FindOwnedEntry(base, write_entries, count, volume, mesh);
    if (!IsGuestDataAddress(volume) || !IsGuestDataAddress(mesh) ||
        !IsGuestDataAddress(matrix) ||
        read_index == UINT32_MAX || write_index == UINT32_MAX ||
        read_index != write_index) {
      g_door_state.store(DoorState::MissingFromCollection,
                         std::memory_order_release);
      return;
    }
    const float angle = g_door_angles[index];
    const skate::world::Vec3 x_axis = RotateAroundAxis(
        door.closed_width_axis, door.hinge_axis, angle);
    const skate::world::Vec3 z_axis = RotateAroundAxis(
        door.closed_depth_axis, door.hinge_axis, angle);
    WriteBasisTransform(
        base, matrix, x_axis, door.hinge_axis, z_axis,
        map_origin + door.hinge_position);
    const std::uint32_t write_entry =
        write_entries + write_index * kCollectionEntrySize;
    PPCContext rebuild = ctx;
    rebuild.r3.u64 = write_entry;
    rebuild.r4.u64 = volume;
    rebuild.r5.u64 = mesh;
    rebuild.r6.u64 = matrix;
    rebuild.r7.u64 = 0;
    rebuild.r8.u64 = UINT32_MAX;
    rebuild.r9.u64 = 0;
    sub_8276CB18(rebuild, base);
    WriteEntryLocalBounds(base, write_entry, door);
    const std::uint32_t read_entry =
        read_entries + read_index * kCollectionEntrySize;
    if (read_entry != write_entry) {
      std::memcpy(base + read_entry, base + write_entry,
                  kCollectionEntrySize);
    }
    g_door_angle_bits[index].store(
        std::bit_cast<std::uint32_t>(angle),
        std::memory_order_release);
    g_door_velocity_bits[index].store(
        std::bit_cast<std::uint32_t>(
            g_door_angular_velocities[index]),
        std::memory_order_release);
    g_door_pose_frames[index].store(frame,
                                    std::memory_order_release);
  }
  g_door_updates.fetch_add(1, std::memory_order_relaxed);
}

void AppendTelemetry(std::ostream& out) {
  out << " sandbox_native_collision=" << (Enabled() ? 1 : 0)
      << " sandbox_native_collision_replace="
      << (REXCVAR_GET(
              skate3_mechanics_sandbox_native_collision_replace_retail)
              ? 1
              : 0)
      << " sandbox_native_collision_state="
      << StateName(g_state.load(std::memory_order_acquire))
      << " sandbox_native_collision_view="
      << g_world_streamer_view.load(std::memory_order_acquire)
      << " sandbox_native_collision_collection="
      << g_collection.load(std::memory_order_acquire)
      << " sandbox_native_collision_capacity="
      << g_collection_capacity.load(std::memory_order_acquire)
      << " sandbox_native_collision_read_entries="
      << g_collection_read_entries.load(std::memory_order_acquire)
      << " sandbox_native_collision_write_entries="
      << g_collection_write_entries.load(std::memory_order_acquire)
      << " sandbox_native_collision_count_before="
      << g_collection_count_before.load(std::memory_order_acquire)
      << " sandbox_native_collision_count_after="
      << g_collection_count_after.load(std::memory_order_acquire)
      << " sandbox_native_collision_mesh="
      << g_mesh_address.load(std::memory_order_acquire)
      << " sandbox_native_collision_volume="
      << g_volume_address.load(std::memory_order_acquire)
      << " sandbox_native_collision_bytes="
      << g_mesh_bytes.load(std::memory_order_acquire)
      << " sandbox_native_collision_triangles="
      << g_mesh_triangles.load(std::memory_order_acquire)
      << " sandbox_native_collision_vertices="
      << g_mesh_vertices.load(std::memory_order_acquire)
      << " sandbox_native_collision_chunks="
      << g_static_mesh_count.load(std::memory_order_acquire)
      << " sandbox_native_collision_removed_retail="
      << g_removed_retail_volumes.load(std::memory_order_acquire)
      << " sandbox_native_collision_reconciliations="
      << g_exclusive_reconciliations.load(std::memory_order_relaxed)
      << " sandbox_native_collision_reintroduced_removed="
      << g_reintroduced_retail_removed.load(std::memory_order_relaxed)
      << " sandbox_native_collision_owned_readded="
      << g_owned_static_readded.load(std::memory_order_relaxed)
      << " sandbox_native_collision_reconcile_failures="
      << g_exclusive_reconcile_failures.load(std::memory_order_relaxed)
      << " sandbox_native_collision_suppressed_retail_batches="
      << g_suppressed_retail_batches.load(std::memory_order_relaxed)
      << " sandbox_native_collision_suppressed_retail_volumes="
      << g_suppressed_retail_volumes.load(std::memory_order_relaxed)
      << " sandbox_native_collision_ground_y_bits="
      << g_ground_y_bits.load(std::memory_order_acquire)
      << " sandbox_native_collision_attempts="
      << g_install_attempts.load(std::memory_order_relaxed)
      << " sandbox_native_live_count="
      << g_live_collection_count.load(std::memory_order_acquire)
      << " sandbox_native_live_read_owned_index="
      << g_live_read_owned_index.load(std::memory_order_acquire)
      << " sandbox_native_live_write_owned_index="
      << g_live_write_owned_index.load(std::memory_order_acquire)
      << " sandbox_native_owned_bbox_min_x_bits="
      << g_live_owned_bbox_min_x_bits.load(std::memory_order_acquire)
      << " sandbox_native_owned_bbox_min_y_bits="
      << g_live_owned_bbox_min_y_bits.load(std::memory_order_acquire)
      << " sandbox_native_owned_bbox_min_z_bits="
      << g_live_owned_bbox_min_z_bits.load(std::memory_order_acquire)
      << " sandbox_native_owned_bbox_max_x_bits="
      << g_live_owned_bbox_max_x_bits.load(std::memory_order_acquire)
      << " sandbox_native_owned_bbox_max_y_bits="
      << g_live_owned_bbox_max_y_bits.load(std::memory_order_acquire)
      << " sandbox_native_owned_bbox_max_z_bits="
      << g_live_owned_bbox_max_z_bits.load(std::memory_order_acquire)
      << " sandbox_native_line_workers="
      << g_native_line_workers.load(std::memory_order_relaxed)
      << " sandbox_native_owned_line_workers="
      << g_native_owned_line_workers.load(std::memory_order_relaxed)
      << " sandbox_native_box_workers="
      << g_native_box_workers.load(std::memory_order_relaxed)
      << " sandbox_native_owned_box_workers="
      << g_native_owned_box_workers.load(std::memory_order_relaxed)
      << " sandbox_native_iterators="
      << g_native_iterators.load(std::memory_order_relaxed)
      << " sandbox_native_owned_iterators="
      << g_native_owned_iterators.load(std::memory_order_relaxed)
      << " sandbox_native_query_candidates="
      << g_native_query_candidates.load(std::memory_order_relaxed)
      << " sandbox_native_last_candidate_mesh="
      << g_native_last_candidate_mesh.load(std::memory_order_relaxed)
      << " sandbox_native_query_entries="
      << g_native_query_entries.load(std::memory_order_relaxed)
      << " sandbox_native_cluster_decodes="
      << g_native_cluster_decodes.load(std::memory_order_relaxed)
      << " sandbox_native_decoded_triangles="
      << g_native_decoded_triangles.load(std::memory_order_relaxed)
      << " sandbox_native_triangle_tests="
      << g_native_triangle_tests.load(std::memory_order_relaxed)
      << " sandbox_native_triangle_hits="
      << g_native_triangle_hits.load(std::memory_order_relaxed)
      << " sandbox_native_last_hit_mesh="
      << g_native_last_hit_mesh.load(std::memory_order_relaxed)
      << " sandbox_kinematic_count="
      << mechanics_sandbox::map::ActiveKinematicObjectCount()
      << " sandbox_kinematic_state="
      << KinematicStateName(
             g_kinematic_state.load(std::memory_order_acquire))
      << " sandbox_kinematic_mesh="
      << g_kinematic_mesh_address.load(std::memory_order_acquire)
      << " sandbox_kinematic_volume="
      << g_kinematic_volume_address.load(std::memory_order_acquire)
      << " sandbox_kinematic_bytes="
      << g_kinematic_mesh_bytes.load(std::memory_order_acquire)
      << " sandbox_kinematic_triangles="
      << g_kinematic_mesh_triangles.load(std::memory_order_acquire)
      << " sandbox_kinematic_epoch_frame="
      << g_kinematic_epoch_frame.load(std::memory_order_acquire)
      << " sandbox_kinematic_update_frame="
      << g_kinematic_last_update_frame.load(std::memory_order_acquire)
      << " sandbox_kinematic_updates="
      << g_kinematic_updates.load(std::memory_order_relaxed)
      << " sandbox_hinged_door_count="
      << g_door_count.load(std::memory_order_acquire)
      << " sandbox_hinged_door_state="
      << DoorStateName(g_door_state.load(std::memory_order_acquire))
      << " sandbox_hinged_door_updates="
      << g_door_updates.load(std::memory_order_relaxed)
      << " sandbox_hinged_door_contact_impulses="
      << g_door_contact_impulses.load(std::memory_order_relaxed)
      << " sandbox_hinged_door_capsule_overlaps="
      << g_door_capsule_overlaps.load(std::memory_order_relaxed)
      << " sandbox_hinged_door_native_confirmed_contacts="
      << g_door_native_confirmed_contacts.load(
             std::memory_order_relaxed)
      << " sandbox_hinged_door_player_samples="
      << g_door_player_samples.load(std::memory_order_relaxed)
      << " sandbox_hinged_door_limit_hits="
      << g_door_limit_hits.load(std::memory_order_relaxed)
      << " sandbox_hinged_door_triangle_hits="
      << g_door_triangle_hits.load(std::memory_order_relaxed)
      << " sandbox_kinematic_line_workers="
      << g_kinematic_line_workers.load(std::memory_order_relaxed)
      << " sandbox_kinematic_box_workers="
      << g_kinematic_box_workers.load(std::memory_order_relaxed)
      << " sandbox_kinematic_iterators="
      << g_kinematic_iterators.load(std::memory_order_relaxed)
      << " sandbox_kinematic_line_batches="
      << g_kinematic_line_batches.load(std::memory_order_relaxed)
      << " sandbox_kinematic_visible_line_batches="
      << g_kinematic_visible_line_batches.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_linear_line_batches="
      << g_kinematic_linear_line_batches.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_linear_box_batches="
      << g_kinematic_linear_box_batches.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_batch_entry_refreshes="
      << g_kinematic_batch_entry_refreshes.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_last_batch_entries="
      << g_kinematic_last_batch_entries.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_last_batch_count="
      << g_kinematic_last_batch_count.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_last_batch_accelerator="
      << g_kinematic_last_batch_accelerator.load(
             std::memory_order_relaxed)
      << " sandbox_kinematic_read_index="
      << g_kinematic_read_index.load(std::memory_order_acquire)
      << " sandbox_kinematic_write_index="
      << g_kinematic_write_index.load(std::memory_order_acquire)
      << " sandbox_kinematic_forward_x_bits="
      << g_kinematic_forward_x_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_forward_y_bits="
      << g_kinematic_forward_y_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_forward_z_bits="
      << g_kinematic_forward_z_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_inverse_x_bits="
      << g_kinematic_inverse_x_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_inverse_y_bits="
      << g_kinematic_inverse_y_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_inverse_z_bits="
      << g_kinematic_inverse_z_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_bbox_min_x_bits="
      << g_kinematic_bbox_min_x_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_bbox_min_y_bits="
      << g_kinematic_bbox_min_y_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_bbox_min_z_bits="
      << g_kinematic_bbox_min_z_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_bbox_max_x_bits="
      << g_kinematic_bbox_max_x_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_bbox_max_y_bits="
      << g_kinematic_bbox_max_y_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_bbox_max_z_bits="
      << g_kinematic_bbox_max_z_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_position_x_bits="
      << g_kinematic_position_x_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_position_y_bits="
      << g_kinematic_position_y_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_position_z_bits="
      << g_kinematic_position_z_bits.load(std::memory_order_acquire)
      << " sandbox_kinematic_query_entries="
      << g_kinematic_query_entries.load(std::memory_order_relaxed)
      << " sandbox_kinematic_triangle_tests="
      << g_kinematic_triangle_tests.load(std::memory_order_relaxed)
      << " sandbox_kinematic_triangle_hits="
      << g_kinematic_triangle_hits.load(std::memory_order_relaxed);
}

}  // namespace skate3::native_collision
