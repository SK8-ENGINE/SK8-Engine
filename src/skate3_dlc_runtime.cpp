#include "skate3_dlc_runtime.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <string>

#include <rex/logging.h>

namespace {

std::atomic<uint32_t> g_manager{0};
std::atomic<bool> g_gunplay_gesture_archive_mounted{false};
std::atomic<uint32_t> g_gunplay_collection{0};
std::atomic<uint64_t> g_last_cac_body_key{UINT64_MAX};

void LogCall(const char* operation, PPCContext& ctx) {
  REXLOG_WARN(
      "skate3-dlc: {} manager=0x{:08X} r4=0x{:08X} r5=0x{:08X} "
      "caller=0x{:08X}",
      operation, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.lr);
}

std::string ReadGuestString(uint8_t* base, uint32_t address,
                            size_t max_length = 512) {
  if (!address) {
    return {};
  }
  std::string value;
  value.reserve(128);
  for (size_t i = 0; i < max_length; ++i) {
    const char character = static_cast<char>(REX_LOAD_U8(address + i));
    if (!character) {
      break;
    }
    value.push_back(character);
  }
  return value;
}

}  // namespace

namespace skate3::dlc_runtime {

bool IsGunplayGestureArchiveMounted() {
  return g_gunplay_gesture_archive_mounted.load(std::memory_order_acquire);
}

uint32_t ProbeGunplayGestureCollection(PPCContext& ctx, uint8_t* base) {
  constexpr uint64_t kCacBodyClass = 0x95E142284549717FULL;
  constexpr uint64_t kGunplayTestKey = 0x7328108B01BE6A05ULL;
  PPCContext probe = ctx;
  probe.r3.u64 = kCacBodyClass;
  probe.r4.u64 = kGunplayTestKey;
  sub_82B69B08(probe, base);
  const uint32_t collection = probe.r3.u32;
  if (collection != 0 &&
      g_gunplay_collection.exchange(collection, std::memory_order_acq_rel) ==
          0) {
    REXLOG_WARN(
        "skate3-dlc: verified registered collection "
        "cac_body/gunplaytest=0x{:08X}",
        collection);
  }
  return collection;
}

void ObserveManagerConstructor(PPCContext& ctx) {
  g_manager.store(ctx.r3.u32, std::memory_order_release);
  g_gunplay_gesture_archive_mounted.store(false, std::memory_order_release);
  LogCall("Manager::Manager", ctx);
}

void ObserveManagerRun(PPCContext& ctx) { LogCall("Manager::Run", ctx); }

void ObserveManagerEnumerate(PPCContext& ctx) {
  LogCall("Manager::EnumerateContent", ctx);
}

void ObserveManagerRefresh(PPCContext& ctx) {
  LogCall("Manager::Refresh", ctx);
}

void ObserveDriverMount(PPCContext& ctx) {
  LogCall("XenonDLCDriver::Mount", ctx);
}

void ObserveContentManagerEnumerate(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kActiveUserIndexAddress = 0x82FC8851;
  const auto active_user_index = REX_LOAD_U8(kActiveUserIndexAddress);
  REXLOG_WARN(
      "skate3-dlc: ContentManager::EnumerateContent manager=0x{:08X} "
      "active_user={} (raw=0x{:02X}) caller=0x{:08X}",
      ctx.r3.u32, static_cast<int8_t>(active_user_index), active_user_index,
      ctx.lr);
}

void ObserveAddArchiveFromFile(PPCContext& ctx, uint8_t* base) {
  const std::string path = ReadGuestString(base, ctx.r4.u32);
  if (path.find("dlc_codex_gunplay") != std::string::npos) {
    g_gunplay_gesture_archive_mounted.store(true,
                                            std::memory_order_release);
    REXLOG_WARN("skate3-dlc: armed Gunplay CAC slot bridge for '{}'", path);
  }
  REXLOG_WARN(
      "skate3-dlc: BigHandler::AddArchiveFromFile handler=0x{:08X} "
      "path='{}' flags=0x{:08X} caller=0x{:08X}",
      ctx.r3.u32, path, ctx.r5.u32, ctx.lr);
}

void ObserveAsyncFileOpen(PPCContext& ctx, uint8_t* base) {
  const std::string path = ReadGuestString(base, ctx.r4.u32);
  std::string lower_path = path;
  std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  const bool gesture_relevant =
      lower_path.find("gunplay") != std::string::npos ||
      lower_path.find("gesture") != std::string::npos ||
      lower_path.find(".vaultlist") != std::string::npos ||
      lower_path.ends_with(".vlt") || lower_path.ends_with(".bin") ||
      lower_path.ends_with(".abin");
  if (!gesture_relevant) {
    return;
  }
  REXLOG_WARN(
      "skate3-dlc: async-open manager=0x{:08X} path='{}' r5=0x{:08X} "
      "r6=0x{:08X} r7=0x{:08X} r8=0x{:08X} caller=0x{:08X}",
      ctx.r3.u32, path, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
      ctx.lr);
}

void ObserveVaultDlcLoadUpdate(PPCContext& ctx, uint8_t* base) {
  LogCall("cVaultManager::DLCLoadUpdate", ctx);
  if (!IsGunplayGestureArchiveMounted() ||
      g_gunplay_collection.load(std::memory_order_acquire) != 0) {
    return;
  }

  ProbeGunplayGestureCollection(ctx, base);
}

void ObserveFindCollection(PPCContext& ctx) {
  constexpr uint64_t kCacBodyClass = 0x95E142284549717FULL;
  if (ctx.r3.u64 != kCacBodyClass) {
    return;
  }
  const uint64_t key = ctx.r4.u64;
  if (g_last_cac_body_key.exchange(key, std::memory_order_acq_rel) == key) {
    return;
  }
  REXLOG_WARN(
      "skate3-dlc: Attrib::FindCollection class=cac_body "
      "key=0x{:016X} caller=0x{:08X}",
      key, ctx.lr);
}

void ObserveLanguageDlcLoadUpdate(PPCContext& ctx) {
  LogCall("LanguageManager::DLCLanguageDBLoadUpdate", ctx);
}

}  // namespace skate3::dlc_runtime
