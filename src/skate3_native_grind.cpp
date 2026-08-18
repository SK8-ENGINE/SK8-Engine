#include "skate3_native_grind.h"

#include "generated/skate3_init.h"
#include "skate/world/grind_spline.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_native_collision.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <ostream>
#include <vector>

#include <rex/cvar.h>
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
std::mutex g_install_mutex;
thread_local bool g_installing_owned = false;

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
    build = skate::world::BuildGrindSplineData(
        mechanics_sandbox::map::ActiveDefinition(),
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

  const std::uint64_t adds_before =
      g_owned_add_calls.load(std::memory_order_acquire);
  PPCContext add = ctx;
  add.r3.u64 = grind_data;
  add.r4.u64 = spline_data;
  add.r5.u64 = g_memory_group.load(std::memory_order_acquire);
  g_installing_owned = true;
  sub_82C1EEF0(add, base);
  g_installing_owned = false;

  const std::uint64_t adds_after =
      g_owned_add_calls.load(std::memory_order_acquire);
  const std::uint32_t registered_count =
      g_last_add_vector_count.load(std::memory_order_acquire);
  if (adds_after != adds_before + 1 ||
      registered_count != build.blob.rail_count) {
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
  g_state.store(State::Installed, std::memory_order_release);
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
      << g_install_attempts.load(std::memory_order_relaxed);
}

}  // namespace skate3::native_grind
