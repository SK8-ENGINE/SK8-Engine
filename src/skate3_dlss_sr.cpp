#include "skate3_dlss_sr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(skate3_dlss_mode, 0, "Skate 3",
                     "NVIDIA DLSS Super Resolution: 0=Off, 1=Quality, "
                     "2=Balanced, 3=Performance, 4=DLAA")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_dlss_status, "Off", "Skate 3",
                      "Read-only DLSS Super Resolution capability and sizing");
REXCVAR_DEFINE_BOOL(skate3_dlss_neural_rendering, false, "Skate 3",
                    "DLSS 5 Neural Rendering post-pass (private preview)");
REXCVAR_DEFINE_DOUBLE(skate3_dlss_nr_intensity, 1.0, "Skate 3",
                      "DLSS Neural Rendering intensity")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_dlss_nr_local_tone, 1.0, "Skate 3",
                      "DLSS Neural Rendering local tone strength")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_dlss_nr_local_structure, 1.0, "Skate 3",
                      "DLSS Neural Rendering local structure strength")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_dlss_nr_global_tone, 1.0, "Skate 3",
                      "DLSS Neural Rendering global tone strength")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_dlss_nr_skin_structure, 1.0, "Skate 3",
                      "DLSS Neural Rendering skin structure strength")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_dlss_nr_auto_mask, false, "Skate 3",
                    "DLSS Neural Rendering automatic control mask");
REXCVAR_DEFINE_INT32(skate3_dlss_nr_style, 0, "Skate 3",
                     "DLSS Neural Rendering style (preview SDK enum)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_dlss_nr_preset, 0, "Skate 3",
                     "DLSS Neural Rendering preset (preview SDK enum)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_dlss_nr_performance_mode, 3, "Skate 3",
                     "DLSS Neural Rendering performance mode (preview SDK enum)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_dlss_nr_status, "Off", "Skate 3",
                      "Read-only DLSS Neural Rendering capability");

#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <windows.h>

#include <sl.h>
#include <sl_dlss.h>
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
#include "skate3_dlss_nr_private.h"
#endif
#endif

namespace skate3::dlss {
namespace {

std::mutex g_mutex;
Status g_status;
HistoryTracker g_history;
std::uint32_t g_jitter_index = 0;
std::uint32_t g_frame_index = 0;
bool g_evaluation_failed = false;

NeuralSettings ReadNeuralSettings() {
  NeuralSettings settings;
  settings.enabled =
      rex::cvar::Query<bool>("skate3_dlss_neural_rendering");
  settings.intensity = ClampNeuralStrength(
      rex::cvar::Query<double>("skate3_dlss_nr_intensity"));
  settings.local_tone_strength = ClampNeuralStrength(
      rex::cvar::Query<double>("skate3_dlss_nr_local_tone"));
  settings.local_structure_strength = ClampNeuralStrength(
      rex::cvar::Query<double>("skate3_dlss_nr_local_structure"));
  settings.global_tone_strength = ClampNeuralStrength(
      rex::cvar::Query<double>("skate3_dlss_nr_global_tone"));
  settings.skin_structure_strength = ClampNeuralStrength(
      rex::cvar::Query<double>("skate3_dlss_nr_skin_structure"));
  settings.use_auto_mask =
      rex::cvar::Query<bool>("skate3_dlss_nr_auto_mask");
  settings.style =
      ClampNeuralStyle(rex::cvar::Query<std::int32_t>("skate3_dlss_nr_style"));
  settings.preset = ClampNeuralPreset(
      rex::cvar::Query<std::int32_t>("skate3_dlss_nr_preset"));
  settings.performance_mode = ClampNeuralPerformanceMode(
      rex::cvar::Query<std::int32_t>("skate3_dlss_nr_performance_mode"));
  return settings;
}

void PublishStatusLocked() {
  std::string value;
  if (g_status.selected_mode == Mode::kOff) {
    if (g_status.supported) {
      value = "Off (available)";
    } else if (g_status.initialized &&
               g_status.unavailable_reason == UnavailableReason::kNone) {
      value = "Off (capability checked when selected)";
    } else {
      value = std::string(ReasonText(g_status.unavailable_reason));
    }
  } else if (g_status.supported &&
             g_status.unavailable_reason == UnavailableReason::kNone &&
             g_status.render.valid()) {
    value = std::format("{}: {}x{} -> {}x{}", ModeName(g_status.selected_mode),
                        g_status.render.width, g_status.render.height,
                        g_status.output.width, g_status.output.height);
  } else {
    value = std::format("{} unavailable: {}", ModeName(g_status.selected_mode),
                        ReasonText(g_status.unavailable_reason));
  }
  if (!g_status.detail.empty()) {
    value += " (" + g_status.detail + ")";
  }
  rex::cvar::SetFlagByName("skate3_dlss_status", value);

  std::string neural;
  if (!g_status.neural_requested) {
    neural = g_status.neural_supported ? "Off (available)" : "Off";
  } else if (g_status.selected_mode == Mode::kOff) {
    neural = "Unavailable: enable DLSS Super Resolution or DLAA first";
  } else if (g_status.neural_active) {
    neural = std::format("On: {}x{} pre-tonemap HDR", g_status.output.width,
                         g_status.output.height);
  } else if (!g_status.neural_detail.empty()) {
    neural = "Unavailable: " + g_status.neural_detail;
  } else {
    neural = "Unavailable";
  }
  rex::cvar::SetFlagByName("skate3_dlss_nr_status", neural);
}

#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
struct Api {
  HMODULE module = nullptr;
  PFun_slInit *init = nullptr;
  PFun_slShutdown *shutdown = nullptr;
  PFun_slIsFeatureSupported *is_feature_supported = nullptr;
  PFun_slEvaluateFeature *evaluate_feature = nullptr;
  PFun_slFreeResources *free_resources = nullptr;
  PFun_slGetFeatureVersion *get_feature_version = nullptr;
  PFun_slSetTagForFrame *set_tag_for_frame = nullptr;
  PFun_slSetConstants *set_constants = nullptr;
  PFun_slGetFeatureFunction *get_feature_function = nullptr;
  PFun_slGetNewFrameToken *get_new_frame_token = nullptr;
  PFun_slSetD3DDevice *set_d3d_device = nullptr;
  PFun_slDLSSGetOptimalSettings *get_optimal_settings = nullptr;
  PFun_slDLSSSetOptions *set_options = nullptr;
  PFun_slDLSSGetState *get_state = nullptr;
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  sl::PFun_slDLSSNRSetOptions *set_neural_options = nullptr;
#endif
};

Api g_api;
sl::ViewportHandle g_viewport{0u};
ID3D12Device *g_device = nullptr;
Mode g_configured_mode = Mode::kOff;
RenderSize g_configured_output{};
sl::DLSSOptions g_options{};
sl::FrameToken *g_frame_token = nullptr;
std::chrono::steady_clock::time_point g_last_state_query{};
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
sl::DLSSNROptions g_neural_options{};
NeuralSettings g_configured_neural_settings{};
bool g_neural_options_configured = false;
bool g_neural_evaluation_failed = false;
std::chrono::steady_clock::time_point g_last_neural_log{};
#endif

void RecordEvaluationFailureLocked(UnavailableReason reason,
                                   sl::Result result) {
  ++g_status.evaluation_failures;
  g_status.unavailable_reason = reason;
  g_evaluation_failed = true;
  if ((g_status.evaluation_failures & 63u) == 1u) {
    REXLOG_ERROR("DLSS SR: frame evaluation disabled until mode is set Off "
                 "(stage={}, result={})",
                 ReasonText(reason), static_cast<int>(result));
  }
  PublishStatusLocked();
}

template <typename T>
bool Import(HMODULE module, const char *name, T *&output) {
  output = reinterpret_cast<T *>(GetProcAddress(module, name));
  return output != nullptr;
}

sl::DLSSMode ToSlMode(Mode mode) {
  switch (mode) {
  case Mode::kQuality:
    return sl::DLSSMode::eMaxQuality;
  case Mode::kBalanced:
    return sl::DLSSMode::eBalanced;
  case Mode::kPerformance:
    return sl::DLSSMode::eMaxPerformance;
  case Mode::kDlaa:
    return sl::DLSSMode::eDLAA;
  case Mode::kOff:
    return sl::DLSSMode::eOff;
  }
  return sl::DLSSMode::eOff;
}

UnavailableReason MapSupportFailure(sl::Result result) {
  switch (result) {
  case sl::Result::eErrorDriverOutOfDate:
    return UnavailableReason::kDriver;
  case sl::Result::eErrorOSOutOfDate:
  case sl::Result::eErrorOSDisabledHWS:
    return UnavailableReason::kOperatingSystem;
  case sl::Result::eErrorNoSupportedAdapterFound:
    return UnavailableReason::kNonNvidia;
  case sl::Result::eErrorAdapterNotSupported:
  case sl::Result::eErrorFeatureNotSupported:
    return UnavailableReason::kUnsupportedGpu;
  case sl::Result::eErrorFeatureMissing:
  case sl::Result::eErrorFeatureFailedToLoad:
  case sl::Result::eErrorNoPlugins:
    return UnavailableReason::kFeatureNotLoaded;
  default:
    return UnavailableReason::kInitialization;
  }
}

void CopyMatrix(const std::array<float, 16> &source, sl::float4x4 &target) {
  for (std::uint32_t row = 0; row < 4; ++row) {
    target[row] = {source[row * 4], source[row * 4 + 1], source[row * 4 + 2],
                   source[row * 4 + 3]};
  }
}

bool ConfigureViewportLocked(Mode mode, RenderSize output, RenderSize &render) {
  g_options = {};
  g_options.mode = ToSlMode(mode);
  g_options.outputWidth = output.width;
  g_options.outputHeight = output.height;
  g_options.colorBuffersHDR = sl::Boolean::eTrue;
  g_options.useAutoExposure = sl::Boolean::eTrue;
  g_options.alphaUpscalingEnabled = sl::Boolean::eFalse;
  g_options.dlaaPreset = sl::DLSSPreset::ePresetK;
  g_options.qualityPreset = sl::DLSSPreset::ePresetK;
  g_options.balancedPreset = sl::DLSSPreset::ePresetK;
  g_options.performancePreset = sl::DLSSPreset::ePresetM;

  sl::DLSSOptimalSettings optimal{};
  if (g_api.get_optimal_settings(g_options, optimal) != sl::Result::eOk ||
      optimal.optimalRenderWidth == 0 || optimal.optimalRenderHeight == 0) {
    g_status.unavailable_reason = UnavailableReason::kOptimalSettings;
    return false;
  }
  render = {optimal.optimalRenderWidth, optimal.optimalRenderHeight};
  if (g_api.set_options(g_viewport, g_options) != sl::Result::eOk) {
    g_status.unavailable_reason = UnavailableReason::kInitialization;
    return false;
  }
  g_configured_mode = mode;
  g_configured_output = output;
  return true;
}
#endif

} // namespace

void InitializeEarly(const std::filesystem::path &log_directory) {
  std::lock_guard lock(g_mutex);
  g_status = {};
  g_status.unavailable_reason = UnavailableReason::kBuildDisabled;
  g_status.neural_requested = ReadNeuralSettings().enabled;
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  std::error_code directory_error;
  std::filesystem::create_directories(log_directory, directory_error);
  wchar_t executable[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    g_status.unavailable_reason = UnavailableReason::kInitialization;
    PublishStatusLocked();
    return;
  }
  const std::filesystem::path directory =
      std::filesystem::path(executable).parent_path();
  constexpr const char *project_id = SKATE3_NVIDIA_PROJECT_ID;
  if (SKATE3_NVIDIA_APPLICATION_ID == 0 && project_id[0] == '\0') {
    g_status.unavailable_reason = UnavailableReason::kIdentity;
    PublishStatusLocked();
    return;
  }
  for (const wchar_t *name : {L"sl.interposer.dll", L"sl.common.dll",
                              L"sl.dlss.dll", L"nvngx_dlss.dll"}) {
    if (!std::filesystem::exists(directory / name)) {
      g_status.unavailable_reason =
          std::wstring_view(name) == L"sl.interposer.dll"
              ? UnavailableReason::kMissingInterposer
              : UnavailableReason::kMissingPlugin;
      g_status.detail = std::filesystem::path(name).string();
      PublishStatusLocked();
      return;
    }
  }
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  g_status.neural_plugin_present =
      std::filesystem::exists(directory / L"sl.dlss_nr.dll") &&
      std::filesystem::exists(directory / L"nvngx_dlssnr.dll");
  if (!g_status.neural_plugin_present) {
    g_status.neural_detail =
        "sl.dlss_nr.dll or nvngx_dlssnr.dll is absent";
  }
#else
  g_status.neural_detail = "This build does not include the private preview";
#endif
  const auto interposer = directory / L"sl.interposer.dll";
  g_api.module = LoadLibraryExW(interposer.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (g_api.module == nullptr || !Import(g_api.module, "slInit", g_api.init) ||
      !Import(g_api.module, "slShutdown", g_api.shutdown) ||
      !Import(g_api.module, "slIsFeatureSupported",
              g_api.is_feature_supported) ||
      !Import(g_api.module, "slEvaluateFeature", g_api.evaluate_feature) ||
      !Import(g_api.module, "slFreeResources", g_api.free_resources) ||
      !Import(g_api.module, "slGetFeatureVersion", g_api.get_feature_version) ||
      !Import(g_api.module, "slSetTagForFrame", g_api.set_tag_for_frame) ||
      !Import(g_api.module, "slSetConstants", g_api.set_constants) ||
      !Import(g_api.module, "slGetFeatureFunction",
              g_api.get_feature_function) ||
      !Import(g_api.module, "slGetNewFrameToken", g_api.get_new_frame_token) ||
      !Import(g_api.module, "slSetD3DDevice", g_api.set_d3d_device)) {
    g_status.unavailable_reason = UnavailableReason::kInitialization;
    g_status.detail = "Streamline core exports";
    PublishStatusLocked();
    return;
  }

  std::array<sl::Feature, 2> features{sl::kFeatureDLSS, 0};
  std::uint32_t feature_count = 1;
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  if (g_status.neural_plugin_present) {
    features[feature_count++] = sl::kFeatureDLSS_NR;
  }
#endif
  const std::wstring plugin_path = directory.wstring();
  const wchar_t *plugin_paths[] = {plugin_path.c_str()};
  const std::wstring log_path = log_directory.wstring();
  sl::Preferences preferences{};
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = log_path.c_str();
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eDisableDebugText |
                      sl::PreferenceFlags::eUseManualHooking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.featuresToLoad = features.data();
  preferences.numFeaturesToLoad = feature_count;
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = SKATE3_DLSS_ENGINE_VERSION;
  preferences.projectId = project_id[0] != '\0' ? project_id : nullptr;
  preferences.applicationId = SKATE3_NVIDIA_APPLICATION_ID;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  constexpr std::uint64_t streamline_sdk_version =
      sl::kSDKVersionDLSSNRPreview;
#else
  constexpr std::uint64_t streamline_sdk_version = sl::kSDKVersion;
#endif
  if (g_api.init(preferences, streamline_sdk_version) != sl::Result::eOk) {
    g_status.unavailable_reason = UnavailableReason::kInitialization;
    PublishStatusLocked();
    return;
  }
  g_status.initialized = true;
  g_status.unavailable_reason = UnavailableReason::kNone;
  g_status.detail.clear();
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  g_status.streamline_version = "2.13.0";
#else
  g_status.streamline_version = "2.12.0";
#endif
  REXLOG_INFO("DLSS SR: Streamline {} initialized (D3D12, custom-engine "
              "identity, application-id={}, project-id={})",
              g_status.streamline_version,
              preferences.applicationId,
              project_id[0] != '\0' ? "SK8 Engine-owned" : "not supplied");
#endif
  PublishStatusLocked();
}

void Shutdown() {
  std::lock_guard lock(g_mutex);
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  if (g_status.initialized && g_api.free_resources != nullptr) {
    g_api.free_resources(sl::kFeatureDLSS, g_viewport);
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
    if (g_status.neural_supported) {
      g_api.free_resources(sl::kFeatureDLSS_NR, g_viewport);
    }
#endif
  }
  if (g_status.initialized && g_api.shutdown != nullptr) {
    g_api.shutdown();
  }
  if (g_api.module != nullptr) {
    FreeLibrary(g_api.module);
  }
  g_api = {};
  g_device = nullptr;
  g_frame_token = nullptr;
  g_evaluation_failed = false;
  g_configured_mode = Mode::kOff;
  g_configured_output = {};
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  g_neural_options = {};
  g_configured_neural_settings = {};
  g_neural_options_configured = false;
  g_neural_evaluation_failed = false;
#endif
#endif
  g_history.Invalidate();
  g_status = {};
  g_status.unavailable_reason = UnavailableReason::kBuildDisabled;
  PublishStatusLocked();
}

Mode RequestedMode() {
  return SanitizeMode(rex::cvar::Query<std::int32_t>("skate3_dlss_mode"));
}

FramePlan BeginFrame(ID3D12Device *device, RenderSize output,
                     std::uint64_t scene_generation,
                     std::uint64_t sample_time_us,
                     const std::array<float, 3> &camera_position,
                     bool renderer_served_previous_frame) {
  std::lock_guard lock(g_mutex);
  FramePlan plan{};
  plan.mode = RequestedMode();
  plan.output = output;
  plan.render = output;
  g_status.selected_mode = plan.mode;
  g_status.output = output;
  g_status.neural_requested = ReadNeuralSettings().enabled;
  g_status.neural_active = false;
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR && \
    SKATE3_ENABLE_DLSS_NR_PREVIEW
  if (!g_status.neural_requested) {
    if (g_neural_options_configured) {
      g_neural_options.mode = sl::DLSSNRMode::eOff;
      if (g_api.set_neural_options != nullptr) {
        g_api.set_neural_options(g_viewport, g_neural_options);
      }
      if (g_api.free_resources != nullptr &&
          g_status.neural_plugin_present) {
        g_api.free_resources(sl::kFeatureDLSS_NR, g_viewport);
      }
    }
    g_neural_options_configured = false;
    g_neural_evaluation_failed = false;
    g_status.neural_detail.clear();
  }
#endif
  if (plan.mode == Mode::kOff) {
    g_history.Invalidate();
    g_jitter_index = 0;
    g_evaluation_failed = false;
    g_status.render = output;
    PublishStatusLocked();
    return plan;
  }
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  if (!g_status.initialized || device == nullptr) {
    g_status.unavailable_reason = UnavailableReason::kDevice;
    PublishStatusLocked();
    return plan;
  }
  if (g_device != device) {
    if (g_device != nullptr) {
      g_api.free_resources(sl::kFeatureDLSS, g_viewport);
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
      if (g_status.neural_plugin_present) {
        g_api.free_resources(sl::kFeatureDLSS_NR, g_viewport);
      }
      g_neural_options_configured = false;
      g_neural_evaluation_failed = false;
#endif
      g_history.Invalidate();
    }
    if (g_api.set_d3d_device(device) != sl::Result::eOk) {
      g_status.unavailable_reason = UnavailableReason::kDevice;
      PublishStatusLocked();
      return plan;
    }
    g_device = device;
    g_status.device_registered = true;
    const LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID =
        reinterpret_cast<std::uint8_t *>(const_cast<LUID *>(&luid));
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    const sl::Result support =
        g_api.is_feature_supported(sl::kFeatureDLSS, adapter);
    if (support != sl::Result::eOk) {
      g_status.supported = false;
      g_status.unavailable_reason = MapSupportFailure(support);
      PublishStatusLocked();
      return plan;
    }
    void *function = nullptr;
    if (g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings",
                                   function) != sl::Result::eOk) {
      g_status.unavailable_reason = UnavailableReason::kFeatureNotLoaded;
      PublishStatusLocked();
      return plan;
    }
    g_api.get_optimal_settings =
        reinterpret_cast<PFun_slDLSSGetOptimalSettings *>(function);
    function = nullptr;
    g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", function);
    g_api.set_options = reinterpret_cast<PFun_slDLSSSetOptions *>(function);
    function = nullptr;
    g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSGetState", function);
    g_api.get_state = reinterpret_cast<PFun_slDLSSGetState *>(function);
    if (g_api.set_options == nullptr || g_api.get_state == nullptr) {
      g_status.unavailable_reason = UnavailableReason::kFeatureNotLoaded;
      PublishStatusLocked();
      return plan;
    }
    sl::FeatureVersion version{};
    if (g_api.get_feature_version(sl::kFeatureDLSS, version) ==
        sl::Result::eOk) {
      g_status.dlss_version = version.versionNGX.toStr();
    }
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
    g_status.neural_supported = false;
    g_api.set_neural_options = nullptr;
    if (g_status.neural_plugin_present) {
      const sl::Result neural_support =
          g_api.is_feature_supported(sl::kFeatureDLSS_NR, adapter);
      if (neural_support == sl::Result::eOk) {
        function = nullptr;
        if (g_api.get_feature_function(sl::kFeatureDLSS_NR,
                                       "slDLSSNRSetOptions",
                                       function) == sl::Result::eOk) {
          g_api.set_neural_options =
              reinterpret_cast<sl::PFun_slDLSSNRSetOptions *>(function);
        }
        if (g_api.set_neural_options != nullptr) {
          g_status.neural_supported = true;
          g_status.neural_detail.clear();
          sl::FeatureVersion neural_version{};
          if (g_api.get_feature_version(sl::kFeatureDLSS_NR,
                                        neural_version) == sl::Result::eOk) {
            g_status.neural_version = neural_version.versionNGX.toStr();
          }
          REXLOG_INFO(
              "DLSS Neural Rendering: Streamline feature 1004 available, "
              "NGX version={}",
              g_status.neural_version.empty() ? "unknown"
                                               : g_status.neural_version);
        } else {
          g_status.neural_detail = "slDLSSNRSetOptions is unavailable";
        }
      } else {
        NeuralSupportFailure failure = NeuralSupportFailure::kOther;
        if (neural_support == sl::Result::eErrorFeatureMissing) {
          failure = NeuralSupportFailure::kPluginMissing;
        } else if (neural_support ==
                   sl::Result::eErrorFeatureNotSupported) {
          // The signed 2.13 plugin logs NGX 0xBAD0000C here. NVIDIA's public
          // NGX definitions identify that result as FAIL_OutOfDate.
          failure = NeuralSupportFailure::kPreviewDriverRequired;
        }
        g_status.neural_detail =
            std::format("{} (Streamline result {})",
                        NeuralSupportFailureText(failure),
                        static_cast<int>(neural_support));
        REXLOG_WARN("DLSS Neural Rendering: {}", g_status.neural_detail);
      }
    }
#endif
    g_status.supported = true;
    g_status.unavailable_reason = UnavailableReason::kNone;
    g_status.detail.clear();
    g_configured_mode = Mode::kOff;
    g_evaluation_failed = false;
  }
  if (!g_status.supported) {
    PublishStatusLocked();
    return plan;
  }
  if (g_evaluation_failed) {
    if (g_configured_mode != Mode::kOff) {
      g_api.free_resources(sl::kFeatureDLSS, g_viewport);
      g_configured_mode = Mode::kOff;
      g_configured_output = {};
    }
    g_status.unavailable_reason = UnavailableReason::kEvaluation;
    PublishStatusLocked();
    return plan;
  }
  if (g_configured_mode != plan.mode || g_configured_output != output) {
    if (g_configured_mode != Mode::kOff) {
      g_api.free_resources(sl::kFeatureDLSS, g_viewport);
    }
    if (!ConfigureViewportLocked(plan.mode, output, plan.render)) {
      PublishStatusLocked();
      return plan;
    }
    g_history.Invalidate();
  } else {
    sl::DLSSOptimalSettings optimal{};
    if (g_api.get_optimal_settings(g_options, optimal) != sl::Result::eOk) {
      g_status.unavailable_reason = UnavailableReason::kOptimalSettings;
      PublishStatusLocked();
      return plan;
    }
    plan.render = {optimal.optimalRenderWidth, optimal.optimalRenderHeight};
  }
  plan.active = plan.render.valid();
  plan.mip_lod_bias = RecommendedMipLodBias(plan.render, plan.output);
  ResetInputs reset{};
  reset.mode = plan.mode;
  reset.render = plan.render;
  reset.output = output;
  reset.scene_generation = scene_generation;
  reset.sample_time_us = sample_time_us;
  reset.camera_position = camera_position;
  reset.renderer_served_previous_frame = renderer_served_previous_frame;
  plan.reset = g_history.Update(reset);
  if (plan.reset) {
    g_jitter_index = 0;
    ++g_status.history_resets;
  }
  plan.jitter_pixels = JitterForFrame(g_jitter_index++);
  g_status.render = plan.render;
  g_status.unavailable_reason = UnavailableReason::kNone;
#else
  g_status.unavailable_reason = UnavailableReason::kBuildDisabled;
#endif
  PublishStatusLocked();
  return plan;
}

void NotifyFrameNotServed() {
  std::lock_guard lock(g_mutex);
  g_history.Invalidate();
}

bool Evaluate(ID3D12GraphicsCommandList *command_list,
              const TaggedResources &resources, const CameraData &camera,
              const FramePlan &plan) {
  std::lock_guard lock(g_mutex);
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  if (!plan.active || !g_status.supported || command_list == nullptr ||
      !resources.valid()) {
    if (plan.active) {
      RecordEvaluationFailureLocked(UnavailableReason::kResources,
                                    sl::Result::eErrorMissingInputParameter);
    }
    return false;
  }
  if (g_api.get_new_frame_token(g_frame_token, &g_frame_index) !=
          sl::Result::eOk ||
      g_frame_token == nullptr) {
    RecordEvaluationFailureLocked(UnavailableReason::kEvaluation,
                                  sl::Result::eErrorInvalidState);
    return false;
  }
  ++g_frame_index;

  constexpr std::uint32_t input_state =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  sl::Resource color{sl::ResourceType::eTex2d,
                     const_cast<void *>(resources.color), input_state};
  sl::Resource depth{sl::ResourceType::eTex2d,
                     const_cast<void *>(resources.depth), input_state};
  sl::Resource motion{sl::ResourceType::eTex2d,
                      const_cast<void *>(resources.motion), input_state};
  sl::Resource output{sl::ResourceType::eTex2d,
                      const_cast<void *>(resources.output), input_state};
  const sl::Extent render_extent{0, 0, resources.render.width,
                                 resources.render.height};
  const sl::Extent output_extent{0, 0, resources.output_size.width,
                                 resources.output_size.height};
  const sl::ResourceTag tags[] = {
      {&color, sl::kBufferTypeScalingInputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &render_extent},
      {&output, sl::kBufferTypeScalingOutputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
      {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow,
       &render_extent},
      {&motion, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eOnlyValidNow, &render_extent}};
  const sl::Result tag_result = g_api.set_tag_for_frame(
      *g_frame_token, g_viewport, tags,
      static_cast<std::uint32_t>(std::size(tags)), command_list);
  if (tag_result != sl::Result::eOk) {
    RecordEvaluationFailureLocked(UnavailableReason::kResources, tag_result);
    return false;
  }
  sl::Constants constants{};
  CopyMatrix(camera.projection, constants.cameraViewToClip);
  CopyMatrix(camera.inverse_projection, constants.clipToCameraView);
  CopyMatrix(camera.clip_to_previous_clip, constants.clipToPrevClip);
  CopyMatrix(camera.previous_clip_to_clip, constants.prevClipToClip);
  constants.jitterOffset = {plan.jitter_pixels.x, plan.jitter_pixels.y};
  constants.mvecScale = {1.0f, 1.0f};
  constants.cameraPos = {camera.position[0], camera.position[1],
                         camera.position[2]};
  constants.cameraUp = {camera.up[0], camera.up[1], camera.up[2]};
  constants.cameraRight = {camera.right[0], camera.right[1], camera.right[2]};
  constants.cameraFwd = {camera.forward[0], camera.forward[1],
                         camera.forward[2]};
  constants.cameraNear = camera.near_plane;
  constants.cameraFar = camera.far_plane;
  constants.cameraFOV = camera.vertical_fov;
  constants.cameraAspectRatio = camera.aspect;
  constants.depthInverted = sl::Boolean::eFalse;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = plan.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.orthographicProjection = sl::Boolean::eFalse;
  constants.motionVectorsDilated = sl::Boolean::eFalse;
  constants.motionVectorsJittered = sl::Boolean::eFalse;
  const sl::Result constants_result =
      g_api.set_constants(constants, *g_frame_token, g_viewport);
  if (constants_result != sl::Result::eOk) {
    RecordEvaluationFailureLocked(UnavailableReason::kEvaluation,
                                  constants_result);
    return false;
  }
  const sl::BaseStructure *inputs[] = {&g_viewport};
  const sl::Result result = g_api.evaluate_feature(
      sl::kFeatureDLSS, *g_frame_token, inputs, 1, command_list);
  if (result != sl::Result::eOk) {
    RecordEvaluationFailureLocked(UnavailableReason::kEvaluation, result);
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (g_api.get_state != nullptr &&
      (g_last_state_query == std::chrono::steady_clock::time_point{} ||
       now - g_last_state_query >= std::chrono::seconds(5))) {
    sl::DLSSState state{};
    if (g_api.get_state(g_viewport, state) == sl::Result::eOk) {
      g_status.estimated_vram_bytes = state.estimatedVRAMUsageInBytes;
    }
    g_last_state_query = now;
    REXLOG_INFO("DLSS SR: {} {}x{} -> {}x{}, LOD bias {:.3f}, reset={}, "
                "estimated VRAM={} MiB, failures={}",
                ModeName(plan.mode), plan.render.width, plan.render.height,
                plan.output.width, plan.output.height, plan.mip_lod_bias,
                plan.reset ? 1 : 0,
                g_status.estimated_vram_bytes / (1024 * 1024),
                g_status.evaluation_failures);
  }
  return true;
#else
  (void)command_list;
  (void)resources;
  (void)camera;
  (void)plan;
  return false;
#endif
}

NeuralSettings RequestedNeuralSettings() {
  return ReadNeuralSettings();
}

bool EvaluateNeuralRendering(ID3D12GraphicsCommandList *command_list,
                             const NeuralTaggedResources &resources,
                             const FramePlan &plan) {
  std::lock_guard lock(g_mutex);
  const NeuralSettings settings = ReadNeuralSettings();
  g_status.neural_requested = settings.enabled;
  g_status.neural_active = false;
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR && \
    SKATE3_ENABLE_DLSS_NR_PREVIEW
  if (!settings.enabled) {
    if (g_neural_options_configured && g_api.set_neural_options != nullptr) {
      g_neural_options.mode = sl::DLSSNRMode::eOff;
      g_api.set_neural_options(g_viewport, g_neural_options);
      g_api.free_resources(sl::kFeatureDLSS_NR, g_viewport);
    }
    g_neural_options_configured = false;
    g_neural_evaluation_failed = false;
    PublishStatusLocked();
    return false;
  }
  if (!plan.active || !g_status.neural_plugin_present ||
      !g_status.neural_supported || g_api.set_neural_options == nullptr) {
    if (g_status.neural_detail.empty()) {
      g_status.neural_detail =
          !plan.active ? "DLSS SR or DLAA is not active"
                       : "feature 1004 is unavailable on this system";
    }
    PublishStatusLocked();
    return false;
  }
  if (g_neural_evaluation_failed) {
    g_status.neural_detail =
        "evaluation disabled until Neural Rendering is toggled Off";
    PublishStatusLocked();
    return false;
  }
  if (command_list == nullptr || g_frame_token == nullptr ||
      !resources.valid()) {
    ++g_status.neural_evaluation_failures;
    g_status.neural_detail =
        "required full-resolution color, depth, motion, or output is invalid";
    g_neural_evaluation_failed = true;
    PublishStatusLocked();
    return false;
  }

  if (!g_neural_options_configured ||
      settings != g_configured_neural_settings) {
    g_neural_options = {};
    g_neural_options.mode = sl::DLSSNRMode::eOn;
    g_neural_options.intensity = settings.intensity;
    g_neural_options.localToneStrength = settings.local_tone_strength;
    g_neural_options.localStructureStrength =
        settings.local_structure_strength;
    g_neural_options.globalToneStrength = settings.global_tone_strength;
    g_neural_options.style = settings.style;
    g_neural_options.preset = settings.preset;
    g_neural_options.useAutoMask =
        settings.use_auto_mask ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    g_neural_options.skinStructureStrength =
        settings.skin_structure_strength;
    g_neural_options.performanceMode = settings.performance_mode;
    const sl::Result options_result =
        g_api.set_neural_options(g_viewport, g_neural_options);
    if (options_result != sl::Result::eOk) {
      ++g_status.neural_evaluation_failures;
      g_status.neural_detail =
          std::format("option setup failed ({})",
                      static_cast<int>(options_result));
      g_neural_evaluation_failed = true;
      PublishStatusLocked();
      return false;
    }
    g_configured_neural_settings = settings;
    g_neural_options_configured = true;
  }

  constexpr std::uint32_t input_state =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  sl::Resource input{sl::ResourceType::eTex2d,
                     const_cast<void *>(resources.input), input_state};
  sl::Resource depth{sl::ResourceType::eTex2d,
                     const_cast<void *>(resources.depth), input_state};
  sl::Resource motion{sl::ResourceType::eTex2d,
                      const_cast<void *>(resources.motion), input_state};
  sl::Resource output{sl::ResourceType::eTex2d,
                      const_cast<void *>(resources.output), input_state};
  const sl::Extent render_extent{0, 0, resources.render.width,
                                 resources.render.height};
  const sl::Extent output_extent{0, 0, resources.output_size.width,
                                 resources.output_size.height};
  const sl::ResourceTag tags[] = {
      {&input, sl::kBufferTypeUpliftInputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
      {&output, sl::kBufferTypeUpliftOutputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
      {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow,
       &render_extent},
      {&motion, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eOnlyValidNow, &render_extent}};
  sl::Result result = g_api.set_tag_for_frame(
      *g_frame_token, g_viewport, tags,
      static_cast<std::uint32_t>(std::size(tags)), command_list);
  if (result == sl::Result::eOk) {
    const sl::BaseStructure *inputs[] = {&g_viewport};
    result = g_api.evaluate_feature(sl::kFeatureDLSS_NR, *g_frame_token,
                                    inputs, 1, command_list);
  }
  if (result != sl::Result::eOk) {
    ++g_status.neural_evaluation_failures;
    g_status.neural_detail =
        std::format("feature 1004 evaluation failed ({})",
                    static_cast<int>(result));
    g_neural_evaluation_failed = true;
    if ((g_status.neural_evaluation_failures & 63u) == 1u) {
      REXLOG_ERROR(
          "DLSS Neural Rendering: disabled after evaluation failure {}",
          static_cast<int>(result));
    }
    PublishStatusLocked();
    return false;
  }

  g_status.neural_active = true;
  g_status.neural_detail.clear();
  const auto now = std::chrono::steady_clock::now();
  if (g_last_neural_log == std::chrono::steady_clock::time_point{} ||
      now - g_last_neural_log >= std::chrono::seconds(5)) {
    g_last_neural_log = now;
    REXLOG_INFO(
        "DLSS Neural Rendering: feature=1004, {}x{}, intensity={:.2f}, "
        "tone={:.2f}/{:.2f}, structure={:.2f}/{:.2f}, auto-mask={}, "
        "style={}, reset={}, failures={}",
        resources.output_size.width, resources.output_size.height,
        settings.intensity, settings.local_tone_strength,
        settings.global_tone_strength, settings.local_structure_strength,
        settings.skin_structure_strength, settings.use_auto_mask ? 1 : 0,
        settings.style, plan.reset ? 1 : 0,
        g_status.neural_evaluation_failures);
  }
  PublishStatusLocked();
  return true;
#else
  if (settings.enabled) {
    g_status.neural_detail = "This build does not include the private preview";
  }
  (void)command_list;
  (void)resources;
  (void)plan;
  PublishStatusLocked();
  return false;
#endif
}

void ReleaseViewportResources() {
  std::lock_guard lock(g_mutex);
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  if (g_status.initialized && g_api.free_resources != nullptr &&
      g_configured_mode != Mode::kOff) {
    g_api.free_resources(sl::kFeatureDLSS, g_viewport);
  }
#if SKATE3_ENABLE_DLSS_NR_PREVIEW
  if (g_status.initialized && g_api.free_resources != nullptr &&
      g_status.neural_supported) {
    g_api.free_resources(sl::kFeatureDLSS_NR, g_viewport);
  }
  g_neural_options_configured = false;
  g_neural_evaluation_failed = false;
#endif
  g_configured_mode = Mode::kOff;
  g_configured_output = {};
  g_evaluation_failed = false;
#endif
  g_history.Invalidate();
}

Status GetStatus() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

std::string StatusLine() {
  std::lock_guard lock(g_mutex);
  return rex::cvar::Query<std::string>("skate3_dlss_status");
}

bool BuildHasDlss() {
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  return true;
#else
  return false;
#endif
}

} // namespace skate3::dlss
