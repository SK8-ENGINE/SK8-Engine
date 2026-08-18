#pragma once

#include <cstdint>
#include <vector>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
namespace nrhi {
class Cmd;
class Texture;
class TextureView;
}
}  // namespace rex::graphics

namespace skate3::native_scene {

struct FrameScene;

constexpr std::uint32_t kRaytracedDynamicTextureLimit = 64;

struct RaytracedDynamicVertex {
  float position[3] = {};
  float normal[3] = {};
  float uv[2] = {};
};

struct RaytracedDynamicMaterial {
  float color[3] = {0.45f, 0.45f, 0.45f};
  float roughness = 0.7f;
  std::uint32_t lighting_index = 0xffffffffu;
  std::uint32_t character_family = 0;
  std::uint32_t texture_index = 0xffffffffu;
  std::uint32_t texture_flags = 0;
};

struct RaytracedCharacterLighting {
  // Exact scene character cbuffer rows: light, key/exposure, ambient,
  // nine SH rows, tint A/B, misc, key/rim spec and rim light.
  float rows[72] = {};
};

struct RaytracedMovingLight {
  float position_range[4] = {};
  float color_intensity[4] = {};
  float source_radius = 0.0f;
  float unused[3] = {};
};

// CPU-skinned, world-space presentation geometry for the current rendered
// frame. The native scene renderer builds this from the same decoded meshes
// and interpolated bone palettes used by the visible skater.
struct RaytracedDynamicScene {
  std::vector<RaytracedDynamicVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<RaytracedDynamicMaterial> triangle_materials;
  std::vector<RaytracedCharacterLighting> character_lighting;
  std::vector<RaytracedMovingLight> moving_lights;
  // The exact texture/view tuple used by the main character draw. The view
  // is retained so the DXR descriptor preserves the guest texture's host
  // format and component swizzle rather than reconstructing an approximate
  // SRV from the underlying resource.
  struct TextureBinding {
    rex::graphics::nrhi::Texture* texture = nullptr;
    rex::graphics::nrhi::TextureView* view = nullptr;
  };
  std::vector<TextureBinding> diffuse_textures;
};

struct RaytracedMirrorTelemetry {
  bool supported = false;
  bool initialized = false;
  bool acceleration_structure_recorded = false;
  std::uint64_t dispatches = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t triangle_count = 0;
  std::uint32_t dynamic_triangle_count = 0;
  std::uint32_t reflector_count = 0;
  std::uint32_t puddle_count = 0;
};

// Records hardware inline-ray-query passes for the authored planar mirror
// and wet puddles, then composites each result by rasterizing its real plane
// against the main scene depth.
// Returns false when DXR is unavailable or initialization failed.
bool RenderRaytracedMirror(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    rex::graphics::nrhi::Cmd* cmd, const FrameScene& scene,
    rex::graphics::nrhi::Texture* scene_color,
    rex::graphics::nrhi::Texture* scene_depth,
    std::uint32_t scene_sample_count,
    const RaytracedDynamicScene* dynamic_scene);

RaytracedMirrorTelemetry GetRaytracedMirrorTelemetry();

}  // namespace skate3::native_scene
