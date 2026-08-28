#pragma once

#include "skate3_dlss_sr_types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace skate3::dlss {

struct CameraData {
  std::array<float, 16> projection{};
  std::array<float, 16> inverse_projection{};
  std::array<float, 16> view_projection{};
  std::array<float, 16> previous_view_projection{};
  std::array<float, 16> clip_to_previous_clip{};
  std::array<float, 16> previous_clip_to_clip{};
  std::array<float, 3> position{};
  std::array<float, 3> up{};
  std::array<float, 3> right{};
  std::array<float, 3> forward{};
  float near_plane = 0.1f;
  float far_plane = 10000.0f;
  float vertical_fov = 1.0f;
  float aspect = 16.0f / 9.0f;
};

struct FramePlan {
  Mode mode = Mode::kOff;
  RenderSize render;
  RenderSize output;
  Jitter jitter_pixels;
  float mip_lod_bias = 0.0f;
  bool active = false;
  bool reset = true;
};

struct Status {
  bool initialized = false;
  bool device_registered = false;
  bool supported = false;
  UnavailableReason unavailable_reason = UnavailableReason::kBuildDisabled;
  std::string detail;
  std::string streamline_version;
  std::string dlss_version;
  std::string gpu_name;
  std::string driver;
  Mode selected_mode = Mode::kOff;
  RenderSize render;
  RenderSize output;
  std::uint64_t estimated_vram_bytes = 0;
  std::uint64_t evaluation_failures = 0;
  std::uint64_t history_resets = 0;
};

void InitializeEarly(const std::filesystem::path &log_directory);
void Shutdown();
Mode RequestedMode();
FramePlan BeginFrame(ID3D12Device *device, RenderSize output,
                     std::uint64_t scene_generation,
                     std::uint64_t sample_time_us,
                     const std::array<float, 3> &camera_position,
                     bool renderer_served_previous_frame);
void NotifyFrameNotServed();
bool Evaluate(ID3D12GraphicsCommandList *command_list,
              const TaggedResources &resources, const CameraData &camera,
              const FramePlan &plan);
void ReleaseViewportResources();
Status GetStatus();
std::string StatusLine();
bool BuildHasDlss();

} // namespace skate3::dlss
