// Focused diagnostics for Skate 3's Create-a-Skater gesture animation loader.

#include "generated/skate3_init.h"
#include "skate3_cac_gesture.h"
#include "skate3_dlc_runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/logging.h>

namespace {

std::atomic<uint64_t> g_last_active_millis{0};
std::atomic<int32_t> g_phase{-1};
std::atomic<int32_t> g_selected{-1};
std::atomic<uint64_t> g_last_state_signature{UINT64_MAX};
thread_local bool g_inside_character_gesture_update = false;
std::atomic<uint32_t> g_logged_gesture_dispatch{0};
std::atomic<uint32_t> g_logged_gesture_set_provider{0};
std::atomic<uint64_t> g_last_pending_consumer_signature{UINT64_MAX};
std::atomic<uint64_t> g_last_pending_update_signature{UINT64_MAX};
std::atomic<uint64_t> g_last_load_id_signature{UINT64_MAX};
std::atomic<uint64_t> g_last_load_name_signature{UINT64_MAX};

std::string ReadGuestString(uint8_t* base, uint32_t address) {
  if (!base || address < 0x10000 || address >= 0xE0000000) {
    return "<invalid>";
  }
  std::string value;
  value.reserve(96);
  for (size_t index = 0; index < 255; ++index) {
    const char character =
        static_cast<char>(REX_LOAD_U8(address + static_cast<uint32_t>(index)));
    if (!character) {
      return value;
    }
    value.push_back(character);
  }
  return value + "<unterminated>";
}

}  // namespace

namespace skate3::cac_gesture {

bool IsActive() {
  using namespace std::chrono;
  const uint64_t now = static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
  const uint64_t last = g_last_active_millis.load(std::memory_order_acquire);
  return last != 0 && now >= last && now - last <= 100;
}

bool IsUpdating() { return g_inside_character_gesture_update; }

int32_t Phase() { return g_phase.load(std::memory_order_acquire); }

int32_t Selected() { return g_selected.load(std::memory_order_acquire); }

uint32_t ResolveGestureDescriptorAddress(PPCContext& ctx, uint8_t* base,
                                         uint32_t address) {
  constexpr uint32_t kRetailDescriptorTable = 0x830BF5D8;
  constexpr std::string_view kGunplayIntent = "B_GSTR_GUNPLAYTEST";
  static std::atomic<bool> installed{false};

  if (address == kRetailDescriptorTable &&
      dlc_runtime::IsGunplayGestureArchiveMounted() &&
      !installed.load(std::memory_order_acquire)) {
    const PPCContext saved = ctx;
    PPCContext constructor = ctx;
    constructor.r1.u32 = (ctx.r1.u32 - 0x100) & ~0xFu;
    const uint32_t name_address = constructor.r1.u32 + 0x40;
    for (size_t index = 0; index < kGunplayIntent.size(); ++index) {
      REX_STORE_U8(name_address + static_cast<uint32_t>(index),
                   static_cast<uint8_t>(kGunplayIntent[index]));
    }
    REX_STORE_U8(name_address + static_cast<uint32_t>(kGunplayIntent.size()),
                 0);
    constructor.r3.u64 = kRetailDescriptorTable;
    constructor.r4.u64 = name_address;
    sub_823C3B00(constructor, base);
    ctx = saved;
    installed.store(true, std::memory_order_release);
    REXLOG_WARN(
        "cac-gesture: replaced Air Guitar intent descriptor with '{}'",
        kGunplayIntent);
  }
  return address;
}

void ObserveLoadBaseAnims(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32;
  if (manager < 0x10000 || manager >= 0xE0000000) {
    return;
  }
  REXLOG_WARN("cac-gesture: LoadBaseAnims manager=0x{:08X} female={}",
              manager, REX_LOAD_U8(manager));
}

void ObservePendingGestureUpdate(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32;
  if (manager < 0x10000 || manager >= 0xE0000000) {
    return;
  }
  const uint64_t requested = REX_LOAD_U64(manager + 16);
  if (requested == UINT64_MAX) {
    return;
  }
  const uint64_t signature =
      (static_cast<uint64_t>(manager) << 32) ^
      requested ^ (requested >> 32);
  if (g_last_pending_update_signature.exchange(
          signature, std::memory_order_acq_rel) == signature) {
    return;
  }
  REXLOG_WARN(
      "cac-gesture: pending manager=0x{:08X} base=0x{:016X} "
      "requested=0x{:016X} caller=0x{:08X}",
      manager, REX_LOAD_U64(manager + 8), requested, ctx.lr);
}

void ObserveLoadGestureById(PPCContext& ctx, uint8_t* base) {
  const uint64_t signature =
      (static_cast<uint64_t>(ctx.r3.u32) << 32) ^
      ctx.r4.u64 ^ (ctx.r4.u64 >> 32) ^
      (static_cast<uint64_t>(ctx.r5.u32 & 0xFF) << 24);
  if (g_last_load_id_signature.exchange(signature,
                                        std::memory_order_acq_rel) ==
      signature) {
    return;
  }
  REXLOG_WARN(
      "cac-gesture: LoadGesture(id) library=0x{:08X} id=0x{:016X} "
      "option={} caller=0x{:08X}",
      ctx.r3.u32, ctx.r4.u64, ctx.r5.u32 & 0xFF, ctx.lr);
}

void ObserveLoadGestureByName(PPCContext& ctx, uint8_t* base) {
  const uint32_t library = ctx.r3.u32;
  const uint32_t name_address = ctx.r4.u32;
  const uint64_t signature =
      (static_cast<uint64_t>(library) << 32) | name_address;
  if (g_last_load_name_signature.exchange(signature,
                                          std::memory_order_acq_rel) ==
      signature) {
    return;
  }
  REXLOG_WARN(
      "cac-gesture: LoadGesture(name) library=0x{:08X} name='{}' "
      "caller=0x{:08X}",
      library, ReadGuestString(base, name_address), ctx.lr);
}

void ObservePendingGestureGetter(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32;
  if (manager < 0x10000 || manager >= 0xE0000000) {
    return;
  }
  const uint32_t vtable = REX_LOAD_U32(manager);
  const uint32_t pending = REX_LOAD_U8(manager + 3304);
  const uint32_t gesture = REX_LOAD_U32(manager + 3300);
  const uint32_t option = REX_LOAD_U8(manager + 3305);
  const uint64_t signature =
      (static_cast<uint64_t>(ctx.lr) << 32) |
      (static_cast<uint64_t>(pending) << 31) |
      (static_cast<uint64_t>(gesture) << 8) | option;
  if (g_last_pending_consumer_signature.exchange(
          signature, std::memory_order_acq_rel) == signature) {
    return;
  }
  REXLOG_WARN(
      "cac-gesture: pending-consumer caller=0x{:08X} manager=0x{:08X} "
      "vtable=0x{:08X} pending={} gesture={} option={}",
      ctx.lr, manager, vtable, pending, gesture, option);
}

}  // namespace skate3::cac_gesture

// Sk8::Behaviours::CharacterGesture update (resolved from the registered
// behavior factory and vtable). It is the common on-board/off-board gesture
// path and writes the selected gesture request into the CAC animation manager.
extern "C" REX_FUNC(sub_82BAA320) {
  const uint32_t state_context = ctx.r4.u32;
  const uint32_t context_owner =
      state_context >= 0x10000 ? REX_LOAD_U32(state_context + 4) : 0;
  const uint32_t behavior_state =
      state_context >= 0x10000 ? REX_LOAD_U32(state_context + 8) : 0;
  const uint32_t animation_manager =
      context_owner >= 0x10000 ? REX_LOAD_U32(context_owner + 1800) : 0;
  if (context_owner >= 0x10000 && context_owner < 0xE0000000) {
    PPCContext probe = ctx;
    probe.r3.u64 = context_owner + 4;
    probe.r4.u64 = 0x82453410;
    sub_82965630(probe, base);
    const uint32_t provider = probe.r3.u32;
    const uint32_t vtable =
        provider >= 0x10000 && provider < 0xE0000000
            ? REX_LOAD_U32(provider)
            : 0;
    const uint32_t getter =
        vtable >= 0x10000 && vtable < 0xE0000000
            ? REX_LOAD_U32(vtable + 56)
            : 0;
    uint32_t expected = 0;
    if (getter &&
        g_logged_gesture_set_provider.compare_exchange_strong(
            expected, getter, std::memory_order_acq_rel)) {
      REXLOG_WARN(
          "cac-gesture: set provider=0x{:08X} vtable=0x{:08X} "
          "getter=0x{:08X}",
          provider, vtable, getter);
    }
  }
  if (animation_manager >= 0x10000 &&
      animation_manager < 0xE0000000) {
    const uint32_t vtable = REX_LOAD_U32(animation_manager);
    const uint32_t dispatch =
        vtable >= 0x10000 && vtable < 0xE0000000
            ? REX_LOAD_U32(vtable + 244)
            : 0;
    uint32_t expected = 0;
    if (dispatch &&
        g_logged_gesture_dispatch.compare_exchange_strong(
            expected, dispatch, std::memory_order_acq_rel)) {
      REXLOG_WARN(
          "cac-gesture: CharacterGesture manager=0x{:08X} "
          "vtable=0x{:08X} gesture_dispatch=0x{:08X}",
          animation_manager, vtable, dispatch);
    }
  }

  g_inside_character_gesture_update = true;
  __imp__sub_82BAA320(ctx, base);
  g_inside_character_gesture_update = false;

  if (behavior_state >= 0x10000 && behavior_state < 0xE0000000) {
    const uint8_t active = REX_LOAD_U8(behavior_state + 8);
    const int32_t phase =
        static_cast<int32_t>(REX_LOAD_U32(behavior_state + 12));
    const int32_t selected =
        static_cast<int32_t>(REX_LOAD_U32(behavior_state + 16));
    const int32_t hand =
        static_cast<int32_t>(REX_LOAD_U32(behavior_state + 20));
    if (active) {
      using namespace std::chrono;
      g_last_active_millis.store(
          static_cast<uint64_t>(
              duration_cast<milliseconds>(
                  steady_clock::now().time_since_epoch())
                  .count()),
          std::memory_order_release);
      // CharacterGesture runs for every nearby skater. Inactive actors must
      // not erase the phase and branch belonging to the active gesture that
      // armed the final-pose observation window.
      g_phase.store(phase, std::memory_order_release);
      g_selected.store(selected, std::memory_order_release);
    }
    if (active || phase != -1 || selected != -1 || hand != -1) {
      const uint64_t signature =
          (static_cast<uint64_t>(active) << 56) |
          (static_cast<uint64_t>(static_cast<uint8_t>(phase)) << 48) |
          (static_cast<uint64_t>(static_cast<uint16_t>(selected)) << 24) |
          static_cast<uint32_t>(hand);
      if (g_last_state_signature.exchange(signature,
                                          std::memory_order_acq_rel) ==
          signature) {
        return;
      }
      const uint64_t base_id =
          animation_manager >= 0x10000
              ? REX_LOAD_U64(animation_manager + 8)
              : UINT64_MAX;
      const uint64_t requested_id =
          animation_manager >= 0x10000
              ? REX_LOAD_U64(animation_manager + 16)
              : UINT64_MAX;
      REXLOG_WARN(
          "cac-gesture: CharacterGesture context=0x{:08X} state=0x{:08X} "
          "active={} phase={} selected={} hand={} manager=0x{:08X} "
          "base=0x{:016X} requested=0x{:016X}",
          state_context, behavior_state, active, phase, selected, hand,
          animation_manager, base_id, requested_id);
    }
  }
}

// Character animation gesture request setter. CharacterGesture selects one of
// the four persisted D-pad values and calls this method immediately before the
// animation controller consumes it.
extern "C" REX_FUNC(sub_8258FB30) {
  static std::atomic<uint64_t> last_dispatch{UINT64_MAX};
  const uint32_t original_gesture = ctx.r4.u32;
  if (original_gesture == 0 &&
      skate3::dlc_runtime::IsGunplayGestureArchiveMounted()) {
    static std::atomic<bool> registration_logged{false};
    if (!registration_logged.exchange(true, std::memory_order_acq_rel)) {
      const uint32_t collection =
          skate3::dlc_runtime::ProbeGunplayGestureCollection(ctx, base);
      REXLOG_WARN(
          "cac-gesture: Air Guitar activation registration probe "
          "cac_body/gunplaytest=0x{:08X}",
          collection);
    }
  }
  const uint64_t signature =
      (static_cast<uint64_t>(ctx.lr) << 32) |
      (static_cast<uint64_t>(ctx.r4.u32) << 8) | (ctx.r5.u32 & 0xFF);
  if (last_dispatch.exchange(signature, std::memory_order_acq_rel) !=
      signature) {
    REXLOG_WARN(
        "cac-gesture: dispatch caller=0x{:08X} manager=0x{:08X} "
        "gesture={} original={} option={}",
        ctx.lr, ctx.r3.u32, ctx.r4.u32, original_gesture,
        ctx.r5.u32 & 0xFF);
  }
  __imp__sub_8258FB30(ctx, base);
}

// Character animation gesture-pending getter. Recording LR when the flag is
// set identifies the virtual consumer that translates the profile index into
// the CAC/VLT row ID.
extern "C" REX_FUNC(sub_8258FB48) {
  __imp__sub_8258FB48(ctx, base);
}

// Sk8::GetCACSettings(int, NaturalStance&, CAC::GestureSet&). Capture the
// player's persisted gesture-set payload after the retail function fills it.
extern "C" REX_FUNC(sub_82590B50) {
  const uint32_t player = ctx.r3.u32;
  const uint32_t stance = ctx.r4.u32;
  const uint32_t gesture_set = ctx.r5.u32;
  __imp__sub_82590B50(ctx, base);
  const uint32_t retail_key_address = ctx.r3.u32;
  const std::string retail_key = ReadGuestString(base, retail_key_address);
  REXLOG_WARN(
      "cac-gesture: GetCACSettings result player={} key='{}'@0x{:08X}",
      player, retail_key, retail_key_address);
  if (gesture_set >= 0x10000 && gesture_set < 0xE0000000) {
    std::array<uint32_t, 12> words{};
    for (uint32_t index = 0; index < words.size(); ++index) {
      words[index] = REX_LOAD_U32(gesture_set + index * 4);
    }
    REXLOG_WARN(
        "cac-gesture: GetCACSettings player={} stance_ptr=0x{:08X} "
        "set=0x{:08X} words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},"
        "{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
        player, stance, gesture_set, words[0], words[1], words[2], words[3],
        words[4], words[5], words[6], words[7], words[8], words[9], words[10],
        words[11]);
  }
}
