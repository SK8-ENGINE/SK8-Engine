#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <sl.h>

#include "skate3_dlss_nr_private.h"

#include <array>
#include <filesystem>
#include <iostream>

namespace {

template <typename T>
bool Import(HMODULE module, const char *name, T *&output) {
  output = reinterpret_cast<T *>(GetProcAddress(module, name));
  return output != nullptr;
}

} // namespace

int main() {
  HMODULE module = LoadLibraryExW(
      L"sl.interposer.dll", nullptr,
      LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (module == nullptr) {
    std::cerr << "sl.interposer.dll could not be loaded\n";
    return 1;
  }

  PFun_slInit *init = nullptr;
  PFun_slShutdown *shutdown = nullptr;
  PFun_slSetD3DDevice *set_device = nullptr;
  PFun_slIsFeatureSupported *is_supported = nullptr;
  PFun_slGetFeatureFunction *get_feature_function = nullptr;
  if (!Import(module, "slInit", init) ||
      !Import(module, "slShutdown", shutdown) ||
      !Import(module, "slSetD3DDevice", set_device) ||
      !Import(module, "slIsFeatureSupported", is_supported) ||
      !Import(module, "slGetFeatureFunction", get_feature_function)) {
    std::cerr << "required Streamline core export is missing\n";
    FreeLibrary(module);
    return 2;
  }

  wchar_t executable[MAX_PATH]{};
  GetModuleFileNameW(nullptr, executable, MAX_PATH);
  const std::wstring plugin_directory =
      std::filesystem::path(executable).parent_path().wstring();
  const wchar_t *plugin_paths[] = {plugin_directory.c_str()};
  const std::array<sl::Feature, 2> features{sl::kFeatureDLSS,
                                           sl::kFeatureDLSS_NR};
  sl::Preferences preferences{};
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = plugin_directory.c_str();
  preferences.logLevel = sl::LogLevel::eVerbose;
  preferences.featuresToLoad = features.data();
  preferences.numFeaturesToLoad =
      static_cast<std::uint32_t>(features.size());
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eDisableDebugText |
                      sl::PreferenceFlags::eUseManualHooking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = "DLSS-NR API smoke";
  preferences.projectId = "2dbb17e3-cfcb-5063-9d5f-e5247d36d3f2";
  preferences.applicationId = 0;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  const sl::Result init_result =
      init(preferences, sl::kSDKVersionDLSSNRPreview);
  if (init_result != sl::Result::eOk) {
    std::cerr << "slInit failed: " << static_cast<int>(init_result) << '\n';
    FreeLibrary(module);
    return 3;
  }

  IDXGIFactory6 *factory = nullptr;
  IDXGIAdapter1 *adapter = nullptr;
  ID3D12Device *device = nullptr;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) {
    hr = factory->EnumAdapterByGpuPreference(
        0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  }
  if (SUCCEEDED(hr)) {
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                           IID_PPV_ARGS(&device));
  }
  if (FAILED(hr) || device == nullptr) {
    std::cerr << "D3D12 device creation failed\n";
    if (adapter != nullptr) adapter->Release();
    if (factory != nullptr) factory->Release();
    shutdown();
    FreeLibrary(module);
    return 4;
  }

  const sl::Result device_result = set_device(device);
  const LUID luid = device->GetAdapterLuid();
  sl::AdapterInfo adapter_info{};
  adapter_info.deviceLUID =
      reinterpret_cast<std::uint8_t *>(const_cast<LUID *>(&luid));
  adapter_info.deviceLUIDSizeInBytes = sizeof(luid);
  const sl::Result sr_support =
      is_supported(sl::kFeatureDLSS, adapter_info);
  const sl::Result nr_support =
      is_supported(sl::kFeatureDLSS_NR, adapter_info);
  void *set_options = nullptr;
  const sl::Result function_result = get_feature_function(
      sl::kFeatureDLSS_NR, "slDLSSNRSetOptions", set_options);
  std::cout << "set-device=" << static_cast<int>(device_result)
            << " dlss-sr-support=" << static_cast<int>(sr_support)
            << " dlss-nr-support=" << static_cast<int>(nr_support)
            << " nr-function=" << static_cast<int>(function_result)
            << " pointer=" << (set_options != nullptr ? "yes" : "no")
            << '\n';

  device->Release();
  adapter->Release();
  factory->Release();
  shutdown();
  FreeLibrary(module);
  return device_result == sl::Result::eOk ? 0 : 5;
}
