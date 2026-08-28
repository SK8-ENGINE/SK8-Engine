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

#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <windows.h>

#include <sl.h>
#include <sl_dlss.h>
#endif

namespace skate3::dlss {
namespace {

std::mutex g_mutex;
Status g_status;
HistoryTracker g_history;
std::uint32_t g_jitter_index = 0;
std::uint32_t g_frame_index = 0;
bool g_evaluation_failed = false;

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
};

Api g_api;
sl::ViewportHandle g_viewport{0u};
ID3D12Device *g_device = nullptr;
Mode g_configured_mode = Mode::kOff;
RenderSize g_configured_output{};
sl::DLSSOptions g_options{};
sl::FrameToken *g_frame_token = nullptr;
std::chrono::steady_clock::time_point g_last_state_query{};

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

  const sl::Feature features[] = {sl::kFeatureDLSS};
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
  preferences.featuresToLoad = features;
  preferences.numFeaturesToLoad = 1;
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = SKATE3_DLSS_ENGINE_VERSION;
  preferences.projectId = project_id[0] != '\0' ? project_id : nullptr;
  preferences.applicationId = SKATE3_NVIDIA_APPLICATION_ID;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  if (g_api.init(preferences, sl::kSDKVersion) != sl::Result::eOk) {
    g_status.unavailable_reason = UnavailableReason::kInitialization;
    PublishStatusLocked();
    return;
  }
  g_status.initialized = true;
  g_status.unavailable_reason = UnavailableReason::kNone;
  g_status.detail.clear();
  g_status.streamline_version = "2.12.0";
  REXLOG_INFO("DLSS SR: Streamline 2.12.0 initialized (D3D12, custom-engine "
              "identity, application-id={}, project-id={})",
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

void ReleaseViewportResources() {
  std::lock_guard lock(g_mutex);
#if defined(_WIN32) && SKATE3_ENABLE_DLSS_SR
  if (g_status.initialized && g_api.free_resources != nullptr &&
      g_configured_mode != Mode::kOff) {
    g_api.free_resources(sl::kFeatureDLSS, g_viewport);
  }
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
