#include "skate3_app_common.h"

#include "skate3_custom_trick.h"
#include "skate3_demo_path.h"
#include "skate3_fov.h"
#include "skate3_input_lab.h"
#include "skate3_iso_installer.h"
#include "skate3_map_editor.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_multiplayer_assets.h"
#include "skate3_multiplayer_session.h"
#include "skate3_native_render.h"
#include "skate3_native_scene.h"
#include "skate3_release_updater.h"
#include "skate3_scoring.h"
#include "skate3_screenshot.h"
#include "skate3_shader_disasm.h"
#include "skate3_steam_backend.h"
#include "skate3_title_update_installer.h"
#include "skate3_user_settings.h"
#include "skate3_vanilla_ui/skate3_ui_asset_cache.h"
#include "skate3_vanilla_ui/skate3_ui_audio.h"
#include "skate3_version.h"
#include "skate3_win_icon.h"

#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/filesystem.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <spawn.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/system.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/simple_settings_overlay.h>
#include <rex/ui/window.h>

#include <imgui.h>
#include <toml++/toml.hpp>

#if defined(__linux__) || defined(__APPLE__)
extern char **environ;
#endif

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

// Register multi-entry-function alternate entries that rex's analyzer can't
// lift via config because they overlap with auto-discovered parent functions.
extern "C" REX_FUNC(__restgprlr_19);

// Defined in skate3_native_scene.cpp; toggled by the showcase hotkey below.
REXCVAR_DECLARE(bool, skate3_native_render_capture_hotkeys);
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
// Defined in skate3_native_scene.cpp; the freecam hotkey below toggles it,
// and while it captures input the guest input system is gated off.
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam_capture_input);

REXCVAR_DEFINE_STRING(skate3_dlc_root, "", "Skate 3",
                      "Directory containing Skate 3 DLC package files");
REXCVAR_DEFINE_BOOL(
    skate3_auto_install_dlc, true, "Skate 3",
    "Install DLC package files found in configured DLC folders");
REXCVAR_DEFINE_BOOL(
    skate3_ultrawide, false, "Skate 3",
    "Ultrawide display support: the native renderer draws true widescreen "
    "frames at the host display aspect (requires the native renderer; the "
    "emulated fallback presents 16:9 pillarboxed)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(skate3_field_of_view, 60.0, "Skate 3",
                      "Gameplay camera field of view in degrees")
    .range(40.0, 120.0);
REXCVAR_DEFINE_BOOL(
    skate3_ultrawide_widen_game_frustum, true, "Skate 3",
    "Widen the game-side main-world cull frustum to match the ultrawide view");
REXCVAR_DEFINE_DOUBLE(
    skate3_ultrawide_target_aspect, 0.0, "Skate 3",
    "Ultrawide display aspect (0 = derive from the host display at boot)")
    .range(0.0, 8.0);

namespace {

void ApplyDemoPathProfileOverride() {
  if (rex::cvar::Query<bool>("skate3_direct_boot")) {
    // Direct boot must resume a local profile and its save. An install with
    // no persisted profiles file uses an ephemeral default profile, so make
    // its offline sign-in state explicit before XAM initializes. This is a
    // local profile session, not an Xbox Live sign-in.
    rex::cvar::SetFlagByName("user_profile_signed_in", "true");
    rex::cvar::SetFlagByName("user_live_signed_in", "false");
    return;
  }
  if (!rex::cvar::Query<bool>("skate3_demo_path") &&
      !rex::cvar::Query<bool>("skate3_demo_path_probe")) {
    return;
  }
  if (rex::cvar::Query<bool>("skate3_demo_path_signed_in")) {
    // Closer-to-real-use runs: keep the user's profile and save so the boot
    // flow resumes their career instead of a signed-out session.
    return;
  }

  rex::cvar::SetFlagByName("user_profile_signed_in", "false");
  rex::cvar::SetFlagByName("user_live_signed_in", "false");
}

#if defined(__linux__) || defined(__APPLE__)
std::vector<std::string> CurrentProcessArgumentsForRestart(
    const std::filesystem::path &executable_path) {
  std::vector<std::string> args;
#if defined(__APPLE__)
  int argc = *_NSGetArgc();
  char **argv = *_NSGetArgv();
  args.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i] ? argv[i] : "");
  }
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  char ch = 0;
  while (cmdline.get(ch)) {
    if (ch == '\0') {
      args.push_back(std::move(arg));
      arg.clear();
    } else {
      arg.push_back(ch);
    }
  }
  if (!arg.empty()) {
    args.push_back(std::move(arg));
  }
#endif

  const std::string executable = rex::path_to_utf8(executable_path);
  if (args.empty()) {
    args.push_back(executable);
  } else {
    args[0] = executable;
  }
  return args;
}

void SetRestartArgument(std::vector<std::string> &args, std::string name,
                        std::string value) {
  const std::string option = "--" + name;
  const std::string option_with_equals = option + "=";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == option) {
      if (i + 1 < args.size()) {
        args[i + 1] = std::move(value);
      } else {
        args.push_back(std::move(value));
      }
      return;
    }
    if (args[i].starts_with(option_with_equals)) {
      args[i] = option_with_equals + value;
      return;
    }
  }

  args.push_back(std::move(option));
  args.push_back(std::move(value));
}
#endif

constexpr std::string_view kUserDirectoryName = "skate3";
constexpr std::string_view kSettingsFilename = "settings.toml";
constexpr std::string_view kMapsDirectoryName = "maps";
constexpr std::string_view kActiveMapFilename = "active_map.txt";

std::filesystem::path AbsoluteMapPath(const std::filesystem::path &path) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal();
}

bool IsSkatePackage(const std::filesystem::path &path) {
  std::string extension = rex::path_to_utf8(path.extension());
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension == ".skate";
}

bool SameMapFile(const std::filesystem::path &left,
                 const std::filesystem::path &right) {
  if (left.empty() || right.empty()) {
    return false;
  }
  std::error_code ec;
  if (std::filesystem::equivalent(left, right, ec) && !ec) {
    return true;
  }
  return AbsoluteMapPath(left) == AbsoluteMapPath(right);
}

struct SkatePackageHeader {
  bool valid = false;
  bool supported = false;
  int version = 0;
  std::string display_name;
  std::string issue;
};

SkatePackageHeader InspectSkatePackage(const std::filesystem::path &path) {
  SkatePackageHeader result;
  std::ifstream stream(path, std::ios::binary);
  std::array<char, 12> header{};
  stream.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (!stream || std::memcmp(header.data(), "SKATE", 5) != 0 ||
      !std::isdigit(static_cast<unsigned char>(header[5])) ||
      !std::isdigit(static_cast<unsigned char>(header[6])) ||
      header[7] != '\0') {
    result.issue = "This file is not a valid SKATE map package.";
    return result;
  }
  result.version = (header[5] - '0') * 10 + (header[6] - '0');
  const std::uint32_t endian =
      static_cast<std::uint8_t>(header[8]) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[9])) << 8) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[10]))
       << 16) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[11])) << 24);
  if (endian != 0x12345678u) {
    result.issue = "This SKATE package uses an unsupported byte order.";
    return result;
  }
  result.valid = true;
  result.supported =
      skate::world::IsSupportedOwnedMapPackageVersion(result.version);
  if (!result.supported) {
    result.issue =
        result.version > skate::world::kLatestSupportedOwnedMapPackageVersion
            ? "This map uses SKATE v" + std::to_string(result.version) +
                  ". Update the Custom Engine Layer before loading it."
            : "This map uses an unsupported legacy SKATE format.";
    return result;
  }
  std::array<std::uint8_t, 4> size_bytes{};
  stream.read(reinterpret_cast<char *>(size_bytes.data()),
              static_cast<std::streamsize>(size_bytes.size()));
  if (!stream) {
    result.valid = false;
    result.supported = false;
    result.issue = "This SKATE package has a truncated header.";
    return result;
  }
  const std::uint32_t size = size_bytes[0] |
                             (static_cast<std::uint32_t>(size_bytes[1]) << 8) |
                             (static_cast<std::uint32_t>(size_bytes[2]) << 16) |
                             (static_cast<std::uint32_t>(size_bytes[3]) << 24);
  if (size == 0 || size > 64u * 1024u) {
    result.valid = false;
    result.supported = false;
    result.issue = "This SKATE package has an invalid map name.";
    return result;
  }
  std::string name(size, '\0');
  stream.read(name.data(), static_cast<std::streamsize>(name.size()));
  if (!stream) {
    result.valid = false;
    result.supported = false;
    result.issue = "This SKATE package has a truncated map name.";
    return result;
  }
  name.erase(std::remove_if(name.begin(), name.end(),
                            [](unsigned char value) {
                              return value < 0x20 && value != '\t';
                            }),
             name.end());
  result.display_name = std::move(name);
  return result;
}

std::string ReadSkateDisplayName(const std::filesystem::path &path) {
  return InspectSkatePackage(path).display_name;
}

bool IsSupportedSkatePackage(const std::filesystem::path &path) {
  return InspectSkatePackage(path).supported;
}

std::filesystem::path EnvironmentMapPath() {
  const char *selected = std::getenv("SKATE3_OWNED_MAP");
  return selected && selected[0] ? AbsoluteMapPath(selected)
                                 : std::filesystem::path{};
}

void SetEnvironmentMapPath(const std::filesystem::path &path) {
  const auto absolute = AbsoluteMapPath(path);
#if defined(_WIN32)
  _wputenv_s(L"SKATE3_OWNED_MAP", absolute.c_str());
#else
  const std::string utf8_path = rex::path_to_utf8(absolute);
  setenv("SKATE3_OWNED_MAP", utf8_path.c_str(), 1);
#endif
}

bool SaveSelectedMap(const std::filesystem::path &maps_path,
                     const std::filesystem::path &package_path);

void ConfigureMapsFolder(const std::filesystem::path &maps_path) {
  std::error_code ec;
  std::filesystem::create_directories(maps_path, ec);
  if (ec) {
    REXLOG_WARN("Could not create custom Maps folder '{}': {}",
                maps_path.string(), ec.message());
    return;
  }
  if (!EnvironmentMapPath().empty()) {
    return;
  }
  std::ifstream selected_file(maps_path / kActiveMapFilename);
  std::string selected_path;
  std::getline(selected_file, selected_path);
  const auto package_path = AbsoluteMapPath(selected_path);
  if (!package_path.empty() && IsSkatePackage(package_path) &&
      IsSupportedSkatePackage(package_path) &&
      std::filesystem::is_regular_file(package_path, ec) && !ec) {
    SetEnvironmentMapPath(package_path);
    REXLOG_INFO("Restored selected custom map '{}'", package_path.string());
    return;
  }

  // A fresh install, a moved Maps folder, or a selection left by an older
  // build should still enter the Custom Engine Layer without requiring
  // command-line flags. Prefer the project starter map when it is installed;
  // otherwise choose the first SKATE package deterministically.
  std::vector<std::filesystem::path> installed_maps;
  ec.clear();
  for (std::filesystem::directory_iterator
           it(maps_path,
              std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || ec || !IsSkatePackage(it->path())) {
      ec.clear();
      continue;
    }
    if (IsSupportedSkatePackage(it->path())) {
      installed_maps.push_back(AbsoluteMapPath(it->path()));
    }
  }
  std::sort(installed_maps.begin(), installed_maps.end(),
            [](const auto &left, const auto &right) {
              return rex::path_to_utf8(left.filename()) <
                     rex::path_to_utf8(right.filename());
            });
  if (installed_maps.empty()) {
    return;
  }

  const auto preferred_path =
      AbsoluteMapPath(maps_path / "blender_bake_showcase.skate");
  const auto preferred = std::find_if(
      installed_maps.begin(), installed_maps.end(), [&](const auto &candidate) {
        return SameMapFile(candidate, preferred_path);
      });
  const auto &startup_map =
      preferred != installed_maps.end() ? *preferred : installed_maps.front();
  SetEnvironmentMapPath(startup_map);
  REXLOG_INFO("Selected startup custom map '{}'", startup_map.string());
  if (!SaveSelectedMap(maps_path, startup_map)) {
    REXLOG_WARN("Could not persist startup custom map '{}'",
                startup_map.string());
  }
}

std::filesystem::path
ResolveMapsFolder(const std::filesystem::path &user_data_root) {
  const auto portable_maps =
      rex::filesystem::GetAppRootFolder() / std::string(kMapsDirectoryName);
  std::error_code ec;
  if (std::filesystem::is_directory(portable_maps, ec) && !ec) {
    return portable_maps;
  }
  return user_data_root / std::string(kMapsDirectoryName);
}

bool SaveSelectedMap(const std::filesystem::path &maps_path,
                     const std::filesystem::path &package_path) {
  std::error_code ec;
  std::filesystem::create_directories(maps_path, ec);
  if (ec) {
    return false;
  }
  std::ofstream output(maps_path / kActiveMapFilename,
                       std::ios::binary | std::ios::trunc);
  output << rex::path_to_utf8(AbsoluteMapPath(package_path)) << '\n';
  return output.good();
}

rex::ui::SimpleMapState
DiscoverCustomMaps(const std::filesystem::path &maps_path) {
  rex::ui::SimpleMapState state;
  state.maps_folder = maps_path;
  auto active_path = EnvironmentMapPath();
  if (active_path.empty()) {
    active_path = AbsoluteMapPath("owned_maps/blender_bake_showcase.skate");
  }
  std::error_code ec;
  std::filesystem::create_directories(maps_path, ec);
  ec.clear();
  for (std::filesystem::directory_iterator
           it(maps_path,
              std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || ec || !IsSkatePackage(it->path())) {
      ec.clear();
      continue;
    }
    rex::ui::SimpleMapInfo map;
    map.package_path = AbsoluteMapPath(it->path());
    const auto header = InspectSkatePackage(map.package_path);
    map.name = header.display_name;
    if (map.name.empty()) {
      map.name = rex::path_to_utf8(map.package_path.stem());
    }
    map.compatible = header.supported;
    map.compatibility_note = header.issue;
    map.active = SameMapFile(map.package_path, active_path);
    state.maps.push_back(std::move(map));
  }
  std::sort(state.maps.begin(), state.maps.end(),
            [](const rex::ui::SimpleMapInfo &left,
               const rex::ui::SimpleMapInfo &right) {
              return left.name < right.name;
            });
  for (int index = 0; index < static_cast<int>(state.maps.size()); ++index) {
    if (state.maps[index].active) {
      state.selected_index = index;
      state.active_name = state.maps[index].name;
      break;
    }
  }
  if (state.active_name.empty() && !active_path.empty()) {
    state.active_name = ReadSkateDisplayName(active_path);
    if (state.active_name.empty()) {
      state.active_name = rex::path_to_utf8(active_path.stem());
    }
  }
  return state;
}

void OpenFolderInFileManager(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
#if defined(_WIN32)
  std::wstring command = L"explorer.exe \"" + path.wstring() + L"\"";
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup_info, &process_info)) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
  } else {
    REXLOG_WARN("Could not open custom Maps folder '{}'", path.string());
  }
#else
  REXLOG_INFO("Custom Maps folder: {}", path.string());
#endif
}
constexpr std::string_view kDlcDirectoryName = "dlc";
constexpr std::string_view kSavesDirectoryName = "saves";
constexpr double kSixteenNineAspect = 16.0 / 9.0;
constexpr double kUltrawideAspectEpsilon = 0.01;

struct DisplaySize {
  int32_t width = 0;
  int32_t height = 0;
};

#if defined(_WIN32)
BOOL CALLBACK CollectMonitorCallback(HMONITOR monitor_handle, HDC, LPRECT,
                                     LPARAM data) {
  auto *monitors = reinterpret_cast<std::vector<HMONITOR> *>(data);
  monitors->push_back(monitor_handle);
  return TRUE;
}

HMONITOR GetConfiguredMonitorHandle() {
  const int32_t monitor_index = REXCVAR_GET(monitor);
  if (monitor_index > 0) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorCallback,
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitor_index <= static_cast<int32_t>(monitors.size())) {
      return monitors[monitor_index - 1];
    }
  }

  POINT origin{0, 0};
  return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  HMONITOR monitor_handle = GetConfiguredMonitorHandle();
  if (!monitor_handle) {
    return std::nullopt;
  }

  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!GetMonitorInfo(monitor_handle, &monitor_info)) {
    return std::nullopt;
  }

  const int32_t width =
      monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
  const int32_t height =
      monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  return DisplaySize{width, height};
}
#else
std::optional<DisplaySize> QueryFullscreenMonitorSize() { return std::nullopt; }
#endif

std::optional<DisplaySize> ResolveUltrawideTargetDisplaySize() {
  if (rex::cvar::HasNonDefaultValue("resolution")) {
    return std::nullopt;
  }

  if (REXCVAR_GET(fullscreen)) {
    return QueryFullscreenMonitorSize();
  }

  const int32_t configured_window_width = REXCVAR_GET(window_width);
  const int32_t configured_window_height = REXCVAR_GET(window_height);
  if (rex::cvar::HasNonDefaultValue("window_width") &&
      rex::cvar::HasNonDefaultValue("window_height") &&
      configured_window_width > 0 && configured_window_height > 0) {
    return DisplaySize{configured_window_width, configured_window_height};
  }

  return std::nullopt;
}

void ApplyUltrawideVideoDefaults() {
  if (!REXCVAR_GET(skate3_ultrawide)) {
    return;
  }

  // Explicitly configured aspect wins; otherwise derive it from the
  // fullscreen monitor (or a configured window size) once at boot.
  double target_aspect =
      rex::cvar::HasNonDefaultValue("skate3_ultrawide_target_aspect")
          ? REXCVAR_GET(skate3_ultrawide_target_aspect)
          : 0.0;
  if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
    const std::optional<DisplaySize> target_size =
        ResolveUltrawideTargetDisplaySize();
    if (!target_size || target_size->width <= 0 || target_size->height <= 0) {
      return;
    }
    target_aspect = static_cast<double>(target_size->width) /
                    static_cast<double>(target_size->height);
    if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
      return;
    }
    rex::cvar::SetFlagByName("skate3_ultrawide_target_aspect",
                             std::to_string(target_aspect));
  }

  // The native renderer draws true wide frames at this aspect (wide guest
  // output + Hor+ projection + centered 2D band). Emulated fallback frames
  // keep the 16:9 guest output and present pillarboxed via the letterbox
  // default below.
  rex::graphics::SetNativeGuestOutputWideAspect(
      std::clamp(target_aspect, kSixteenNineAspect, 8.0));

  if (!rex::cvar::HasNonDefaultValue("present_letterbox")) {
    rex::cvar::SetFlagByName("present_letterbox", "true");
  }
}

void DisableActiveDebugDiagnostics() {
  constexpr std::string_view kFalseFlags[] = {
      "perf_draw_fingerprints",
      "perf_keep_heavyweight_draw_diagnostics",
      "trace_gpu_stream",
  };
  for (std::string_view flag : kFalseFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "false");
  }

  constexpr std::string_view kZeroFlags[] = {
      "vulkan_debug_log_frame_summaries_remaining",
      "vulkan_debug_log_resolve_decisions_remaining",
      "vulkan_debug_log_team_profile_background_candidates_remaining",
      "vulkan_debug_log_team_profile_background_bindings_remaining",
      "filesystem_debug_log_fe_asset_ops_remaining",
      "filesystem_debug_log_team_profile_background_remaining",
  };
  for (std::string_view flag : kZeroFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "0");
  }
}

std::filesystem::path DefaultDocumentsUserRoot() {
  return rex::filesystem::GetUserFolder() / std::string(kUserDirectoryName);
}

std::filesystem::path DefaultRoamingUserRoot() {
#if defined(_WIN32)
  char *appdata = nullptr;
  size_t appdata_size = 0;
  if (_dupenv_s(&appdata, &appdata_size, "APPDATA") == 0 && appdata &&
      *appdata) {
    std::filesystem::path result =
        std::filesystem::path(appdata) / std::string(kUserDirectoryName);
    std::free(appdata);
    return result;
  }
  std::free(appdata);
#endif
  return DefaultDocumentsUserRoot();
}

std::filesystem::path ResolveSkate3UserRoot(const rex::PathConfig &paths) {
  const auto executable_root = rex::filesystem::GetAppRootFolder();
  if (std::filesystem::exists(executable_root / "portable.txt")) {
    return executable_root;
  }

  const auto old_default = DefaultDocumentsUserRoot();
  if (!paths.user_data_root.empty() && paths.user_data_root != old_default) {
    return paths.user_data_root;
  }

  return DefaultRoamingUserRoot();
}

void ConfigureSkate3UserPaths(rex::PathConfig &paths,
                              std::filesystem::path &settings_path,
                              std::filesystem::path &profiles_path) {
  const auto old_user_root = DefaultDocumentsUserRoot();
  const auto old_cache_root = old_user_root / "cache";
  const auto original_cache_root = paths.cache_root;
  const auto resolved_user_root = ResolveSkate3UserRoot(paths);

  paths.user_data_root = resolved_user_root;
  if (original_cache_root.empty() || original_cache_root == old_cache_root) {
    paths.cache_root = resolved_user_root / "cache";
  }
  settings_path = resolved_user_root / std::string(kSettingsFilename);
  profiles_path = skate3::ProfilesFilePath(resolved_user_root);
}

bool ConfigContainsAnyKey(const toml::table &table,
                          std::initializer_list<std::string_view> keys) {
  for (std::string_view key : keys) {
    if (table.contains(key)) {
      return true;
    }
  }
  return false;
}

bool ConfigFileContainsAnyKey(const std::filesystem::path &config_path,
                              std::initializer_list<std::string_view> keys) {
  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return false;
  }
  try {
    auto config = toml::parse_file(config_path.string());
    return ConfigContainsAnyKey(config, keys);
  } catch (const toml::parse_error &) {
    return false;
  }
}

bool DeveloperConfigHasResolutionScaleOverride(
    const std::filesystem::path &config_path) {
  return ConfigFileContainsAnyKey(config_path, {"resolution_scale",
                                                "draw_resolution_scale_x",
                                                "draw_resolution_scale_y"});
}

constexpr int kDefaultResolutionScale = 2;

void ApplyFirstRunVideoDefaults(
    const std::filesystem::path &settings_path,
    const std::filesystem::path &developer_config_path) {
  if (std::filesystem::exists(settings_path) ||
      DeveloperConfigHasResolutionScaleOverride(developer_config_path) ||
      rex::cvar::HasNonDefaultValue("resolution_scale") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_x") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")) {
    return;
  }

  const auto scale = std::to_string(kDefaultResolutionScale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
}

void LoadAndNormalizeSimpleSettings(
    const std::filesystem::path &settings_path,
    const std::filesystem::path &developer_config_path) {
  if (std::filesystem::exists(settings_path)) {
    rex::cvar::LoadConfig(settings_path);
  } else {
    ApplyFirstRunVideoDefaults(settings_path, developer_config_path);
  }
  rex::ui::EnsureSimpleSettingsConfig(settings_path);
}

std::filesystem::path ResolveRuntimeGameDataRoot(const rex::PathConfig &paths) {
  if (!paths.game_data_root.empty()) {
    return paths.game_data_root;
  }

  const auto working_directory_game = std::filesystem::current_path() / "game";
  if (skate3::IsGameInstalled(working_directory_game)) {
    return working_directory_game;
  }

  return paths.config_path.parent_path() / "game";
}

const char *FirstExistingFontPath(std::initializer_list<const char *> paths) {
  for (const char *path : paths) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return nullptr;
}

std::string Hex8(uint32_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
         << value;
  return stream.str();
}

std::filesystem::path
InstalledMarketplaceContentPath(const std::filesystem::path &content_root,
                                uint32_t title_id,
                                const std::filesystem::path &package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "00000002" /
         package_path.filename();
}

std::filesystem::path
InstalledMarketplaceHeaderPath(const std::filesystem::path &content_root,
                               uint32_t title_id,
                               const std::filesystem::path &package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "Headers" /
         "00000002" / (package_path.filename().string() + ".header");
}

bool IsInstalledMarketplaceContent(const std::filesystem::path &content_root,
                                   uint32_t title_id,
                                   const std::filesystem::path &package_path) {
  return std::filesystem::is_directory(InstalledMarketplaceContentPath(
             content_root, title_id, package_path)) &&
         std::filesystem::is_regular_file(InstalledMarketplaceHeaderPath(
             content_root, title_id, package_path));
}

std::vector<std::filesystem::path>
DiscoverDlcSourceDirectories(const std::filesystem::path &executable_root,
                             const std::filesystem::path &game_data_root,
                             const std::filesystem::path &user_data_root) {
  std::vector<std::filesystem::path> dirs;
  auto add_dir = [&](std::filesystem::path dir) {
    if (dir.empty()) {
      return;
    }
    std::error_code ec;
    dir = std::filesystem::absolute(dir, ec);
    if (ec) {
      return;
    }
    for (const auto &existing : dirs) {
      if (std::filesystem::equivalent(existing, dir, ec)) {
        return;
      }
      ec.clear();
    }
    dirs.push_back(std::move(dir));
  };

  const std::string configured_root = REXCVAR_GET(skate3_dlc_root);
  if (!configured_root.empty()) {
    add_dir(configured_root);
  }
  add_dir(executable_root / std::string(kDlcDirectoryName));
  add_dir(game_data_root / std::string(kDlcDirectoryName));
  add_dir(user_data_root / std::string(kDlcDirectoryName));
  return dirs;
}

} // namespace

#if !SKATE3_HAS_TITLE_UPDATE
// Retail sub_82EBAE4C: alternate entry into sub_82EBAE34 (37 vtable refs in
// .data). Body: lwz r3, 0x164(r31); addi r1, r31, 0x140; b __restgprlr_19
static void Sub82EBAE4CImpl(PPCContext &ctx, uint8_t *base) {
  ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0x164);
  ctx.r1.s64 = ctx.r31.s64 + 0x140;
  __restgprlr_19(ctx, base);
}
#endif

Skate3BaseApp::~Skate3BaseApp() = default;

void Skate3BaseApp::OnConfigurePaths(rex::PathConfig &paths) {
  ConfigureSkate3UserPaths(paths, user_settings_path_, profiles_path_);
  maps_path_ = ResolveMapsFolder(paths.user_data_root);
  ConfigureMapsFolder(maps_path_);
  config_path_ = paths.config_path;
  LoadAndNormalizeSimpleSettings(user_settings_path_, config_path_);
  Skate3InitializeFieldOfViewOverride();
  ApplyUltrawideVideoDefaults();
  DisableActiveDebugDiagnostics();
}

void Skate3BaseApp::OnConfigureFonts(ImFontAtlas *atlas) {
  if (!atlas) {
    return;
  }

  atlas->Clear();
  ImFontConfig font_config;
  font_config.OversampleH = 2;
  font_config.OversampleV = 2;
  font_config.PixelSnapH = false;

  const char *base_font_path = FirstExistingFontPath({
#if defined(_WIN32)
      "C:\\Windows\\Fonts\\Helvetica.ttf",
      "C:\\Windows\\Fonts\\helvetica.ttf",
      "C:\\Windows\\Fonts\\HelveticaNeue.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/SFCompact.ttf",
      "/System/Library/Fonts/HelveticaNeue.ttc",
      "/System/Library/Fonts/LucidaGrande.ttc",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/local/share/fonts/NotoSans-Regular.ttf",
      "/usr/local/share/fonts/LiberationSans-Regular.ttf",
#endif
  });

  if (base_font_path) {
    atlas->AddFontFromFileTTF(base_font_path, 16.0f, &font_config,
                              atlas->GetGlyphRangesDefault());
  } else {
    atlas->AddFontDefault();
  }

#if defined(_WIN32)
  const char *jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (std::filesystem::exists(jp_font_path)) {
    ImFontConfig jp_font_config;
    jp_font_config.MergeMode = true;
    jp_font_config.OversampleH = jp_font_config.OversampleV = 1;
    jp_font_config.PixelSnapH = true;
    jp_font_config.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, 16.0f, &jp_font_config,
                              atlas->GetGlyphRangesJapanese());
  }
#endif
}

std::optional<rex::PathConfig>
Skate3BaseApp::OnFinalizePaths(const rex::PathConfig &defaults,
                               std::function<void(rex::PathConfig)> resume) {
  config_path_ = defaults.config_path;
  user_settings_path_ =
      defaults.user_data_root / std::string(kSettingsFilename);
  profiles_path_ = skate3::ProfilesFilePath(defaults.user_data_root);
  maps_path_ = ResolveMapsFolder(defaults.user_data_root);
  ConfigureMapsFolder(maps_path_);

  auto profiles = skate3::LoadProfiles(profiles_path_);
  const bool has_profiles_file = std::filesystem::exists(profiles_path_);
  skate3::EnsureUsableProfileStore(profiles, "Player");
  if (auto *profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    ApplyDemoPathProfileOverride();
  }

  const bool has_config_file = std::filesystem::exists(defaults.config_path);
  const bool has_game_path =
      std::filesystem::is_directory(defaults.game_data_root);
  if (!has_profiles_file && has_config_file && has_game_path) {
    skate3::SaveProfiles(profiles_path_, profiles);
  }
  auto runtime_paths = defaults;
  runtime_paths.game_data_root = ResolveRuntimeGameDataRoot(runtime_paths);
  runtime_paths.config_path.clear();
  if (!skate3::IsGameInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Game files not found at {}; launching rexglue ISO installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__)
    if (const char *automated_iso = std::getenv("SKATE3_INSTALL_ISO");
        automated_iso == nullptr || *automated_iso == '\0') {
#if SKATE3_HAS_TITLE_UPDATE
      // Chain the title update wizard after the ISO install completes.
      auto resume_after_title_update = [this, resume = std::move(resume)](
                                           rex::PathConfig paths) mutable {
        if (!skate3::IsTitleUpdateInstalled(paths.game_data_root)) {
          skate3::ShowTitleUpdateInstallWizard(imgui_drawer(), std::move(paths),
                                               std::move(resume));
          return;
        }
        resume(std::move(paths));
      };
      skate3::ShowRexglueIsoInstallWizard(imgui_drawer(),
                                          std::move(runtime_paths),
                                          std::move(resume_after_title_update));
#else
      skate3::ShowRexglueIsoInstallWizard(
          imgui_drawer(), std::move(runtime_paths), std::move(resume));
#endif
      return std::nullopt;
    }
#endif
    rex::PathConfig installed_paths;
    const bool installed = skate3::RunRexglueIsoInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths,
        installed_paths);
    if (!installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    runtime_paths = std::move(installed_paths);
  }

#if SKATE3_HAS_TITLE_UPDATE
  // This build executes Title Update 3 code; the game cannot boot without the
  // TU payloads staged next to the installed game files. Existing installs
  // from releases that predate TU support land here with the game present but
  // the title update missing.
  if (!skate3::IsTitleUpdateInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Skate 3 Title Update 3 not staged at {}; launching title "
                "update installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__)
    if (const char *automated_tu = std::getenv("SKATE3_INSTALL_TU");
        automated_tu == nullptr || *automated_tu == '\0') {
      skate3::ShowTitleUpdateInstallWizard(
          imgui_drawer(), std::move(runtime_paths), std::move(resume));
      return std::nullopt;
    }
#endif
    rex::PathConfig tu_paths;
    const bool tu_installed = skate3::RunTitleUpdateInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths, tu_paths);
    if (!tu_installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    runtime_paths = std::move(tu_paths);
  }
#endif
#if defined(_WIN32)
  // Window/taskbar + Explorer icon sourced from the user's OWN game art at
  // runtime (game/nxeart); the shipped exe and the repo carry no EA
  // artwork (see skate3_win_icon.h). Game files are guaranteed installed by
  // this point on Windows (the install wizards above run blocking).
  skate3::ApplyGameIconFromGameData(runtime_paths.game_data_root,
                                    window() ? window()->GetNativeWindowHandle()
                                             : nullptr);
#endif
  return runtime_paths;
}

void Skate3BaseApp::OnCreateDialogs(rex::ui::ImGuiDrawer *drawer) {
  // Native/emulated corner readout (top right; off by default, cvar
  // skate3_native_render_mode_indicator shows it live). Input-transparent,
  // so it never affects cursor or focus handling.
  render_mode_indicator_ =
      std::make_unique<skate3::RenderModeIndicator>(drawer);
  map_editor_spawn_dialog_ =
      std::make_unique<skate3::MapEditorSpawnDialog>(drawer);
  auto poll_vanilla_ui_gamepad = [this]() {
    skate3::vanilla_ui::GamepadState pad;
    auto *rt = runtime();
    auto *input_system =
        rt ? static_cast<rex::input::InputSystem *>(rt->input_system())
           : nullptr;
    if (input_system) {
      rex::input::X_INPUT_GAMEPAD state = {};
      if (input_system->GetUiGamepadState(&state)) {
        pad.connected = true;
        pad.buttons = state.buttons;
        pad.thumb_lx = state.thumb_lx;
        pad.thumb_ly = state.thumb_ly;
      }
    }
    return pad;
  };
  auto vanilla_ui_visibility_changed = [this](bool visible) {
    if (visible) {
      // The prototype is a separate surface, but menus are mutually exclusive
      // so Alex's existing screen can continue to own its state unchanged.
      if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
        simple_settings_dialog_->Hide();
      }
      ApplySettingsCursorMode();
      skate3::native_scene::SetSettingsMenuBlur(false);
      skate3::native_scene::SetVanillaUiBackdrop(true);
    } else if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
      ApplySettingsCursorMode();
      skate3::native_scene::SetVanillaUiBackdrop(false);
      skate3::native_scene::SetSettingsMenuBlur(true);
    } else {
      skate3::native_scene::SetVanillaUiBackdrop(false);
      skate3::native_scene::SetSettingsMenuBlur(false);
      ApplyGameplayCursorMode();
    }
  };
  // OnCreateDialogs runs before Runtime is guaranteed to exist. Use the
  // finalized app paths here; dereferencing runtime() at this stage crashes
  // during startup on the normal launch path.
  auto vanilla_ui_asset_cache = user_data_root() / "ui_asset_cache";
  if (const char *override_path = std::getenv("SKATE3_UI_ASSET_CACHE");
      override_path && *override_path) {
    vanilla_ui_asset_cache = override_path;
  }
  std::string vanilla_ui_asset_error;
  if (!skate3::vanilla_ui::EnsureRetailAssetBootstrap(
          game_data_root(), vanilla_ui_asset_cache, vanilla_ui_asset_error)) {
    REXLOG_WARN("Native UI retail asset bootstrap failed: {}",
                vanilla_ui_asset_error);
  }
  vanilla_ui_prototype_dialog_ =
      std::make_unique<skate3::vanilla_ui::PrototypeDialog>(
          drawer, std::move(poll_vanilla_ui_gamepad),
          std::move(vanilla_ui_visibility_changed),
          std::move(vanilla_ui_asset_cache));
  rex::ui::RegisterBind(
      "bind_skate3_menu", "Escape", "Skate 3 settings", [this] {
        if (vanilla_ui_prototype_dialog_ &&
            vanilla_ui_prototype_dialog_->visible()) {
          vanilla_ui_prototype_dialog_->NavigateBack();
          return;
        }
        // Escape backs out of the settings screen level by level (rows ->
        // category rail -> closed); when the screen is closed it opens it.
        if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
          simple_settings_dialog_->NavigateBack();
        } else {
          ToggleSimpleSettings();
        }
      });
  rex::ui::RegisterBind("bind_skate3_menu_alt", "F1",
                        "Skate 3 settings alternate",
                        [this] { ToggleSimpleSettings(); });
  // Remembered handle: the F11 paired A/B parity capture (native + emulated
  // screenshots + gsnap, sequenced from the guest frame loop in
  // skate3_native_render.cpp) needs the window without an app pointer.
  skate3::screenshot::RememberWindow(
      window() ? window()->GetNativeWindowHandle() : nullptr);
  skate3::map_editor::SetWindowHandle(
      window() ? window()->GetNativeWindowHandle() : nullptr);
  rex::ui::RegisterBind("bind_skate3_map_editor", "G",
                        "Toggle in-game map editor", [] {
                          skate3::map_editor::Toggle();
                        });
  rex::ui::RegisterBind("bind_skate3_map_editor_spawn", "E",
                        "Toggle map-editor object list", [] {
                          skate3::map_editor::ToggleSpawnMenu();
                        });
  rex::ui::RegisterBind("bind_skate3_screenshot", "Shift+F6",
                        "Save screenshot to screenshots/", [this] {
                          skate3::screenshot::CaptureWindow(
                              window() ? window()->GetNativeWindowHandle()
                                       : nullptr);
                        });
  // Keep the modified chord registered first: plain keybinds intentionally
  // accept extra modifiers in the shared dispatcher.
  rex::ui::RegisterBind("bind_skate3_vanilla_ui_prototype", "F6",
                        "Skate 3-style native UI prototype",
                        [this] { ToggleVanillaUiPrototype(); });
  rex::ui::RegisterBind("bind_skate3_native_render_toggle", "F5",
                        "Toggle native/emulated renderer",
                        [] { skate3::native_scene::ToggleSceneEnabled(); });
  rex::ui::RegisterBind("bind_skate3_save_draw_fingerprints", "F8",
                        "Save draw fingerprint log",
                        [this] { SaveDrawFingerprintLog(); });
  rex::ui::RegisterBind("bind_skate3_log_debug_marker", "F9",
                        "Write debug marker to log",
                        [this] { LogDebugMarker(); });
  rex::ui::RegisterBind("bind_skate3_log_user_marker", "F10",
                        "Write marker to log", [this] { LogUserMarker(); });
  rex::ui::RegisterBind("bind_skate3_native_debug", "F12",
                        "Native render debug menu",
                        [this] { ToggleNativeDebug(); });
  rex::ui::RegisterBind(
      "bind_skate3_showcase", "Ctrl+Shift+B", "Graphics build-up showcase", [] {
        // A capture/recording tool, not a player feature:
        // like the capture hotkeys, the bind is inert
        // unless the diagnostics cvar opts in (the F12
        // Showcase Setup window still starts runs).
        if (!REXCVAR_GET(skate3_native_render_capture_hotkeys)) {
          return;
        }
        REXCVAR_SET(skate3_native_render_scene_showcase,
                    !REXCVAR_GET(skate3_native_render_scene_showcase));
      });
  rex::ui::RegisterBind(
      "bind_skate3_freecam", "End", "Drone camera (free fly)", [] {
        if (skate3::map_editor::Active()) {
          return;
        }
        REXCVAR_SET(skate3_native_render_scene_freecam,
                    !REXCVAR_GET(skate3_native_render_scene_freecam));
      });
}

void Skate3BaseApp::OnPostSetup() {
  skate3::shader_disasm::RunIfRequested();
  ApplySelectedProfileToRuntime();
  ApplyGameplayCursorMode();
  skate3::map_editor::ConfigureObjectLibrary(
      runtime()->game_data_root(),
      rex::filesystem::GetAppRootFolder() / "objects");
  skate3::multiplayer_assets::StartLocalCatalogue(
      runtime()->game_data_root(), cache_root());
  release_updater_ = std::make_unique<skate3::ReleaseUpdater>(
      rex::filesystem::GetExecutablePath(), SKATE3_VERSION_STRING, [this]() {
        app_context().CallInUIThreadDeferred([this]() {
          if (simple_settings_dialog_) {
            simple_settings_dialog_->Hide();
          }
          if (vanilla_ui_prototype_dialog_) {
            vanilla_ui_prototype_dialog_->Hide();
          }
          if (window()) {
            window()->RequestClose();
          } else {
            app_context().QuitFromUIThread();
          }
          // Large custom collision worlds can make guest teardown linger.
          // The verified update helper is already waiting for this PID, so
          // bound shutdown exactly as map-switch restarts do.
          std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            REXLOG_WARN("Update shutdown watchdog exiting the old process");
            std::_Exit(EXIT_SUCCESS);
          }).detach();
        });
      });

  if (auto *input_system =
          static_cast<rex::input::InputSystem *>(runtime()->input_system())) {
    input_system->SetActiveCallback([this]() {
      const bool settings_visible =
          (simple_settings_dialog_ && simple_settings_dialog_->visible()) ||
          (vanilla_ui_prototype_dialog_ &&
           vanilla_ui_prototype_dialog_->visible());
      const bool xam_ui_active = rex::kernel::xam::xeXamIsUIActive();
      // The drone cam owns the keyboard while flying: keep guest input off
      // so the fly keys don't also steer the skater.
      const bool freecam_captures =
          REXCVAR_GET(skate3_native_render_scene_freecam) &&
          REXCVAR_GET(skate3_native_render_scene_freecam_capture_input);
      return !settings_visible && !xam_ui_active &&
             !freecam_captures && !skate3::map_editor::Active();
    });
    input_system->SetMenuChordCallback([this]() {
      app_context().CallInUIThreadDeferred(
          [this]() { ToggleSimpleSettings(); });
    });
  }

  if (std::getenv("SKATE3_DISABLE_BIG_ALIASES") == nullptr) {
    InstallBigDeviceAliases();
  }
  // Portable saves: a "saves" folder next to the executable takes over
  // saved-game content when it exists (launcher-managed installs). Layout is
  // flattened to saves/<xuid>/<save>; nothing is migrated from the user
  // content tree, and without the folder the default location is unchanged.
  {
    const auto portable_saves =
        rex::filesystem::GetAppRootFolder() / std::string(kSavesDirectoryName);
    if (std::filesystem::is_directory(portable_saves) &&
        runtime()->kernel_state() &&
        runtime()->kernel_state()->content_manager()) {
      runtime()->kernel_state()->content_manager()->SetContentTypeRoot(
          rex::system::XContentType::kSavedGame, portable_saves);
      REXLOG_INFO("Skate 3 saves: using portable folder {}",
                  portable_saves.string());
    }
  }
  InstallDlcPackages();
  InstallRecipeOverlay();

  // Register retail multi-entry-function alternate entries.
#if !SKATE3_HAS_TITLE_UPDATE
  runtime()->function_dispatcher()->SetFunction(0x82EBAE4C, &Sub82EBAE4CImpl);
#endif

  auto *dispatcher = runtime()->function_dispatcher();
  skate3::native_render::Install();
  skate3::demo_path::InstallHooks(dispatcher);
  skate3::custom_trick::InstallHooks(dispatcher);
  skate3::scoring::InstallHooks(dispatcher);
  skate3::input_lab::InstallHooks(dispatcher);
  skate3::vanilla_ui::InstallAudioHooks(dispatcher);
  skate3::input_lab::Install();
  // User-facing intro-movie skip (independent of the demo path): the movie
  // completion override polls the merged UI pad state through this provider.
  skate3::demo_path::SetUiInputProvider([this]() {
    return static_cast<rex::input::InputSystem *>(runtime()->input_system());
  });
  skate3::custom_trick::InstallInputDriver(
      static_cast<rex::input::InputSystem *>(runtime()->input_system()));
  if (dispatcher->InitializeFunctionTable(eawebkit_PPCImageConfig.code_base,
                                          eawebkit_PPCImageConfig.code_size,
                                          eawebkit_PPCImageConfig.image_base,
                                          eawebkit_PPCImageConfig.image_size)) {
    for (int i = 0; eawebkit_PPCImageConfig.func_mappings[i].guest != 0; ++i) {
      auto *host = eawebkit_PPCImageConfig.func_mappings[i].host;
      if (host) {
        dispatcher->SetFunction(
            static_cast<uint32_t>(
                eawebkit_PPCImageConfig.func_mappings[i].guest),
            host);
      }
    }
  }

  // Steam is optional for process startup. Begin low-frequency availability
  // monitoring only after the app and its input system are fully usable.
  skate3::multiplayer::steam::StartAvailabilityMonitor();
}

void Skate3BaseApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_skate3_menu");
  rex::ui::UnregisterBind("bind_skate3_menu_alt");
  rex::ui::UnregisterBind("bind_skate3_vanilla_ui_prototype");
  rex::ui::UnregisterBind("bind_skate3_screenshot");
  rex::ui::UnregisterBind("bind_skate3_save_draw_fingerprints");
  rex::ui::UnregisterBind("bind_skate3_log_debug_marker");
  rex::ui::UnregisterBind("bind_skate3_log_user_marker");
  rex::ui::UnregisterBind("bind_skate3_native_debug");
  rex::ui::UnregisterBind("bind_skate3_map_editor");
  rex::ui::UnregisterBind("bind_skate3_map_editor_spawn");
  ApplyGameplayCursorMode();
  skate3::native_scene::SetVanillaUiBackdrop(false);
  skate3::native_scene::SetSettingsMenuBlur(false);
  skate3::multiplayer::ShutdownSessions();
  skate3::multiplayer_assets::ShutdownLocalCatalogue();
  vanilla_ui_prototype_dialog_.reset();
  simple_settings_dialog_.reset();
  release_updater_.reset();
  native_debug_dialog_.reset();
  render_mode_indicator_.reset();
  map_editor_spawn_dialog_.reset();
}

void Skate3BaseApp::ToggleSimpleSettings() {
  if (vanilla_ui_prototype_dialog_ && vanilla_ui_prototype_dialog_->visible()) {
    vanilla_ui_prototype_dialog_->Hide();
  }
  if (simple_settings_dialog_) {
    if (simple_settings_dialog_->visible()) {
      simple_settings_dialog_->Hide();
    } else {
      ApplySettingsCursorMode();
      skate3::native_scene::SetSettingsMenuBlur(true);
      simple_settings_dialog_->Show();
    }
    return;
  }

  auto load_profiles = [this]() {
    rex::ui::SimpleProfileState state;
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    state.selected_index = 0;
    for (int i = 0; i < static_cast<int>(store.profiles.size()); ++i) {
      const auto &profile = store.profiles[i];
      state.profiles.push_back(
          {profile.id, profile.gamertag, profile.signed_in});
      if (profile.id == store.selected_profile) {
        state.selected_index = i;
      }
    }
    return state;
  };
  auto save_profile = [this](int selected_index, std::string gamertag,
                             bool signed_in) {
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    if (store.profiles.empty()) {
      return;
    }
    selected_index = std::clamp(selected_index, 0,
                                static_cast<int>(store.profiles.size()) - 1);
    auto &profile = store.profiles[selected_index];
    if (!gamertag.empty()) {
      profile.gamertag = std::move(gamertag);
    }
    profile.signed_in = signed_in;
    if (!profile.signed_in) {
      profile.live_signed_in = false;
    }
    store.selected_profile = profile.id;
    skate3::SaveProfiles(profiles_path_, store);
    skate3::ApplyProfileCvars(profile);
    ApplyDemoPathProfileOverride();
    ApplySelectedProfileToRuntime();
  };
  auto active_map_name = [this]() {
    return DiscoverCustomMaps(maps_path_).active_name;
  };
  auto load_multiplayer = [active_map_name](bool refresh) {
    const auto source =
        refresh ? skate3::multiplayer::RefreshServerBrowser(active_map_name())
                : skate3::multiplayer::GetSessionSnapshot(active_map_name());
    rex::ui::SimpleMultiplayerState result;
    result.phase = static_cast<rex::ui::SimpleMultiplayerPhase>(source.phase);
    result.is_host = source.is_host;
    result.steam_available = source.steam_available;
    result.backend_name = source.backend_name;
    result.steam_status = source.steam_status;
    result.status = source.status;
    result.session_name = source.session_name;
    result.host_name = source.host_name;
    result.map_name = source.map_name;
    result.players = static_cast<int>(source.players);
    result.max_players = static_cast<int>(source.max_players);
    for (const auto &server : source.servers) {
      rex::ui::SimpleMultiplayerServerInfo entry;
      entry.id = server.id;
      entry.name = server.name;
      entry.host_name = server.host_name;
      entry.map_name = server.map_name;
      entry.players = static_cast<int>(server.players);
      entry.max_players = static_cast<int>(server.max_players);
      entry.ping_ms = static_cast<int>(server.ping_ms);
      entry.passworded = server.passworded;
      entry.compatible = server.compatible;
      entry.compatibility_note = server.compatibility_note;
      result.servers.push_back(std::move(entry));
    }
    return result;
  };
  auto host_multiplayer =
      [this,
       active_map_name](const rex::ui::SimpleMultiplayerHostSettings &source) {
        auto profiles = skate3::LoadProfiles(profiles_path_);
        skate3::EnsureUsableProfileStore(profiles, "Player");
        std::string host_name = "Player";
        for (const auto &profile : profiles.profiles) {
          if (profile.id == profiles.selected_profile) {
            host_name = profile.gamertag;
            break;
          }
        }
        skate3::multiplayer::HostSettings settings;
        settings.server_name = source.server_name;
        settings.password = source.password;
        settings.host_name = std::move(host_name);
        settings.map_name = active_map_name();
        settings.max_players =
            static_cast<std::uint32_t>(std::clamp(source.max_players, 2, 100));
        settings.privacy =
            static_cast<skate3::multiplayer::SessionPrivacy>(source.privacy);
        settings.allow_late_join = source.allow_late_join;
        skate3::multiplayer::HostSession(settings);
      };
  auto join_multiplayer = [active_map_name](const std::string &server_id,
                                            const std::string &password) {
    skate3::multiplayer::JoinSession(server_id, password, active_map_name());
  };
  auto leave_multiplayer = []() { skate3::multiplayer::LeaveSession(); };
  auto load_maps = [this]() { return DiscoverCustomMaps(maps_path_); };
  auto activate_map = [this](const std::filesystem::path &package_path) {
    std::error_code ec;
    if (!IsSkatePackage(package_path) ||
        !std::filesystem::is_regular_file(package_path, ec) || ec) {
      REXLOG_WARN("Refusing to load missing or invalid custom map path '{}'",
                  package_path.string());
      return;
    }
    const auto header = InspectSkatePackage(package_path);
    if (!header.supported) {
      REXLOG_WARN("Refusing to load incompatible custom map '{}': {}",
                  package_path.string(), header.issue);
      return;
    }
    if (!SaveSelectedMap(maps_path_, package_path)) {
      REXLOG_WARN("Could not save selected custom map '{}'",
                  package_path.string());
      return;
    }
    SetEnvironmentMapPath(package_path);
    REXLOG_INFO("Custom map selected; restarting into '{}'",
                package_path.string());
    RestartGame();
  };
  auto open_maps_folder = [this]() { OpenFolderInFileManager(maps_path_); };
  auto load_world_lighting = []() {
    const auto source =
        skate3::mechanics_sandbox::map::ActiveWorldLightingSettings();
    rex::ui::SimpleWorldLightingState state;
    state.available = source.available;
    state.paused = source.paused;
    state.ping_pong = source.ping_pong;
    state.dynamic_lighting_enabled = source.dynamic_lighting_enabled;
    state.time_of_day_hours = source.time_of_day_hours;
    state.cycle_duration_seconds = source.cycle_duration_seconds;
    state.start_hour = source.start_hour;
    state.end_hour = source.end_hour;
    state.orbit_azimuth_degrees = source.orbit_azimuth_degrees;
    state.sky_red = source.sky_red;
    state.sky_green = source.sky_green;
    state.sky_blue = source.sky_blue;
    state.sunlight_red = source.sunlight_red;
    state.sunlight_green = source.sunlight_green;
    state.sunlight_blue = source.sunlight_blue;
    state.sun_intensity = source.sun_intensity;
    state.moon_intensity = source.moon_intensity;
    state.day_ambient = source.day_ambient;
    state.night_ambient = source.night_ambient;
    return state;
  };
  auto update_world_lighting = [](rex::ui::SimpleWorldLightingField field,
                                  float value) {
    using RuntimeField = skate3::mechanics_sandbox::map::WorldLightingSetting;
    skate3::mechanics_sandbox::map::SetWorldLightingSetting(
        static_cast<RuntimeField>(field), value);
  };
  auto reset_world_lighting = []() {
    skate3::mechanics_sandbox::map::ResetWorldLightingSettings();
  };
  auto load_update_state = [this]() {
    rex::ui::SimpleUpdateState result;
    if (!release_updater_) {
      result.phase = rex::ui::SimpleUpdatePhase::kUnsupported;
      result.status = "The release updater is unavailable in this session.";
      return result;
    }
    const auto state = release_updater_->state();
    result.phase = static_cast<rex::ui::SimpleUpdatePhase>(state.phase);
    result.current_version = state.current_version;
    result.latest_version = state.latest_version;
    result.status = state.status;
    result.progress = state.progress;
    return result;
  };
  auto start_update = [this]() {
    if (release_updater_) {
      release_updater_->Start();
    }
  };
  // Fires on every Hide (B/Esc, Close Settings, Close Game), the one spot
  // that reliably sees the menu close regardless of who initiated it.
  auto close_settings = [this]() {
    skate3::native_scene::SetSettingsMenuBlur(false);
    ApplyGameplayCursorMode();
  };
  auto close_game = [this]() {
#if REX_PLATFORM_MAC || REX_PLATFORM_LINUX
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      REXLOG_WARN("Close Game watchdog exiting process after shutdown timeout");
      std::_Exit(EXIT_SUCCESS);
    }).detach();
#endif
    app_context().CallInUIThreadDeferred([this]() {
#if REX_PLATFORM_MAC
      app_context().QuitFromUIThread();
#else
      if (window()) {
        window()->RequestClose();
      } else {
        app_context().QuitFromUIThread();
      }
#endif
    });
  };
  auto restart_game = [this]() { RestartGame(); };
  // Raw pad poll for menu navigation: bypasses the is_active gate that
  // zeroes guest-facing input while the settings screen is open.
  auto poll_gamepad = [this]() {
    rex::ui::SimpleSettingsGamepad pad;
    // The dialog can be painted (and thus poll) during a frame that races
    // runtime teardown at close time, when runtime() is already gone.
    auto *rt = runtime();
    auto *input_system =
        rt ? static_cast<rex::input::InputSystem *>(rt->input_system())
           : nullptr;
    if (input_system) {
      rex::input::X_INPUT_GAMEPAD state;
      if (input_system->GetUiGamepadState(&state)) {
        pad.connected = true;
        pad.buttons = state.buttons;
        pad.thumb_lx = state.thumb_lx;
        pad.thumb_ly = state.thumb_ly;
      }
    }
    return pad;
  };
  simple_settings_dialog_ = std::make_unique<rex::ui::SimpleSettingsDialog>(
      imgui_drawer(), user_settings_path_, std::move(load_profiles),
      std::move(save_profile), std::move(load_multiplayer),
      std::move(host_multiplayer), std::move(join_multiplayer),
      std::move(leave_multiplayer), std::move(load_maps),
      std::move(activate_map), std::move(open_maps_folder),
      std::move(load_world_lighting), std::move(update_world_lighting),
      std::move(reset_world_lighting), std::move(load_update_state),
      std::move(start_update), std::move(close_settings), std::move(close_game),
      std::move(restart_game), std::move(poll_gamepad));
  ApplySettingsCursorMode();
  skate3::native_scene::SetSettingsMenuBlur(true);
  simple_settings_dialog_->Show();
}

void Skate3BaseApp::ToggleVanillaUiPrototype() {
  if (!vanilla_ui_prototype_dialog_) {
    return;
  }
  vanilla_ui_prototype_dialog_->Toggle();
}

void Skate3BaseApp::ToggleNativeDebug() {
  if (!native_debug_dialog_) {
    native_debug_dialog_ =
        std::make_unique<skate3::NativeDebugDialog>(imgui_drawer());
  }
  if (native_debug_dialog_->visible()) {
    native_debug_dialog_->Hide();
    ApplyGameplayCursorMode();
  } else {
    ApplySettingsCursorMode();
    native_debug_dialog_->Show();
  }
}

void Skate3BaseApp::ApplySettingsCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
}

void Skate3BaseApp::ApplyGameplayCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(
        rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

void Skate3BaseApp::RestartGame() {
  app_context().CallInUIThreadDeferred([this]() {
#if defined(_WIN32)
    wchar_t executable_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) {
      REXLOG_WARN(
          "Restart requested, but the executable path could not be resolved");
      return;
    }

    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(executable_path, command_line.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup_info,
                        &process_info)) {
      REXLOG_WARN("Restart requested, but launching a new process failed");
      return;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    // Guest teardown can stall after a large owned world has installed more
    // than a million collision triangles. The replacement process is already
    // independent at this point, so bound the old process lifetime instead of
    // leaving a multi-gigabyte zombie behind after every map switch.
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      REXLOG_WARN("Restart shutdown watchdog exiting the replaced process");
      std::_Exit(EXIT_SUCCESS);
    }).detach();
#elif defined(__linux__) || defined(__APPLE__)
    const auto executable_path = rex::filesystem::GetExecutablePath();
    if (executable_path.empty()) {
      REXLOG_WARN(
          "Restart requested, but the executable path could not be resolved");
      return;
    }

    auto args = CurrentProcessArgumentsForRestart(executable_path);
    std::filesystem::path restart_game_data_root;
    if (runtime()) {
      restart_game_data_root = runtime()->game_data_root();
    }
    if (restart_game_data_root.empty()) {
      restart_game_data_root = game_data_root();
    }
    if (!restart_game_data_root.empty()) {
      SetRestartArgument(args, "game_data_root",
                         rex::path_to_utf8(restart_game_data_root));
    }
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto &arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const std::string executable = rex::path_to_utf8(executable_path);
    pid_t child_pid = 0;
    const int spawn_result = posix_spawn(
        &child_pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0) {
      REXLOG_WARN("Restart requested, but launching a new process failed: {}",
                  std::strerror(spawn_result));
      return;
    }
#else
    REXLOG_WARN("Restart requested, but automatic restart is not implemented "
                "on this platform");
    return;
#endif

#if REX_PLATFORM_MAC
    app_context().QuitFromUIThread();
#else
    if (window()) {
      window()->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
#endif
  });
}

void Skate3BaseApp::SaveDrawFingerprintLog() {
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  REXLOG_WARN("Perf capture is unavailable because perf counters are disabled "
              "in this build");
  return;
#else
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif

  std::ostringstream counters_filename;
  counters_filename << "perf_capture_counters_"
                    << std::put_time(&local_time, "%Y%m%d_%H%M%S") << ".csv";
  std::ostringstream filename;
  filename << "perf_capture_draw_fingerprints_"
           << std::put_time(&local_time, "%Y%m%d_%H%M%S") << ".csv";

  std::error_code ec;
  std::filesystem::create_directories(cache_root(), ec);
  const auto counters_path = cache_root() / counters_filename.str();
  const auto path = cache_root() / filename.str();
  if (rex::perf::StartCapture(counters_path, path)) {
    REXLOG_INFO("Started perf capture; counter log will be saved to {}",
                counters_path.string());
    REXLOG_INFO(
        "Started perf capture; draw fingerprint log will be saved to {}",
        path.string());
  } else {
    REXLOG_WARN("Perf capture is already running");
  }
#endif
}

void Skate3BaseApp::LogUserMarker() {
  const uint32_t marker =
      debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("USER LOG MARKER #{}: F10 pressed", marker);
}

void Skate3BaseApp::LogDebugMarker() {
  const uint32_t marker =
      debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  DisableActiveDebugDiagnostics();
  REXLOG_WARN("USER DEBUG MARKER #{}: F9 pressed; active diagnostics disabled",
              marker);
}

void Skate3BaseApp::ApplySelectedProfileToRuntime() {
  if (!runtime() || !runtime()->kernel_state() ||
      !runtime()->kernel_state()->user_profile()) {
    return;
  }
  auto profiles = skate3::LoadProfiles(profiles_path_);
  if (auto *profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    ApplyDemoPathProfileOverride();
    runtime()->kernel_state()->user_profile()->SetIdentity(profile->xuid,
                                                           profile->gamertag);
  }
}

bool Skate3BaseApp::IsRecipeNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::set<std::string> Skate3BaseApp::DiscoverRecipeAliases(
    const std::filesystem::path &content_root) {
  static constexpr size_t kScanBytes = 8 * 1024 * 1024;
  static constexpr std::string_view kPrefix = "data/content/recipe/";

  std::set<std::string> aliases;
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(content_root, ec)) {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".big") {
      continue;
    }

    std::ifstream stream(entry.path(), std::ios::binary);
    if (!stream) {
      continue;
    }

    std::string buffer;
    buffer.resize(kScanBytes);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(stream.gcount()));

    size_t pos = 0;
    while ((pos = buffer.find(kPrefix, pos)) != std::string::npos) {
      size_t name_start = pos + kPrefix.size();
      size_t name_end = name_start;
      while (name_end < buffer.size() && IsRecipeNameChar(buffer[name_end])) {
        ++name_end;
      }
      if (name_end > name_start) {
        aliases.insert(buffer.substr(name_start, name_end - name_start));
      }
      pos = name_end;
    }
  }

  return aliases;
}

bool Skate3BaseApp::CreateOverlayDirectory(
    const std::filesystem::path &overlay_root, std::string_view guest_path) {
  std::filesystem::path path = overlay_root;
  size_t segment_start = 0;
  while (segment_start < guest_path.size()) {
    const size_t slash = guest_path.find('/', segment_start);
    const size_t segment_end =
        slash == std::string_view::npos ? guest_path.size() : slash;
    if (segment_end > segment_start) {
      path /= std::string(
          guest_path.substr(segment_start, segment_end - segment_start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    segment_start = slash + 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

void Skate3BaseApp::InstallRecipeOverlay() {
  if (recipe_overlay_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  const auto content_root = runtime()->game_data_root() / "data" / "content";
  if (!std::filesystem::exists(content_root)) {
    REXLOG_WARN(
        "Skipping Skate 3 recipe VFS overlay; content root not found: {}",
        content_root.string());
    return;
  }

  const auto aliases = DiscoverRecipeAliases(content_root);
  if (aliases.empty()) {
    REXLOG_WARN(
        "Skipping Skate 3 recipe VFS overlay; no recipe aliases found in {}",
        content_root.string());
    return;
  }

  const auto overlay_root = cache_root() / "vfs_big_directory_aliases";
  std::error_code ec;
  std::filesystem::create_directories(overlay_root, ec);
  if (ec) {
    REXLOG_WARN(
        "Skipping Skate 3 BIG-directory VFS overlay; failed to create {}: {}",
        overlay_root.string(), ec.message());
    return;
  }

  size_t created = 0;
  for (const auto &alias : aliases) {
    const std::string guest_path = std::string("data/content/recipe/") + alias;
    if (CreateOverlayDirectory(overlay_root, guest_path)) {
      ++created;
    }
  }

  static constexpr std::string_view kBigDirectoryAliases[] = {
      "data/scene",
      "data/scene/anim",
      "data/scene/trickguide",
      "data/livingworld/PluginDescriptor",
      "data/state/livingworldentities/pedestrian/plugin",
  };
  for (std::string_view alias : kBigDirectoryAliases) {
    if (CreateOverlayDirectory(overlay_root, alias)) {
      ++created;
    }
  }

  if (!created) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create "
                "alias directories in {}",
                overlay_root.string());
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      "skate3bigdirs:", overlay_root, true);
  if (!device->Initialize()) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to "
                "initialize host device for {}",
                overlay_root.string());
    return;
  }

  runtime()->file_system()->RegisterDevice(std::move(device));
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\content\\recipe",
      "skate3bigdirs:\\data\\content\\recipe");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\scene",
      "skate3bigdirs:\\data\\scene");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\livingworld\\PluginDescriptor",
      "skate3bigdirs:\\data\\livingworld\\PluginDescriptor");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\state\\livingworldentities\\pedes"
      "trian\\plugin",
      "skate3bigdirs:\\data\\state\\livingworldentities\\pedestrian\\plugin");
  recipe_overlay_installed_ = true;
  REXLOG_INFO(
      "Installed Skate 3 BIG-directory VFS overlay with {} aliases from {}",
      created, content_root.string());
}

void Skate3BaseApp::InstallBigDeviceAliases() {
  if (big_device_aliases_installed_ || !runtime() ||
      !runtime()->file_system()) {
    return;
  }

  // Skate 3 uses these file-server style schemes for world content probes.
  // Route them to the normal title mount so existing disc files resolve.
  runtime()->file_system()->RegisterSymbolicLink(
      "big:", "\\Device\\Harddisk0\\Partition1");
  runtime()->file_system()->RegisterSymbolicLink(
      "dlcbig:", "\\Device\\Harddisk0\\Partition1");
  big_device_aliases_installed_ = true;
}

void Skate3BaseApp::InstallDlcPackages() {
  if (!REXCVAR_GET(skate3_auto_install_dlc) || !runtime() ||
      !runtime()->kernel_state() ||
      !runtime()->kernel_state()->content_manager()) {
    return;
  }

  const uint32_t title_id = runtime()->kernel_state()->title_id();
  if (title_id == 0) {
    REXLOG_WARN("Skipping Skate 3 DLC install; title ID is not available");
    return;
  }

  const auto user_dlc_root =
      runtime()->user_data_root() / std::string(kDlcDirectoryName);
  std::error_code create_ec;
  std::filesystem::create_directories(user_dlc_root, create_ec);
  if (create_ec) {
    REXLOG_WARN("Could not create Skate 3 DLC drop folder {}: {}",
                user_dlc_root.string(), create_ec.message());
  }

  const auto source_dirs = DiscoverDlcSourceDirectories(
      rex::filesystem::GetAppRootFolder(), runtime()->game_data_root(),
      runtime()->user_data_root());
  std::unordered_set<std::string> seen_packages;
  size_t installed_count = 0;
  size_t skipped_count = 0;

  for (const auto &source_dir : source_dirs) {
    if (!std::filesystem::is_directory(source_dir)) {
      continue;
    }

    std::error_code iter_ec;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(source_dir, iter_ec)) {
      if (iter_ec) {
        REXLOG_WARN("Could not scan Skate 3 DLC folder {}: {}",
                    source_dir.string(), iter_ec.message());
        break;
      }
      if (!entry.is_regular_file()) {
        continue;
      }

      const auto package_path = entry.path();
      std::error_code canonical_ec;
      auto package_key =
          std::filesystem::weakly_canonical(package_path, canonical_ec)
              .string();
      if (canonical_ec) {
        package_key = std::filesystem::absolute(package_path).string();
      }
      if (!seen_packages.insert(package_key).second) {
        continue;
      }

      const auto header =
          rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
      if (!header) {
        REXLOG_WARN("Skipping DLC candidate with invalid STFS header: {}",
                    package_path.string());
        ++skipped_count;
        continue;
      }

      const auto content_type =
          static_cast<rex::system::XContentType>(header->metadata.content_type);
      if (content_type != rex::system::XContentType::kMarketplaceContent) {
        REXLOG_WARN("Skipping non-DLC content package {} with type {:08X}",
                    package_path.filename().string(),
                    static_cast<uint32_t>(content_type));
        ++skipped_count;
        continue;
      }

      const uint32_t package_title_id =
          header->metadata.execution_info.title_id;
      if (package_title_id != 0 && package_title_id != title_id) {
        REXLOG_WARN(
            "Skipping DLC package {} for title {:08X}; running title is {:08X}",
            package_path.filename().string(), package_title_id, title_id);
        ++skipped_count;
        continue;
      }

      if (IsInstalledMarketplaceContent(runtime()->user_data_root(), title_id,
                                        package_path)) {
        REXLOG_INFO("DLC package already installed: {}",
                    package_path.filename().string());
        ++skipped_count;
        continue;
      }

      REXLOG_INFO("Installing Skate 3 DLC package: {}", package_path.string());
      const auto result =
          runtime()->kernel_state()->content_manager()->InstallContent(
              package_path);
      if (result == 0) {
        ++installed_count;
        REXLOG_INFO("Installed Skate 3 DLC package: {}",
                    package_path.filename().string());
      } else {
        ++skipped_count;
        REXLOG_WARN("Failed to install Skate 3 DLC package {}: {:08X}",
                    package_path.filename().string(),
                    static_cast<uint32_t>(result));
      }
    }
  }

  if (installed_count || skipped_count) {
    REXLOG_INFO("Skate 3 DLC scan complete: {} installed, {} skipped",
                installed_count, skipped_count);
  } else {
    REXLOG_INFO("No Skate 3 DLC packages found. Drop legally obtained DLC "
                "package files in {}",
                user_dlc_root.string());
  }

  const auto installed_marketplace =
      runtime()->kernel_state()->content_manager()->ListContent(
          static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD), 0,
          rex::system::XContentType::kMarketplaceContent, title_id);
  REXLOG_INFO(
      "Skate 3 installed Marketplace content audit: {} package(s) under {}",
      installed_marketplace.size(), runtime()->user_data_root().string());
  for (const auto &content : installed_marketplace) {
    REXLOG_INFO("Skate 3 installed Marketplace package: {}",
                content.file_name());
  }
}
