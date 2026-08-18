#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skate3_native_raytraced_mirror.h"

#include "skate3_mechanics_sandbox.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_native_scene.h"
#include "skate3_raytraced_mirror_cs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <wrl/client.h>

#include <rex/graphics/d3d12/native_rhi_d3d12.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/native_rhi.h>
#include <rex/logging.h>

namespace skate3::native_scene {
namespace {

using Microsoft::WRL::ComPtr;
namespace nrhi = rex::graphics::nrhi;
namespace d3d12 = rex::graphics::d3d12;

// 48 constants plus seven root SRVs and two descriptor tables exactly fill
// D3D12's 64-DWORD root-signature budget. Reflector metadata is packed into
// otherwise-unused alpha components rather than growing this block.
constexpr std::uint32_t kMirrorConstantCount = 48;
constexpr std::uint32_t kCompositeConstantCount = 32;
constexpr std::uint32_t kDynamicSlotCount = 8;

struct RtVertex {
  float position[3];
  float normal[3];
};
static_assert(sizeof(RtVertex) == 24);

struct RtMaterial {
  float color[3];
  float roughness;
  float pattern;
  float texture_scale;
  float variation;
  float unused;
};
static_assert(sizeof(RtMaterial) == 32);
static_assert(sizeof(RaytracedDynamicVertex) == 32);
static_assert(sizeof(RaytracedDynamicMaterial) == 32);
static_assert(sizeof(RaytracedCharacterLighting) == 288);
static_assert(sizeof(RaytracedMovingLight) == 48);

struct DynamicMetadataHeader {
  std::uint32_t material_offset = 32;
  std::uint32_t material_stride = 32;
  std::uint32_t lighting_offset = 0;
  std::uint32_t lighting_stride = 288;
  std::uint32_t moving_light_offset = 0;
  std::uint32_t moving_light_stride = 48;
  std::uint32_t moving_light_count = 0;
  std::uint32_t night_profile = 1;
};
static_assert(sizeof(DynamicMetadataHeader) == 32);

struct DynamicSlot {
  ComPtr<ID3D12Resource> vertices;
  ComPtr<ID3D12Resource> indices;
  ComPtr<ID3D12Resource> materials;
  ComPtr<ID3D12Resource> scratch;
  ComPtr<ID3D12Resource> blas;
  ComPtr<ID3D12Resource> instances;
  ComPtr<ID3D12Resource> tlas;
  ComPtr<ID3D12DescriptorHeap> descriptor_heap;
  std::vector<ID3D12Resource*> texture_resources;
  D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
  std::uint64_t vertex_capacity = 0;
  std::uint64_t index_capacity = 0;
  std::uint64_t material_capacity = 0;
  std::uint64_t scratch_capacity = 0;
  std::uint64_t blas_capacity = 0;
  std::uint64_t tlas_capacity = 0;
  std::uint64_t last_submission = 0;
  std::uint32_t triangle_count = 0;
};

struct State {
  bool support_checked = false;
  bool supported = false;
  bool initialized = false;
  bool build_recorded = false;
  bool failure_logged = false;
  bool placement_logged = false;
  float origin[3] = {};
  std::uint32_t triangle_count = 0;

  ComPtr<ID3D12Device5> device;
  ComPtr<ID3D12Resource> vertices;
  ComPtr<ID3D12Resource> indices;
  ComPtr<ID3D12Resource> materials;
  ComPtr<ID3D12Resource> scratch;
  ComPtr<ID3D12Resource> blas;
  ComPtr<ID3D12Resource> instances;
  ComPtr<ID3D12Resource> tlas;
  ComPtr<ID3D12RootSignature> compute_root;
  ComPtr<ID3D12PipelineState> compute_pso;
  ComPtr<ID3D12DescriptorHeap> uav_heap;
  D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
      blas_inputs{};
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
      tlas_inputs{};
  std::array<DynamicSlot, kDynamicSlotCount> dynamic_slots;
  std::uint32_t dynamic_triangle_count = 0;

  nrhi::Texture* output = nullptr;
  nrhi::TextureView* output_srv = nullptr;
  ID3D12Resource* output_resource = nullptr;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;

  nrhi::BindingLayout* composite_layout = nullptr;
  nrhi::Pipeline* composite_pso = nullptr;
  nrhi::Format composite_format = nrhi::Format::kUnknown;
  std::uint32_t composite_samples = 0;
  std::uint64_t dispatches = 0;
};

State& MirrorState() {
  static State state;
  return state;
}

std::uint64_t Align(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device* device, std::uint64_t size,
    D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heap_type;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = std::max<std::uint64_t>(size, 256);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state,
          nullptr, IID_PPV_ARGS(&resource)))) {
    return {};
  }
  return resource;
}

template <typename T>
ComPtr<ID3D12Resource> CreateUpload(
    ID3D12Device* device, const std::vector<T>& values) {
  const std::uint64_t bytes =
      std::max<std::uint64_t>(sizeof(T) * values.size(), 1);
  ComPtr<ID3D12Resource> resource = CreateBuffer(
      device, bytes, D3D12_HEAP_TYPE_UPLOAD,
      D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!resource) {
    return {};
  }
  void* mapping = nullptr;
  if (FAILED(resource->Map(0, nullptr, &mapping))) {
    return {};
  }
  if (!values.empty()) {
    std::memcpy(mapping, values.data(),
                sizeof(T) * values.size());
  }
  resource->Unmap(0, nullptr);
  return resource;
}

bool Invert4x4(const float matrix[16], float inverse[16]) {
  float augmented[4][8];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      augmented[row][column] = matrix[row * 4 + column];
      augmented[row][column + 4] =
          row == column ? 1.0f : 0.0f;
    }
  }
  for (int column = 0; column < 4; ++column) {
    int pivot = column;
    for (int row = column + 1; row < 4; ++row) {
      if (std::fabs(augmented[row][column]) >
          std::fabs(augmented[pivot][column])) {
        pivot = row;
      }
    }
    if (std::fabs(augmented[pivot][column]) < 1.0e-12f) {
      return false;
    }
    if (pivot != column) {
      for (int entry = 0; entry < 8; ++entry) {
        std::swap(augmented[column][entry],
                  augmented[pivot][entry]);
      }
    }
    const float scale = 1.0f / augmented[column][column];
    for (int entry = 0; entry < 8; ++entry) {
      augmented[column][entry] *= scale;
    }
    for (int row = 0; row < 4; ++row) {
      if (row == column) {
        continue;
      }
      const float amount = augmented[row][column];
      for (int entry = 0; entry < 8; ++entry) {
        augmented[row][entry] -=
            amount * augmented[column][entry];
      }
    }
  }
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      inverse[row * 4 + column] =
          augmented[row][column + 4];
    }
  }
  return true;
}

const skate::world::SurfaceMaterial* FindMaterial(
    const skate::world::MapDefinition& definition,
    skate::world::MaterialId id) {
  const auto found = std::find_if(
      definition.materials.begin(), definition.materials.end(),
      [id](const skate::world::SurfaceMaterial& material) {
        return material.id == id;
      });
  return found != definition.materials.end() ? &*found : nullptr;
}

bool CreateRootAndPipeline(State& state) {
  D3D12_DESCRIPTOR_RANGE uav_range{};
  uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  uav_range.NumDescriptors = 1;
  uav_range.BaseShaderRegister = 0;

  D3D12_DESCRIPTOR_RANGE texture_range{};
  texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  texture_range.NumDescriptors = kRaytracedDynamicTextureLimit;
  texture_range.BaseShaderRegister = 7;

  D3D12_ROOT_PARAMETER parameters[10] = {};
  parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues =
      kMirrorConstantCount;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (std::uint32_t index = 1; index <= 7; ++index) {
    parameters[index].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[index].Descriptor.ShaderRegister = index - 1;
    parameters[index].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
  }
  parameters[8].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[8].DescriptorTable.NumDescriptorRanges = 1;
  parameters[8].DescriptorTable.pDescriptorRanges = &uav_range;
  parameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[9].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[9].DescriptorTable.NumDescriptorRanges = 1;
  parameters[9].DescriptorTable.pDescriptorRanges = &texture_range;
  parameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_STATIC_SAMPLER_DESC texture_sampler{};
  texture_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  texture_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  texture_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  texture_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  texture_sampler.MipLODBias = 0.0f;
  texture_sampler.MaxAnisotropy = 1;
  texture_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  texture_sampler.BorderColor =
      D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  texture_sampler.MinLOD = 0.0f;
  texture_sampler.MaxLOD = std::numeric_limits<float>::max();
  texture_sampler.ShaderRegister = 0;
  texture_sampler.RegisterSpace = 0;
  texture_sampler.ShaderVisibility =
      D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = std::size(parameters);
  root_desc.pParameters = parameters;
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &texture_sampler;
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error;
  if (FAILED(D3D12SerializeRootSignature(
          &root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
          &blob, &error)) ||
      FAILED(state.device->CreateRootSignature(
          0, blob->GetBufferPointer(), blob->GetBufferSize(),
          IID_PPV_ARGS(&state.compute_root)))) {
    if (error) {
      REXLOG_ERROR("raytraced-mirror: root signature: {}",
                   static_cast<const char*>(
                       error->GetBufferPointer()));
    }
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = state.compute_root.Get();
  pipeline.CS = {
      g_skate3_raytraced_mirror_cs,
      sizeof(g_skate3_raytraced_mirror_cs)};
  if (FAILED(state.device->CreateComputePipelineState(
          &pipeline, IID_PPV_ARGS(&state.compute_pso)))) {
    REXLOG_ERROR(
        "raytraced-mirror: inline-ray-query PSO creation failed");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = 1 + kRaytracedDynamicTextureLimit;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(state.device->CreateDescriptorHeap(
          &heap, IID_PPV_ARGS(&state.uav_heap)))) {
    return false;
  }
  const UINT increment =
      state.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE handle =
      state.uav_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += increment;
  D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
  null_srv.Shader4ComponentMapping =
      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_srv.Texture2D.MipLevels = 1;
  for (std::uint32_t index = 0;
       index < kRaytracedDynamicTextureLimit; ++index) {
    state.device->CreateShaderResourceView(
        nullptr, &null_srv, handle);
    handle.ptr += increment;
  }
  return true;
}

bool InitializeAccelerationStructure(
    State& state, nrhi::Device* rhi_device,
    const float origin[3]) {
  ID3D12Device* base_device =
      d3d12::NativeRhiD3D12GetDevice(rhi_device);
  if (base_device == nullptr) {
    return false;
  }
  state.support_checked = true;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  if (FAILED(base_device->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS5, &options,
          sizeof(options))) ||
      options.RaytracingTier <
          D3D12_RAYTRACING_TIER_1_1 ||
      FAILED(base_device->QueryInterface(
          IID_PPV_ARGS(&state.device)))) {
    REXLOG_WARN(
        "raytraced-mirror: DXR 1.1 inline ray queries unavailable");
    return false;
  }
  state.supported = true;

  const skate::world::MapDefinition& definition =
      mechanics_sandbox::map::ActiveDefinition();
  const skate::world::RenderMesh& mesh = definition.render_mesh;
  if (mesh.vertices.empty() || mesh.indices.empty() ||
      mesh.indices.size() % 3 != 0) {
    return false;
  }
  std::vector<RtVertex> vertices;
  vertices.reserve(mesh.vertices.size());
  for (const skate::world::RenderVertex& source : mesh.vertices) {
    vertices.push_back({
        {source.position.x + origin[0],
         source.position.y + origin[1],
         source.position.z + origin[2]},
        {source.normal.x, source.normal.y, source.normal.z}});
  }
  std::vector<std::uint32_t> indices = mesh.indices;
  std::vector<RtMaterial> materials;
  materials.reserve(indices.size() / 3);
  for (std::size_t triangle = 0;
       triangle < indices.size() / 3; ++triangle) {
    const std::uint32_t vertex_index = indices[triangle * 3];
    const skate::world::MaterialId material_id =
        vertex_index < mesh.vertices.size()
            ? mesh.vertices[vertex_index].material
            : 0;
    const skate::world::SurfaceMaterial* material =
        FindMaterial(definition, material_id);
    const skate::world::Vec3 color =
        material != nullptr
            ? material->display_color
            : skate::world::Vec3{0.5f, 0.5f, 0.5f};
    materials.push_back({
        {color.x, color.y, color.z},
        material != nullptr ? material->roughness : 0.8f,
        material != nullptr
            ? static_cast<float>(material->pattern)
            : 0.0f,
        material != nullptr ? material->texture_scale : 1.0f,
        material != nullptr ? material->variation : 0.0f,
        material != nullptr ? material->emissive_intensity : 0.0f});
  }
  state.vertices = CreateUpload(state.device.Get(), vertices);
  state.indices = CreateUpload(state.device.Get(), indices);
  state.materials = CreateUpload(state.device.Get(), materials);
  if (!state.vertices || !state.indices || !state.materials) {
    return false;
  }

  state.geometry.Type =
      D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  state.geometry.Flags =
      D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  state.geometry.Triangles.VertexBuffer.StartAddress =
      state.vertices->GetGPUVirtualAddress();
  state.geometry.Triangles.VertexBuffer.StrideInBytes =
      sizeof(RtVertex);
  state.geometry.Triangles.VertexCount =
      static_cast<UINT>(vertices.size());
  state.geometry.Triangles.VertexFormat =
      DXGI_FORMAT_R32G32B32_FLOAT;
  state.geometry.Triangles.IndexBuffer =
      state.indices->GetGPUVirtualAddress();
  state.geometry.Triangles.IndexCount =
      static_cast<UINT>(indices.size());
  state.geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

  state.blas_inputs.Type =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  state.blas_inputs.DescsLayout =
      D3D12_ELEMENTS_LAYOUT_ARRAY;
  state.blas_inputs.NumDescs = 1;
  state.blas_inputs.pGeometryDescs = &state.geometry;
  state.blas_inputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO
      blas_info{};
  state.device->GetRaytracingAccelerationStructurePrebuildInfo(
      &state.blas_inputs, &blas_info);
  state.blas = CreateBuffer(
      state.device.Get(), Align(blas_info.ResultDataMaxSizeInBytes, 256),
      D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  if (!state.blas) {
    return false;
  }

  D3D12_RAYTRACING_INSTANCE_DESC instance{};
  instance.Transform[0][0] = 1.0f;
  instance.Transform[1][1] = 1.0f;
  instance.Transform[2][2] = 1.0f;
  instance.InstanceMask = 0xff;
  instance.AccelerationStructure =
      state.blas->GetGPUVirtualAddress();
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instance_values{
      instance};
  state.instances =
      CreateUpload(state.device.Get(), instance_values);
  if (!state.instances) {
    return false;
  }

  state.tlas_inputs.Type =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  state.tlas_inputs.DescsLayout =
      D3D12_ELEMENTS_LAYOUT_ARRAY;
  state.tlas_inputs.NumDescs = 1;
  state.tlas_inputs.InstanceDescs =
      state.instances->GetGPUVirtualAddress();
  state.tlas_inputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO
      tlas_info{};
  state.device->GetRaytracingAccelerationStructurePrebuildInfo(
      &state.tlas_inputs, &tlas_info);
  state.tlas = CreateBuffer(
      state.device.Get(), Align(tlas_info.ResultDataMaxSizeInBytes, 256),
      D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  const std::uint64_t scratch_size = std::max(
      blas_info.ScratchDataSizeInBytes,
      tlas_info.ScratchDataSizeInBytes);
  state.scratch = CreateBuffer(
      state.device.Get(), Align(scratch_size, 256),
      D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!state.tlas || !state.scratch ||
      !CreateRootAndPipeline(state)) {
    return false;
  }

  std::memcpy(state.origin, origin, sizeof(state.origin));
  state.triangle_count =
      static_cast<std::uint32_t>(indices.size() / 3);
  state.initialized = true;
  REXLOG_INFO(
      "raytraced-mirror: DXR 1.1 initialized ({} triangles)",
      state.triangle_count);
  return true;
}

bool WriteUpload(ID3D12Resource* resource, const void* data,
                 std::size_t bytes) {
  if (resource == nullptr || data == nullptr || bytes == 0) {
    return false;
  }
  void* mapping = nullptr;
  if (FAILED(resource->Map(0, nullptr, &mapping))) {
    return false;
  }
  std::memcpy(mapping, data, bytes);
  resource->Unmap(0, nullptr);
  return true;
}

bool EnsureUploadCapacity(ID3D12Device* device,
                          ComPtr<ID3D12Resource>& resource,
                          std::uint64_t& capacity,
                          std::uint64_t required) {
  if (resource && capacity >= required) {
    return true;
  }
  const std::uint64_t grown =
      Align(std::max(required, capacity + capacity / 2), 65536);
  resource = CreateBuffer(
      device, grown, D3D12_HEAP_TYPE_UPLOAD,
      D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!resource) {
    capacity = 0;
    return false;
  }
  capacity = grown;
  return true;
}

bool EnsureAsCapacity(ID3D12Device* device,
                      ComPtr<ID3D12Resource>& resource,
                      std::uint64_t& capacity,
                      std::uint64_t required,
                      D3D12_RESOURCE_STATES state) {
  if (resource && capacity >= required) {
    return true;
  }
  const std::uint64_t grown =
      Align(std::max(required, capacity + capacity / 2), 65536);
  resource = CreateBuffer(
      device, grown, D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, state);
  if (!resource) {
    capacity = 0;
    return false;
  }
  capacity = grown;
  return true;
}

DynamicSlot* PrepareDynamicSlot(
    State& state, nrhi::Device* rhi_device,
    const RaytracedDynamicScene& dynamic_scene) {
  if (dynamic_scene.vertices.empty() ||
      dynamic_scene.indices.empty() ||
      dynamic_scene.indices.size() % 3 != 0 ||
      dynamic_scene.triangle_materials.size() !=
          dynamic_scene.indices.size() / 3) {
    state.dynamic_triangle_count = 0;
    return nullptr;
  }
  const std::uint64_t completed =
      rhi_device->CompletedSubmission();
  DynamicSlot* slot = nullptr;
  for (DynamicSlot& candidate : state.dynamic_slots) {
    if (candidate.last_submission == 0 ||
        candidate.last_submission < completed) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    static std::atomic<std::uint32_t> exhausted{0};
    const std::uint32_t count =
        exhausted.fetch_add(1, std::memory_order_relaxed);
    if (count < 8 || (count & 255u) == 0) {
      REXLOG_WARN(
          "raytraced-mirror: all dynamic AS slots are in flight; "
          "rendering static reflection this frame");
    }
    state.dynamic_triangle_count = 0;
    return nullptr;
  }

  const std::uint64_t vertex_bytes =
      dynamic_scene.vertices.size() *
      sizeof(RaytracedDynamicVertex);
  const std::uint64_t index_bytes =
      dynamic_scene.indices.size() * sizeof(std::uint32_t);
  const std::uint64_t triangle_material_bytes =
      dynamic_scene.triangle_materials.size() *
      sizeof(RaytracedDynamicMaterial);
  DynamicMetadataHeader metadata_header;
  metadata_header.lighting_offset =
      static_cast<std::uint32_t>(
          Align(sizeof(DynamicMetadataHeader) +
                    triangle_material_bytes,
                16));
  metadata_header.moving_light_offset =
      static_cast<std::uint32_t>(
          Align(metadata_header.lighting_offset +
                    dynamic_scene.character_lighting.size() *
                        sizeof(RaytracedCharacterLighting),
                16));
  metadata_header.moving_light_count =
      static_cast<std::uint32_t>(
          std::min<std::size_t>(
              dynamic_scene.moving_lights.size(), 4));
  const std::uint64_t material_bytes =
      metadata_header.moving_light_offset +
      metadata_header.moving_light_count *
          sizeof(RaytracedMovingLight);
  std::vector<std::uint8_t> metadata(
      static_cast<std::size_t>(material_bytes), 0);
  std::memcpy(
      metadata.data(), &metadata_header,
      sizeof(metadata_header));
  std::memcpy(
      metadata.data() + metadata_header.material_offset,
      dynamic_scene.triangle_materials.data(),
      static_cast<std::size_t>(triangle_material_bytes));
  if (!dynamic_scene.character_lighting.empty()) {
    std::memcpy(
        metadata.data() + metadata_header.lighting_offset,
        dynamic_scene.character_lighting.data(),
        dynamic_scene.character_lighting.size() *
            sizeof(RaytracedCharacterLighting));
  }
  if (metadata_header.moving_light_count != 0) {
    std::memcpy(
        metadata.data() + metadata_header.moving_light_offset,
        dynamic_scene.moving_lights.data(),
        metadata_header.moving_light_count *
            sizeof(RaytracedMovingLight));
  }
  if (!EnsureUploadCapacity(
          state.device.Get(), slot->vertices,
          slot->vertex_capacity, vertex_bytes) ||
      !EnsureUploadCapacity(
          state.device.Get(), slot->indices,
          slot->index_capacity, index_bytes) ||
      !EnsureUploadCapacity(
          state.device.Get(), slot->materials,
          slot->material_capacity, material_bytes) ||
      !WriteUpload(slot->vertices.Get(),
                   dynamic_scene.vertices.data(), vertex_bytes) ||
      !WriteUpload(slot->indices.Get(),
                   dynamic_scene.indices.data(), index_bytes) ||
      !WriteUpload(slot->materials.Get(),
                   metadata.data(),
                   material_bytes)) {
    return nullptr;
  }

  if (slot->descriptor_heap == nullptr) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors =
        1 + kRaytracedDynamicTextureLimit;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(state.device->CreateDescriptorHeap(
            &heap, IID_PPV_ARGS(&slot->descriptor_heap)))) {
      return nullptr;
    }
  }
  const UINT descriptor_increment =
      state.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE descriptor =
      slot->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav{};
  output_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  output_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  state.device->CreateUnorderedAccessView(
      state.output_resource, nullptr, &output_uav, descriptor);
  descriptor.ptr += descriptor_increment;
  slot->texture_resources.clear();
  slot->texture_resources.reserve(
      dynamic_scene.diffuse_textures.size());
  D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
  null_srv.Shader4ComponentMapping =
      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_srv.Texture2D.MipLevels = 1;
  for (std::uint32_t index = 0;
       index < kRaytracedDynamicTextureLimit; ++index) {
    bool copied = false;
    if (index < dynamic_scene.diffuse_textures.size()) {
      const auto& binding =
          dynamic_scene.diffuse_textures[index];
      ID3D12Resource* resource =
          d3d12::NativeRhiD3D12GetTextureResource(
              binding.texture);
      copied =
          resource != nullptr &&
          d3d12::NativeRhiD3D12CopyTextureViewDescriptor(
              rhi_device, binding.view, descriptor);
      if (copied &&
          std::find(slot->texture_resources.begin(),
                    slot->texture_resources.end(),
                    resource) ==
              slot->texture_resources.end()) {
        slot->texture_resources.push_back(resource);
      }
    }
    if (!copied) {
      state.device->CreateShaderResourceView(
          nullptr, &null_srv, descriptor);
    }
    descriptor.ptr += descriptor_increment;
  }

  slot->geometry = {};
  slot->geometry.Type =
      D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  slot->geometry.Flags =
      D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  slot->geometry.Triangles.VertexBuffer.StartAddress =
      slot->vertices->GetGPUVirtualAddress();
  slot->geometry.Triangles.VertexBuffer.StrideInBytes =
      sizeof(RaytracedDynamicVertex);
  slot->geometry.Triangles.VertexCount =
      static_cast<UINT>(dynamic_scene.vertices.size());
  slot->geometry.Triangles.VertexFormat =
      DXGI_FORMAT_R32G32B32_FLOAT;
  slot->geometry.Triangles.IndexBuffer =
      slot->indices->GetGPUVirtualAddress();
  slot->geometry.Triangles.IndexCount =
      static_cast<UINT>(dynamic_scene.indices.size());
  slot->geometry.Triangles.IndexFormat =
      DXGI_FORMAT_R32_UINT;

  slot->blas_inputs = {};
  slot->blas_inputs.Type =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  slot->blas_inputs.DescsLayout =
      D3D12_ELEMENTS_LAYOUT_ARRAY;
  slot->blas_inputs.NumDescs = 1;
  slot->blas_inputs.pGeometryDescs = &slot->geometry;
  slot->blas_inputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO
      blas_info{};
  state.device->GetRaytracingAccelerationStructurePrebuildInfo(
      &slot->blas_inputs, &blas_info);
  if (!EnsureAsCapacity(
          state.device.Get(), slot->blas,
          slot->blas_capacity,
          Align(blas_info.ResultDataMaxSizeInBytes, 256),
          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) {
    return nullptr;
  }

  std::array<D3D12_RAYTRACING_INSTANCE_DESC, 2> instances{};
  instances[0].Transform[0][0] = 1.0f;
  instances[0].Transform[1][1] = 1.0f;
  instances[0].Transform[2][2] = 1.0f;
  instances[0].InstanceID = 0;
  instances[0].InstanceMask = 0xff;
  instances[0].AccelerationStructure =
      state.blas->GetGPUVirtualAddress();
  instances[1].Transform[0][0] = 1.0f;
  instances[1].Transform[1][1] = 1.0f;
  instances[1].Transform[2][2] = 1.0f;
  instances[1].InstanceID = 1;
  instances[1].InstanceMask = 0xff;
  instances[1].AccelerationStructure =
      slot->blas->GetGPUVirtualAddress();
  const std::uint64_t instance_bytes = sizeof(instances);
  std::uint64_t unused_instance_capacity =
      slot->instances ? instance_bytes : 0;
  if (!EnsureUploadCapacity(
          state.device.Get(), slot->instances,
          unused_instance_capacity, instance_bytes) ||
      !WriteUpload(
          slot->instances.Get(), instances.data(), instance_bytes)) {
    return nullptr;
  }

  slot->tlas_inputs = {};
  slot->tlas_inputs.Type =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  slot->tlas_inputs.DescsLayout =
      D3D12_ELEMENTS_LAYOUT_ARRAY;
  slot->tlas_inputs.NumDescs = 2;
  slot->tlas_inputs.InstanceDescs =
      slot->instances->GetGPUVirtualAddress();
  slot->tlas_inputs.Flags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO
      tlas_info{};
  state.device->GetRaytracingAccelerationStructurePrebuildInfo(
      &slot->tlas_inputs, &tlas_info);
  const std::uint64_t scratch_required =
      Align(std::max(
          blas_info.ScratchDataSizeInBytes,
          tlas_info.ScratchDataSizeInBytes), 256);
  if (!EnsureAsCapacity(
          state.device.Get(), slot->scratch,
          slot->scratch_capacity, scratch_required,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
      !EnsureAsCapacity(
          state.device.Get(), slot->tlas,
          slot->tlas_capacity,
          Align(tlas_info.ResultDataMaxSizeInBytes, 256),
          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) {
    return nullptr;
  }
  slot->triangle_count =
      static_cast<std::uint32_t>(
          dynamic_scene.indices.size() / 3);
  state.dynamic_triangle_count = slot->triangle_count;
  return slot;
}

bool EnsureOutput(
    State& state,
    const rex::graphics::NativeGuestOutputRenderContext& context) {
  const std::uint32_t width =
      std::max(1u, context.guest_output_width);
  const std::uint32_t height =
      std::max(1u, context.guest_output_height);
  if (state.output != nullptr &&
      state.output_width == width &&
      state.output_height == height) {
    return true;
  }
  if (state.output_srv != nullptr) {
    context.device->DestroyDeferred(state.output_srv);
    state.output_srv = nullptr;
  }
  if (state.output != nullptr) {
    context.device->DestroyDeferred(state.output);
    state.output = nullptr;
  }
  nrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nrhi::Format::kR16G16B16A16_FLOAT;
  desc.usage = nrhi::kTextureUsageUnorderedAccess;
  desc.initial_state = nrhi::ResourceState::kPixelShaderResource;
  state.output = context.device->CreateTexture(desc);
  if (state.output == nullptr) {
    return false;
  }
  nrhi::TextureViewDesc view_desc;
  state.output_srv =
      context.device->CreateTextureView(state.output, view_desc);
  state.output_resource =
      d3d12::NativeRhiD3D12GetTextureResource(state.output);
  if (state.output_srv == nullptr || state.output_resource == nullptr) {
    return false;
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  state.device->CreateUnorderedAccessView(
      state.output_resource, nullptr, &uav,
      state.uav_heap->GetCPUDescriptorHandleForHeapStart());
  state.output_width = width;
  state.output_height = height;
  return true;
}

constexpr char kCompositeShader[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 view_projection;
  float4 mirror_center_half_width;
  float4 mirror_right_half_height;
  float4 mirror_up_unused;
  float4 screen_size;
};
Texture2D<float4> reflection_texture : register(t0);
SamplerState reflection_sampler : register(s0);
struct VOut {
  float4 position : SV_Position;
};
VOut vs_main(uint id : SV_VertexID) {
  static const float2 corners[6] = {
    float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
  };
  const float2 corner = corners[id];
  const float3 world =
      mirror_center_half_width.xyz +
      normalize(mirror_right_half_height.xyz) *
          corner.x * mirror_center_half_width.w +
      normalize(mirror_up_unused.xyz) *
          corner.y * mirror_right_half_height.w;
  VOut output;
  output.position = mul(float4(world, 1.0), view_projection);
  return output;
}
float4 ps_main(VOut input) : SV_Target {
  const float2 uv = input.position.xy * screen_size.zw;
  const float4 reflection = reflection_texture.SampleLevel(
      reflection_sampler, uv, 0.0);
  // The ray pass writes alpha only where the camera ray actually crosses
  // the authored rectangle. Clipping invalid texels is essential near the
  // camera/frustum plane: raster clipping can conservatively cover pixels
  // outside the analytical mirror, and opaque black there would erase the
  // already-rendered scene.
  clip(reflection.a - 0.01);
  return reflection;
}
)";

bool EnsureCompositePipeline(
    State& state,
    const rex::graphics::NativeGuestOutputRenderContext& context,
    nrhi::Format color_format, std::uint32_t sample_count) {
  if (state.composite_pso != nullptr &&
      state.composite_format == color_format &&
      state.composite_samples == sample_count) {
    return true;
  }
  if (state.composite_pso != nullptr) {
    context.device->DestroyDeferred(state.composite_pso);
    state.composite_pso = nullptr;
  }
  if (state.composite_layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.params[0] = {
        nrhi::BindingParamKind::kConstants, 0,
        kCompositeConstantCount, nrhi::Visibility::kAll};
    layout.params[1] = {
        nrhi::BindingParamKind::kTextureTable, 0, 1,
        nrhi::Visibility::kPixel};
    layout.param_count = 2;
    layout.static_samplers[0] = {
        0, nrhi::Filter::kLinear, nrhi::AddressMode::kClamp, 1};
    layout.static_sampler_count = 1;
    layout.allow_input_layout = false;
    state.composite_layout =
        context.device->CreateBindingLayout(layout);
  }
  if (state.composite_layout == nullptr) {
    return false;
  }
  nrhi::ShaderDesc shader;
  shader.name = "raytraced mirror composite";
  shader.hlsl_source = kCompositeShader;
  shader.entry_point = "vs_main";
  shader.stage = nrhi::ShaderStage::kVertex;
  nrhi::Shader* vertex = context.device->CreateShader(shader);
  shader.entry_point = "ps_main";
  shader.stage = nrhi::ShaderStage::kPixel;
  nrhi::Shader* pixel = context.device->CreateShader(shader);
  if (vertex == nullptr || pixel == nullptr) {
    context.device->DestroyDeferred(vertex);
    context.device->DestroyDeferred(pixel);
    return false;
  }
  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = state.composite_layout;
  pipeline.vs = vertex;
  pipeline.ps = pixel;
  pipeline.depth.test_enable = true;
  pipeline.depth.write_enable = false;
  pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
  pipeline.blend.enable = true;
  pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op = nrhi::BlendOp::kAdd;
  pipeline.blend.src_alpha = nrhi::BlendFactor::kOne;
  pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
  pipeline.cull = nrhi::CullMode::kNone;
  pipeline.rtv_format = color_format;
  pipeline.dsv_format = nrhi::Format::kD32_FLOAT;
  pipeline.sample_count = sample_count;
  state.composite_pso =
      context.device->CreateGraphicsPipeline(pipeline);
  context.device->DestroyDeferred(vertex);
  context.device->DestroyDeferred(pixel);
  state.composite_format = color_format;
  state.composite_samples = sample_count;
  return state.composite_pso != nullptr;
}

struct DispatchPayload {
  State* state;
  DynamicSlot* dynamic_slot;
  float constants[kMirrorConstantCount];
  std::uint32_t width;
  std::uint32_t height;
  bool build_acceleration_structures;
  bool build_dynamic_acceleration_structure;
};

void ExecuteDispatch(
    ID3D12GraphicsCommandList* command_list,
    ID3D12GraphicsCommandList1*,
    const void* payload_bytes, std::size_t payload_size) {
  if (payload_size != sizeof(DispatchPayload)) {
    return;
  }
  const auto& payload =
      *static_cast<const DispatchPayload*>(payload_bytes);
  State& state = *payload.state;
  DynamicSlot* dynamic = payload.dynamic_slot;
  ComPtr<ID3D12GraphicsCommandList4> list;
  if (FAILED(command_list->QueryInterface(
          IID_PPV_ARGS(&list)))) {
    return;
  }
  if (payload.build_acceleration_structures) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = state.blas_inputs;
    build.ScratchAccelerationStructureData =
        state.scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData =
        state.blas->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(
        &build, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    // A global UAV barrier is required because the same scratch allocation
    // is reused immediately for TLAS construction. A BLAS-only barrier does
    // not order the preceding scratch writes before that reuse.
    barrier.UAV.pResource = nullptr;
    command_list->ResourceBarrier(1, &barrier);

    build = {};
    build.Inputs = state.tlas_inputs;
    build.ScratchAccelerationStructureData =
        state.scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData =
        state.tlas->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(
        &build, 0, nullptr);
    barrier.UAV.pResource = state.tlas.Get();
    command_list->ResourceBarrier(1, &barrier);
  }
  if (dynamic != nullptr &&
      payload.build_dynamic_acceleration_structure) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = dynamic->blas_inputs;
    build.ScratchAccelerationStructureData =
        dynamic->scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData =
        dynamic->blas->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(
        &build, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    command_list->ResourceBarrier(1, &barrier);

    build = {};
    build.Inputs = dynamic->tlas_inputs;
    build.ScratchAccelerationStructureData =
        dynamic->scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData =
        dynamic->tlas->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(
        &build, 0, nullptr);
    barrier.UAV.pResource = dynamic->tlas.Get();
    command_list->ResourceBarrier(1, &barrier);
  }

  D3D12_RESOURCE_BARRIER output_barrier{};
  output_barrier.Type =
      D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  output_barrier.Transition.pResource = state.output_resource;
  output_barrier.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  output_barrier.Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  output_barrier.Transition.StateAfter =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  command_list->ResourceBarrier(1, &output_barrier);

  std::vector<D3D12_RESOURCE_BARRIER> texture_barriers;
  if (dynamic != nullptr &&
      !dynamic->texture_resources.empty()) {
    texture_barriers.reserve(
        dynamic->texture_resources.size());
    for (ID3D12Resource* texture :
         dynamic->texture_resources) {
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type =
          D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = texture;
      barrier.Transition.Subresource =
          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore =
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      barrier.Transition.StateAfter =
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      texture_barriers.push_back(barrier);
    }
    command_list->ResourceBarrier(
        static_cast<UINT>(texture_barriers.size()),
        texture_barriers.data());
  }

  ID3D12DescriptorHeap* active_heap =
      dynamic != nullptr &&
              dynamic->descriptor_heap != nullptr
          ? dynamic->descriptor_heap.Get()
          : state.uav_heap.Get();
  ID3D12DescriptorHeap* heaps[] = {active_heap};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(
      state.compute_root.Get());
  command_list->SetPipelineState(state.compute_pso.Get());
  command_list->SetComputeRoot32BitConstants(
      0, kMirrorConstantCount, payload.constants, 0);
  command_list->SetComputeRootShaderResourceView(
      1, dynamic != nullptr
             ? dynamic->tlas->GetGPUVirtualAddress()
             : state.tlas->GetGPUVirtualAddress());
  command_list->SetComputeRootShaderResourceView(
      2, state.vertices->GetGPUVirtualAddress());
  command_list->SetComputeRootShaderResourceView(
      3, state.indices->GetGPUVirtualAddress());
  command_list->SetComputeRootShaderResourceView(
      4, state.materials->GetGPUVirtualAddress());
  command_list->SetComputeRootShaderResourceView(
      5, dynamic != nullptr
             ? dynamic->vertices->GetGPUVirtualAddress()
             : state.vertices->GetGPUVirtualAddress());
  command_list->SetComputeRootShaderResourceView(
      6, dynamic != nullptr
             ? dynamic->indices->GetGPUVirtualAddress()
             : state.indices->GetGPUVirtualAddress());
  command_list->SetComputeRootShaderResourceView(
      7, dynamic != nullptr
             ? dynamic->materials->GetGPUVirtualAddress()
             : state.materials->GetGPUVirtualAddress());
  command_list->SetComputeRootDescriptorTable(
      8, active_heap->GetGPUDescriptorHandleForHeapStart());
  D3D12_GPU_DESCRIPTOR_HANDLE texture_table =
      active_heap->GetGPUDescriptorHandleForHeapStart();
  texture_table.ptr +=
      state.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  command_list->SetComputeRootDescriptorTable(
      9, texture_table);
  command_list->Dispatch(
      (payload.width + 7) / 8,
      (payload.height + 7) / 8, 1);

  if (!texture_barriers.empty()) {
    for (D3D12_RESOURCE_BARRIER& barrier :
         texture_barriers) {
      std::swap(
          barrier.Transition.StateBefore,
          barrier.Transition.StateAfter);
    }
    command_list->ResourceBarrier(
        static_cast<UINT>(texture_barriers.size()),
        texture_barriers.data());
  }

  D3D12_RESOURCE_BARRIER uav_barrier{};
  uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barrier.UAV.pResource = state.output_resource;
  command_list->ResourceBarrier(1, &uav_barrier);
  std::swap(output_barrier.Transition.StateBefore,
            output_barrier.Transition.StateAfter);
  command_list->ResourceBarrier(1, &output_barrier);
}

void FillReflectorConstants(
    const FrameScene& scene,
    const skate::world::Vec3& center,
    const skate::world::Vec3& right,
    const skate::world::Vec3& up,
    float half_width, float half_height,
    float reflector_kind, float reflectivity,
    float ripple_strength, float weather_time,
    const float origin[3],
    const RaytracedDynamicScene* dynamic_scene,
    float constants[kMirrorConstantCount]) {
  std::fill(constants, constants + kMirrorConstantCount, 0.0f);
  Invert4x4(scene.view_proj, constants);
  constants[16] = scene.cam_pos[0];
  constants[17] = scene.cam_pos[1];
  constants[18] = scene.cam_pos[2];
  constants[19] = weather_time;
  constants[20] = center.x + origin[0];
  constants[21] = center.y + origin[1];
  constants[22] = center.z + origin[2];
  constants[23] = half_width;
  constants[24] = right.x;
  constants[25] = right.y;
  constants[26] = right.z;
  constants[27] = half_height;
  constants[28] = up.x;
  constants[29] = up.y;
  constants[30] = up.z;
  constants[31] = reflector_kind + ripple_strength;
  constants[35] =
      dynamic_scene != nullptr
          ? static_cast<float>(std::min<std::size_t>(
                dynamic_scene->moving_lights.size(), 4))
          : 0.0f;
  const skate::world::DayNightState celestial =
      mechanics_sandbox::map::ActiveDayNightState();
  constants[32] = celestial.sky_zenith.x;
  constants[33] = celestial.sky_zenith.y;
  constants[34] = celestial.sky_zenith.z;
  constants[36] = celestial.sky_horizon.x;
  constants[37] = celestial.sky_horizon.y;
  constants[38] = celestial.sky_horizon.z;
  constants[39] = reflectivity;
  float light_direction[3] = {
      celestial.light_direction_to_light.x,
      celestial.light_direction_to_light.y,
      celestial.light_direction_to_light.z};
  constants[40] = light_direction[0];
  constants[41] = light_direction[1];
  constants[42] = light_direction[2];
  constants[43] = celestial.ambient;
  constants[44] = celestial.light_color.x;
  constants[45] = celestial.light_color.y;
  constants[46] = celestial.light_color.z;
  constants[47] = celestial.light_intensity;
}

struct ReflectorPlane {
  skate::world::Vec3 center;
  skate::world::Vec3 right;
  skate::world::Vec3 up;
  float half_width = 1.0f;
  float half_height = 1.0f;
  float kind = 0.0f;
  float reflectivity = 1.0f;
  float ripple_strength = 0.0f;
};

}  // namespace

bool RenderRaytracedMirror(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    nrhi::Cmd* cmd, const FrameScene& scene,
    nrhi::Texture* scene_color, nrhi::Texture* scene_depth,
    std::uint32_t scene_sample_count,
    const RaytracedDynamicScene* dynamic_scene) {
  if (cmd == nullptr || scene_color == nullptr ||
      scene_depth == nullptr ||
      context.device->backend() != nrhi::Backend::kD3D12 ||
      !mechanics_sandbox::Active()) {
    return false;
  }
  const skate::world::MapDefinition& definition =
      mechanics_sandbox::map::ActiveDefinition();
  if (definition.raytraced_mirrors.empty() &&
      definition.raytraced_puddles.empty()) {
    return false;
  }
  float origin[3] = {};
  if (!mechanics_sandbox::SandboxMapRenderOrigin(origin)) {
    return false;
  }
  State& state = MirrorState();
  if (!state.initialized &&
      !InitializeAccelerationStructure(
          state, context.device, origin)) {
    if (!state.failure_logged) {
      state.failure_logged = true;
      REXLOG_WARN(
          "raytraced-mirror: initialization failed; mirror disabled");
    }
    return false;
  }
  if (!EnsureOutput(state, context) ||
      !EnsureCompositePipeline(
          state, context, scene_color->format(),
          scene_sample_count)) {
    return false;
  }
  if (!state.placement_logged &&
      !definition.raytraced_mirrors.empty()) {
    const skate::world::RaytracedMirror& mirror =
        definition.raytraced_mirrors.front();
    state.placement_logged = true;
    const float center[4] = {
        mirror.center.x + origin[0],
        mirror.center.y + origin[1],
        mirror.center.z + origin[2], 1.0f};
    float clip[4] = {};
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        clip[column] +=
            center[row] * scene.view_proj[row * 4 + column];
      }
    }
    REXLOG_INFO(
        "raytraced-mirror: placement camera=({:.3f},{:.3f},{:.3f}) "
        "center=({:.3f},{:.3f},{:.3f}) clip=({:.3f},{:.3f},{:.3f},{:.3f})",
        scene.cam_pos[0], scene.cam_pos[1], scene.cam_pos[2],
        center[0], center[1], center[2],
        clip[0], clip[1], clip[2], clip[3]);
    for (int corner_index = 0; corner_index < 4; ++corner_index) {
      const float side =
          (corner_index & 1) != 0 ? 1.0f : -1.0f;
      const float vertical =
          (corner_index & 2) != 0 ? 1.0f : -1.0f;
      const float corner[4] = {
          center[0] + mirror.right.x * mirror.half_width * side +
              mirror.up.x * mirror.half_height * vertical,
          center[1] + mirror.right.y * mirror.half_width * side +
              mirror.up.y * mirror.half_height * vertical,
          center[2] + mirror.right.z * mirror.half_width * side +
              mirror.up.z * mirror.half_height * vertical,
          1.0f};
      float corner_clip[4] = {};
      for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
          corner_clip[column] += corner[row] *
              scene.view_proj[row * 4 + column];
        }
      }
      REXLOG_INFO(
          "raytraced-mirror: corner {} clip=({:.3f},{:.3f},{:.3f},{:.3f}) "
          "ndc=({:.3f},{:.3f})",
          corner_index, corner_clip[0], corner_clip[1],
          corner_clip[2], corner_clip[3],
          corner_clip[0] / corner_clip[3],
          corner_clip[1] / corner_clip[3]);
    }
  }
  DynamicSlot* dynamic_slot =
      dynamic_scene != nullptr
          ? PrepareDynamicSlot(
                state, context.device, *dynamic_scene)
          : nullptr;
  if (dynamic_slot != nullptr) {
    dynamic_slot->last_submission =
        std::max<std::uint64_t>(
            1, context.device->CurrentSubmission());
  }

  std::vector<ReflectorPlane> reflectors;
  reflectors.reserve(
      definition.raytraced_mirrors.size() +
      definition.raytraced_puddles.size());
  for (const skate::world::RaytracedMirror& mirror :
       definition.raytraced_mirrors) {
    reflectors.push_back({
        mirror.center, mirror.right, mirror.up,
        mirror.half_width, mirror.half_height,
        0.0f, 1.0f, 0.0f});
  }
  const float weather_time =
      mechanics_sandbox::map::ActiveDayNightState()
          .elapsed_seconds;
  const float rain_amount =
      definition.weather.enabled
          ? definition.weather.rain_intensity
          : 0.0f;
  for (const skate::world::RaytracedPuddle& puddle :
       definition.raytraced_puddles) {
    reflectors.push_back({
        puddle.center, puddle.right, puddle.forward,
        puddle.half_width, puddle.half_length,
        1.0f, puddle.reflectivity,
        puddle.ripple_strength * rain_amount});
  }

  bool first_reflector = true;
  for (const ReflectorPlane& reflector : reflectors) {
    DispatchPayload dispatch{};
    dispatch.state = &state;
    dispatch.dynamic_slot = dynamic_slot;
    dispatch.width = state.output_width;
    dispatch.height = state.output_height;
    dispatch.build_acceleration_structures =
        !state.build_recorded && first_reflector;
    dispatch.build_dynamic_acceleration_structure =
        dynamic_slot != nullptr && first_reflector;
    FillReflectorConstants(
        scene,
        reflector.center, reflector.right, reflector.up,
        reflector.half_width, reflector.half_height,
        reflector.kind, reflector.reflectivity,
        reflector.ripple_strength, weather_time,
        origin, dynamic_scene, dispatch.constants);
    d3d12::NativeRhiD3D12RecordExtension(
        cmd, ExecuteDispatch, &dispatch, sizeof(dispatch));
    state.build_recorded = true;
    ++state.dispatches;

    // The raw D3D12 compute extension changes descriptor heaps and pipeline
    // state, so establish the NRHI raster state again for every reflector.
    cmd->SetRenderTargets(scene_color, scene_depth);
    cmd->SetBindingLayout(state.composite_layout);
    cmd->SetPipeline(state.composite_pso);
    cmd->SetTexture(1, state.output_srv);
    cmd->SetPrimitiveTopology(
        nrhi::PrimitiveTopology::kTriangleList);

    float composite[kCompositeConstantCount] = {};
    std::memcpy(
        composite, scene.view_proj, sizeof(float) * 16);
    composite[16] = reflector.center.x + origin[0];
    composite[17] = reflector.center.y + origin[1];
    composite[18] = reflector.center.z + origin[2];
    composite[19] = reflector.half_width;
    composite[20] = reflector.right.x;
    composite[21] = reflector.right.y;
    composite[22] = reflector.right.z;
    composite[23] = reflector.half_height;
    composite[24] = reflector.up.x;
    composite[25] = reflector.up.y;
    composite[26] = reflector.up.z;
    composite[28] =
        static_cast<float>(context.guest_output_width);
    composite[29] =
        static_cast<float>(context.guest_output_height);
    composite[30] =
        1.0f / static_cast<float>(
                   context.guest_output_width);
    composite[31] =
        1.0f / static_cast<float>(
                   context.guest_output_height);
    cmd->SetRootConstants(
        0, kCompositeConstantCount, composite, 0);
    cmd->Draw(6, 0);
    first_reflector = false;
  }
  return true;
}

RaytracedMirrorTelemetry GetRaytracedMirrorTelemetry() {
  const State& state = MirrorState();
  RaytracedMirrorTelemetry telemetry;
  telemetry.supported = state.supported;
  telemetry.initialized = state.initialized;
  telemetry.acceleration_structure_recorded =
      state.build_recorded;
  telemetry.dispatches = state.dispatches;
  telemetry.width = state.output_width;
  telemetry.height = state.output_height;
  telemetry.triangle_count = state.triangle_count;
  telemetry.dynamic_triangle_count =
      state.dynamic_triangle_count;
  if (mechanics_sandbox::Active()) {
    const skate::world::MapDefinition& definition =
        mechanics_sandbox::map::ActiveDefinition();
    telemetry.reflector_count =
        static_cast<std::uint32_t>(
            definition.raytraced_mirrors.size() +
            definition.raytraced_puddles.size());
    telemetry.puddle_count =
        static_cast<std::uint32_t>(
            definition.raytraced_puddles.size());
  }
  return telemetry;
}

}  // namespace skate3::native_scene
