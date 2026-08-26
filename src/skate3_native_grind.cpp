#include "skate3_native_grind.h"

#include "generated/skate3_init.h"
#include "skate/world/grind_spline.h"
#include "skate/world/map_editor.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_map_editor.h"
#include "skate3_native_collision.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_native_grinds, true, "Skate 3",
    "Compile project-owned GrindRail paths to native Pegasus tSplineData "
    "and register them through Skate 3's authoritative GrindData runtime. "
    "Only active with the owned native-collision sandbox.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace skate3::native_grind {
namespace {

enum class State : std::uint8_t {
  Disabled = 0,
  WaitingForRuntime,
  WaitingForMap,
  Compiling,
  AllocationFailed,
  BuildFailed,
  RegistrationFailed,
  Installed,
};

std::atomic<State> g_state{State::Disabled};
std::atomic<std::uint32_t> g_grind_data{0};
std::atomic<std::uint32_t> g_memory_group{0};
std::atomic<std::uint32_t> g_spline_data_address{0};
std::atomic<std::uint32_t> g_spline_data_bytes{0};
std::atomic<std::uint32_t> g_rail_count{0};
std::atomic<std::uint32_t> g_segment_count{0};
std::atomic<std::uint32_t> g_last_add_vector_count{0};
std::atomic<std::uint64_t> g_runtime_load_calls{0};
std::atomic<std::uint64_t> g_runtime_add_calls{0};
std::atomic<std::uint64_t> g_owned_load_calls{0};
std::atomic<std::uint64_t> g_owned_add_calls{0};
std::atomic<std::uint64_t> g_install_attempts{0};
std::atomic<std::uint64_t> g_transform_updates{0};
std::atomic<std::uint64_t> g_transform_update_failures{0};
std::atomic<std::uint64_t> g_runtime_refreshes{0};
std::atomic<std::uint64_t> g_runtime_retire_failures{0};
std::atomic<std::uint64_t> g_applied_commit_serial{0};
std::atomic<std::uint32_t> g_runtime_object{0};
std::mutex g_install_mutex;
thread_local bool g_installing_owned = false;

// TU3 GrindData's process-wide vector owner. sub_82C1ED60 appends one
// 96-byte runtime object pointer here for each registered tSplineData.
constexpr std::uint32_t kRuntimeVectorOwnerSlot = 0x830846A4u;

bool IsGuestDataAddress(std::uint32_t address) {
  return address >= 0x00010000u && address < 0x80000000u;
}

std::uint32_t LoadU32(std::uint8_t* base, std::uint32_t address) {
  if (!base || !IsGuestDataAddress(address)) {
    return 0;
  }
  return REX_LOAD_U32(address);
}

std::uint16_t LoadU16(std::uint8_t* base, std::uint32_t address) {
  if (!base || !IsGuestDataAddress(address)) {
    return 0;
  }
  return REX_LOAD_U16(address);
}

struct RuntimeVector {
  std::uint32_t owner = 0;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::uint32_t count = 0;
};

bool ReadRuntimeVector(std::uint8_t* base,
                       RuntimeVector& result) {
  if (!base) {
    return false;
  }
  const std::uint32_t owner =
      REX_LOAD_U32(kRuntimeVectorOwnerSlot);
  if (!IsGuestDataAddress(owner)) {
    return false;
  }
  const std::uint32_t begin = LoadU32(base, owner);
  const std::uint32_t end = LoadU32(base, owner + 4);
  const std::uint32_t capacity = LoadU32(base, owner + 8);
  if (!IsGuestDataAddress(begin) || end < begin ||
      capacity < end || (end - begin) % 4 != 0 ||
      end - begin > 16384u * 4u) {
    return false;
  }
  result = {
      .owner = owner,
      .begin = begin,
      .end = end,
      .count = (end - begin) / 4,
  };
  return true;
}

bool RegisterOwnedSpline(PPCContext& ctx, std::uint8_t* base,
                         std::uint32_t grind_data,
                         std::uint32_t spline_data,
                         std::uint32_t expected_rails,
                         std::uint32_t& runtime_object) {
  RuntimeVector before;
  if (!ReadRuntimeVector(base, before)) {
    return false;
  }
  const std::uint64_t adds_before =
      g_owned_add_calls.load(std::memory_order_acquire);
  PPCContext add = ctx;
  add.r3.u64 = grind_data;
  add.r4.u64 = spline_data;
  add.r5.u64 = g_memory_group.load(std::memory_order_acquire);
  g_installing_owned = true;
  sub_82C1EEF0(add, base);
  g_installing_owned = false;

  RuntimeVector after;
  if (!ReadRuntimeVector(base, after)) {
    return false;
  }
  if (after.count == before.count + 1) {
    const std::uint32_t appended =
        LoadU32(base, after.end - 4);
    if (IsGuestDataAddress(appended)) {
      runtime_object = appended;
    }
  }
  if (after.count != before.count + 1 ||
      g_owned_add_calls.load(std::memory_order_acquire) !=
          adds_before + 1 ||
      g_last_add_vector_count.load(std::memory_order_acquire) !=
          expected_rails) {
    return false;
  }
  return IsGuestDataAddress(runtime_object);
}

bool RetireRuntimeObject(PPCContext& ctx, std::uint8_t* base,
                         std::uint32_t runtime_object) {
  RuntimeVector vector;
  if (!IsGuestDataAddress(runtime_object) ||
      !ReadRuntimeVector(base, vector) || vector.count == 0) {
    return false;
  }
  std::uint32_t entry = 0;
  for (std::uint32_t cursor = vector.begin;
       cursor < vector.end; cursor += 4) {
    if (LoadU32(base, cursor) == runtime_object) {
      entry = cursor;
      break;
    }
  }
  if (!entry) {
    return false;
  }

  // Match the retail unload path at 0x8285B448: destroy the GrindData
  // runtime object, move the vector's last pointer into its slot, then shrink
  // the vector. This removes old broadphase/spline caches instead of leaving
  // duplicate grind rails behind.
  PPCContext destroy = ctx;
  destroy.r3.u64 = runtime_object;
  sub_82C1EE28(destroy, base);
  const std::uint32_t last = LoadU32(base, vector.end - 4);
  REX_STORE_U32(entry, last);
  REX_STORE_U32(vector.owner + 4, vector.end - 4);
  return true;
}

const char* StateName(State state) {
  switch (state) {
    case State::Disabled:
      return "disabled";
    case State::WaitingForRuntime:
      return "waiting_runtime";
    case State::WaitingForMap:
      return "waiting_map";
    case State::Compiling:
      return "compiling";
    case State::AllocationFailed:
      return "allocation_failed";
    case State::BuildFailed:
      return "build_failed";
    case State::RegistrationFailed:
      return "registration_failed";
    case State::Installed:
      return "installed";
  }
  return "unknown";
}

skate::world::MapDefinition RuntimeGrindMap() {
  const skate::world::MapDefinition& source =
      mechanics_sandbox::map::ActiveDefinition();
  skate::world::MapDefinition runtime;
  std::vector<skate::world::EditorObjectTransform> transforms(
      source.editable_objects.size());
  for (std::size_t object_index = 0;
       object_index < source.editable_objects.size(); ++object_index) {
    const skate::world::MapObject& object =
        source.editable_objects[object_index];
    transforms[object_index].translation = object.origin;
    float translation[3] = {};
    float basis[9] = {};
    if (!map_editor::ObjectTransform(
            object_index, translation, basis, nullptr)) {
      continue;
    }
    transforms[object_index] = {
        .translation =
            {translation[0], translation[1], translation[2]},
        .x_axis = {basis[0], basis[1], basis[2]},
        .y_axis = {basis[3], basis[4], basis[5]},
        .z_axis = {basis[6], basis[7], basis[8]},
    };
  }
  runtime.grind_rails =
      skate::world::TransformEditorGrindRails(source, transforms);
  return runtime;
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(skate3_mechanics_sandbox_native_grinds) &&
         native_collision::Enabled();
}

void ObserveSplineDataLoad(const PPCContext& ctx,
                           std::uint8_t* base) noexcept {
  const std::uint32_t grind_data = ctx.r3.u32;
  const std::uint32_t spline_data = ctx.r4.u32;
  if (!base || !IsGuestDataAddress(grind_data) ||
      !IsGuestDataAddress(spline_data) ||
      LoadU16(base, spline_data + 2) == 0 ||
      !IsGuestDataAddress(LoadU32(base, spline_data + 8))) {
    return;
  }

  if (g_installing_owned) {
    g_owned_load_calls.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  g_grind_data.store(grind_data, std::memory_order_release);
  g_memory_group.store(ctx.r5.u32, std::memory_order_release);
  g_runtime_load_calls.fetch_add(1, std::memory_order_relaxed);
  if (Enabled() &&
      g_state.load(std::memory_order_acquire) == State::Disabled) {
    g_state.store(State::WaitingForMap, std::memory_order_release);
  }
}

void ObserveGrindDataAdd(const PPCContext& ctx,
                         std::uint8_t* base) noexcept {
  const std::uint32_t vector = ctx.r4.u32;
  std::uint32_t count = 0;
  if (base && IsGuestDataAddress(vector)) {
    const std::uint32_t begin = LoadU32(base, vector);
    const std::uint32_t end = LoadU32(base, vector + 4);
    if (IsGuestDataAddress(begin) && end >= begin &&
        (end - begin) % 4 == 0) {
      count = (end - begin) / 4;
    }
  }
  g_last_add_vector_count.store(count, std::memory_order_release);
  if (g_installing_owned) {
    g_owned_add_calls.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_runtime_add_calls.fetch_add(1, std::memory_order_relaxed);
  }
}

void EnsureInstalled(PPCContext& ctx, std::uint8_t* base) noexcept {
  if (!Enabled() || !base) {
    g_state.store(State::Disabled, std::memory_order_release);
    return;
  }
  if (g_state.load(std::memory_order_acquire) == State::Installed) {
    const std::uint64_t commit_serial =
        map_editor::TransformCommitSerial();
    if (commit_serial ==
        g_applied_commit_serial.load(std::memory_order_acquire)) {
      return;
    }
    std::scoped_lock lock(g_install_mutex);
    const std::uint32_t previous_spline_data =
        g_spline_data_address.load(std::memory_order_acquire);
    float origin[3] = {};
    skate::world::GrindSplineBuildResult updated;
    std::uint32_t replacement_spline_data = 0;
    std::uint32_t replacement_runtime = 0;
    try {
      if (!IsGuestDataAddress(previous_spline_data) ||
          !native_collision::MapWorldOrigin(origin)) {
        throw std::runtime_error("native grind update is unavailable");
      }
      const skate::world::MapDefinition runtime = RuntimeGrindMap();
      updated = skate::world::BuildGrindSplineData(
          runtime, {origin[0], origin[1], origin[2]});
      if (!updated.ok || updated.blob.bytes.empty() ||
          updated.blob.rail_count == 0 ||
          updated.blob.segment_count == 0) {
        throw std::runtime_error(
            updated.error.empty() ? "native grind update build failed"
                                  : updated.error);
      }
      replacement_spline_data =
          REX_KERNEL_MEMORY()->SystemHeapAlloc(
              static_cast<std::uint32_t>(updated.blob.bytes.size()), 16);
      if (!replacement_spline_data) {
        throw std::runtime_error(
            "native grind replacement allocation failed");
      }
      if (!skate::world::FixupGrindSplineDataForGuest(
              updated.blob.bytes, replacement_spline_data)) {
        REX_KERNEL_MEMORY()->SystemHeapFree(replacement_spline_data);
        replacement_spline_data = 0;
        throw std::runtime_error(
            "native grind replacement fixup failed");
      }
      std::memcpy(
          base + replacement_spline_data, updated.blob.bytes.data(),
          updated.blob.bytes.size());
      const std::uint32_t grind_data =
          g_grind_data.load(std::memory_order_acquire);
      const std::uint32_t previous_runtime =
          g_runtime_object.load(std::memory_order_acquire);
      if (!IsGuestDataAddress(grind_data) ||
          !RegisterOwnedSpline(
              ctx, base, grind_data, replacement_spline_data,
              updated.blob.rail_count, replacement_runtime)) {
        // If registration appended an object before observer validation
        // failed, roll it back before releasing its source spline blob.
        if (IsGuestDataAddress(replacement_runtime) &&
            RetireRuntimeObject(ctx, base, replacement_runtime)) {
          REX_KERNEL_MEMORY()->SystemHeapFree(
              replacement_spline_data);
          replacement_spline_data = 0;
        }
        throw std::runtime_error(
            "native grind replacement registration failed");
      }
      if (!RetireRuntimeObject(
              ctx, base, previous_runtime)) {
        g_runtime_retire_failures.fetch_add(
            1, std::memory_order_relaxed);
        // Registration succeeded, so roll the replacement back when the old
        // object cannot be removed. This avoids leaving two authoritative
        // rails at different transforms after a recoverable refresh failure.
        if (RetireRuntimeObject(ctx, base, replacement_runtime)) {
          REX_KERNEL_MEMORY()->SystemHeapFree(
              replacement_spline_data);
          replacement_spline_data = 0;
        }
        throw std::runtime_error(
            "native grind previous runtime retirement failed");
      }
      REX_KERNEL_MEMORY()->SystemHeapFree(previous_spline_data);
      g_spline_data_address.store(
          replacement_spline_data, std::memory_order_release);
      g_spline_data_bytes.store(
          static_cast<std::uint32_t>(updated.blob.bytes.size()),
          std::memory_order_release);
      g_rail_count.store(
          updated.blob.rail_count, std::memory_order_release);
      g_segment_count.store(
          updated.blob.segment_count, std::memory_order_release);
      g_runtime_object.store(
          replacement_runtime, std::memory_order_release);
      g_runtime_refreshes.fetch_add(1, std::memory_order_relaxed);
      g_applied_commit_serial.store(
          commit_serial, std::memory_order_release);
      g_transform_updates.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO(
          "map-editor: grind runtime refreshed serial={} rails={} "
          "segments={} bytes={} old=0x{:08X} new=0x{:08X}",
          commit_serial, updated.blob.rail_count,
          updated.blob.segment_count, updated.blob.bytes.size(),
          previous_runtime, replacement_runtime);
    } catch (const std::exception& error) {
      g_transform_update_failures.fetch_add(
          1, std::memory_order_relaxed);
      g_applied_commit_serial.store(
          commit_serial, std::memory_order_release);
      REXLOG_ERROR(
          "map-editor: grind transform update failed serial={} "
          "reason='{}' replacement_blob=0x{:08X} "
          "replacement_runtime=0x{:08X}",
          commit_serial, error.what(), replacement_spline_data,
          replacement_runtime);
    } catch (...) {
      g_transform_update_failures.fetch_add(
          1, std::memory_order_relaxed);
      g_applied_commit_serial.store(
          commit_serial, std::memory_order_release);
      REXLOG_ERROR(
          "map-editor: grind transform update failed serial={} "
          "reason='unknown'",
          commit_serial);
    }
    return;
  }

  const std::uint32_t grind_data =
      g_grind_data.load(std::memory_order_acquire);
  if (!IsGuestDataAddress(grind_data)) {
    g_state.store(State::WaitingForRuntime, std::memory_order_release);
    return;
  }

  float translation_values[3] = {};
  if (!native_collision::MapWorldOrigin(translation_values)) {
    g_state.store(State::WaitingForMap, std::memory_order_release);
    return;
  }

  std::scoped_lock lock(g_install_mutex);
  if (g_state.load(std::memory_order_acquire) == State::Installed) {
    return;
  }
  g_install_attempts.fetch_add(1, std::memory_order_relaxed);
  g_state.store(State::Compiling, std::memory_order_release);

  skate::world::GrindSplineBuildResult build;
  try {
    const skate::world::MapDefinition runtime = RuntimeGrindMap();
    build = skate::world::BuildGrindSplineData(
        runtime,
        {translation_values[0], translation_values[1],
         translation_values[2]});
  } catch (...) {
    g_state.store(State::BuildFailed, std::memory_order_release);
    return;
  }
  if (!build.ok || build.blob.bytes.empty() ||
      build.blob.rail_count == 0 || build.blob.segment_count == 0) {
    g_state.store(State::BuildFailed, std::memory_order_release);
    return;
  }

  const std::uint32_t spline_data =
      REX_KERNEL_MEMORY()->SystemHeapAlloc(
          static_cast<std::uint32_t>(build.blob.bytes.size()), 16);
  if (!spline_data) {
    g_state.store(State::AllocationFailed, std::memory_order_release);
    return;
  }
  if (!skate::world::FixupGrindSplineDataForGuest(
          build.blob.bytes, spline_data)) {
    REX_KERNEL_MEMORY()->SystemHeapFree(spline_data);
    g_state.store(State::BuildFailed, std::memory_order_release);
    return;
  }
  std::memcpy(base + spline_data, build.blob.bytes.data(),
              build.blob.bytes.size());

  std::uint32_t runtime_object = 0;
  if (!RegisterOwnedSpline(
          ctx, base, grind_data, spline_data,
          build.blob.rail_count, runtime_object)) {
    // The native runtime may have retained pointers before the observer
    // detected a failure, so keep the persistent blob alive.
    g_spline_data_address.store(spline_data,
                                std::memory_order_release);
    g_spline_data_bytes.store(
        static_cast<std::uint32_t>(build.blob.bytes.size()),
        std::memory_order_release);
    g_state.store(State::RegistrationFailed,
                  std::memory_order_release);
    return;
  }

  g_spline_data_address.store(spline_data,
                              std::memory_order_release);
  g_spline_data_bytes.store(
      static_cast<std::uint32_t>(build.blob.bytes.size()),
      std::memory_order_release);
  g_rail_count.store(build.blob.rail_count,
                     std::memory_order_release);
  g_segment_count.store(build.blob.segment_count,
                        std::memory_order_release);
  g_runtime_object.store(runtime_object, std::memory_order_release);
  g_state.store(State::Installed, std::memory_order_release);
  g_applied_commit_serial.store(
      map_editor::TransformCommitSerial(), std::memory_order_release);
}

void AppendTelemetry(std::ostream& out) {
  out << " sandbox_native_grind=" << (Enabled() ? 1 : 0)
      << " sandbox_native_grind_state="
      << StateName(g_state.load(std::memory_order_acquire))
      << " sandbox_native_grind_data="
      << g_grind_data.load(std::memory_order_acquire)
      << " sandbox_native_grind_memory_group="
      << g_memory_group.load(std::memory_order_acquire)
      << " sandbox_native_grind_spline_data="
      << g_spline_data_address.load(std::memory_order_acquire)
      << " sandbox_native_grind_bytes="
      << g_spline_data_bytes.load(std::memory_order_acquire)
      << " sandbox_native_grind_rails="
      << g_rail_count.load(std::memory_order_acquire)
      << " sandbox_native_grind_segments="
      << g_segment_count.load(std::memory_order_acquire)
      << " sandbox_native_grind_last_add_count="
      << g_last_add_vector_count.load(std::memory_order_acquire)
      << " sandbox_native_grind_runtime_loads="
      << g_runtime_load_calls.load(std::memory_order_relaxed)
      << " sandbox_native_grind_runtime_adds="
      << g_runtime_add_calls.load(std::memory_order_relaxed)
      << " sandbox_native_grind_owned_loads="
      << g_owned_load_calls.load(std::memory_order_relaxed)
      << " sandbox_native_grind_owned_adds="
      << g_owned_add_calls.load(std::memory_order_relaxed)
      << " sandbox_native_grind_attempts="
      << g_install_attempts.load(std::memory_order_relaxed)
      << " sandbox_native_grind_transform_updates="
      << g_transform_updates.load(std::memory_order_relaxed)
      << " sandbox_native_grind_transform_failures="
      << g_transform_update_failures.load(std::memory_order_relaxed)
      << " sandbox_native_grind_runtime_refreshes="
      << g_runtime_refreshes.load(std::memory_order_relaxed)
      << " sandbox_native_grind_runtime_retire_failures="
      << g_runtime_retire_failures.load(std::memory_order_relaxed)
      << " sandbox_native_grind_runtime_object="
      << g_runtime_object.load(std::memory_order_relaxed)
      << " sandbox_native_grind_commit_serial="
      << g_applied_commit_serial.load(std::memory_order_relaxed);
}

}  // namespace skate3::native_grind
