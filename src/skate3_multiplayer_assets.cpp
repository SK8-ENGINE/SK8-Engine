#include "skate3_multiplayer_assets.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    skate3_multiplayer_cac_asset_root, "", "Skate 3/Multiplayer",
    "Path to a complete, locally extracted "
    "createacharacter/model/cas_db directory. Incomplete fixed-size memory "
    "snapshots are rejected because they omit clothing texture data.");
REXCVAR_DEFINE_STRING(
    skate3_multiplayer_local_profile_recipe, "", "Skate 3/Multiplayer",
    "Optional path to the local SKATER.P save whose validated cas_db recipe "
    "is used for live wardrobe appearance changes. Only the compact recipe "
    "is read; no other save data enters multiplayer.");

namespace skate3::multiplayer_assets {
namespace {

constexpr std::array<std::uint8_t, 12> kRx2Magic = {
    0x89, 'R', 'W', '4', 'x', 'b', '2', 0x00, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint32_t kTypeModelData = 0x00EB0001;
constexpr std::uint32_t kTypeMeshDesc = 0x00EB0023;
constexpr std::uint32_t kTypeRawBuffer = 0x00010031;
constexpr std::uint32_t kTypeVertexDesc = 0x000200E9;
constexpr std::uint32_t kTypeVertexBufferDesc = 0x000200EA;
constexpr std::uint32_t kTypeIndexBufferDesc = 0x000200EB;

struct Section {
  std::uint32_t index = 0;
  std::uint32_t offset = 0;
  std::uint32_t file_offset = 0;
  std::uint32_t size = 0;
  std::uint32_t type = 0;
};

struct VertexElement {
  std::uint16_t offset = 0;
  std::uint32_t format = 0;
  std::uint8_t usage = 0;
  std::uint8_t usage_index = 0;
};

struct ParsedLayout {
  std::vector<std::uint8_t> bytes;
  std::vector<Section> sections;
  const Section* model = nullptr;
  const Section* mesh = nullptr;
  const Section* vertex_desc = nullptr;
  const Section* vertex_buffer_desc = nullptr;
  const Section* index_buffer_desc = nullptr;
  const Section* vertex_buffer = nullptr;
  const Section* index_buffer = nullptr;
  std::vector<VertexElement> elements;
  std::uint32_t stride = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t index_count = 0;
};

struct Signature {
  std::uint32_t vertex_count = 0;
  std::uint32_t index_count = 0;
  std::uint64_t topology_hash = 0;

  bool operator==(const Signature&) const = default;
};

struct SignatureHash {
  std::size_t operator()(const Signature& value) const noexcept {
    std::uint64_t hash = value.topology_hash;
    hash ^= std::uint64_t(value.vertex_count) << 32;
    hash ^= value.index_count;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    return static_cast<std::size_t>(hash);
  }
};

struct CategoryIndex {
  bool scanned = false;
  std::unordered_map<Signature, std::filesystem::path, SignatureHash> matches;
  std::unordered_map<Signature, std::size_t, SignatureHash> collisions;
};

std::mutex g_mutex;
std::filesystem::path g_indexed_root;
CategoryIndex g_outer_torso;
CategoryIndex g_hair;
std::unordered_map<Signature, BindMesh, SignatureHash> g_loaded;
std::unordered_map<std::uint64_t, BindMesh> g_recipe_meshes;
std::unordered_map<std::uint64_t, RecipeTexture> g_recipe_textures;

struct ProfileRecipePollState {
  std::filesystem::path path;
  std::filesystem::file_time_type accepted_write_time{};
  std::uintmax_t accepted_file_size = 0;
  std::chrono::steady_clock::time_point last_poll{};
  bool accepted = false;
};

ProfileRecipePollState g_profile_recipe_poll;

bool RangeValid(
    const std::vector<std::uint8_t>& bytes, std::size_t offset,
    std::size_t size) {
  return offset <= bytes.size() && size <= bytes.size() - offset;
}

std::uint16_t ReadBe16(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (!RangeValid(bytes, offset, 2)) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      (std::uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
}

std::int16_t ReadBeS16(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

std::uint32_t ReadBe32(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (!RangeValid(bytes, offset, 4)) {
    return 0;
  }
  return (std::uint32_t(bytes[offset]) << 24) |
         (std::uint32_t(bytes[offset + 1]) << 16) |
         (std::uint32_t(bytes[offset + 2]) << 8) |
         std::uint32_t(bytes[offset + 3]);
}

float ReadBeFloat(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return std::bit_cast<float>(ReadBe32(bytes, offset));
}

std::uint64_t HashIndices(
    const std::vector<std::uint16_t>& indices) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto* bytes =
      reinterpret_cast<const std::uint8_t*>(indices.data());
  for (std::size_t index = 0;
       index < indices.size() * sizeof(std::uint16_t); ++index) {
    hash = (hash ^ bytes[index]) * 1099511628211ull;
  }
  return hash;
}

bool ReadFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return false;
  }
  const std::streamoff length = stream.tellg();
  if (length <= 0 || length > 32 * 1024 * 1024) {
    return false;
  }
  bytes.resize(static_cast<std::size_t>(length));
  stream.seekg(0, std::ios::beg);
  return bool(stream.read(
      reinterpret_cast<char*>(bytes.data()), length));
}

const Section* FindSection(
    const std::vector<Section>& sections, std::uint32_t type) {
  const auto found = std::find_if(
      sections.begin(), sections.end(),
      [type](const Section& section) {
        return section.type == type;
      });
  return found == sections.end() ? nullptr : &*found;
}

const Section* FindRawBySize(
    const std::vector<Section>& sections, std::uint32_t size,
    const Section* exclude) {
  const auto found = std::find_if(
      sections.begin(), sections.end(),
      [size, exclude](const Section& section) {
        return &section != exclude &&
               section.type == kTypeRawBuffer &&
               section.size == size;
      });
  return found == sections.end() ? nullptr : &*found;
}

bool ParseLayout(
    const std::filesystem::path& path, ParsedLayout& output) {
  output = {};
  if (!ReadFile(path, output.bytes) ||
      output.bytes.size() < 0x100 ||
      !std::equal(
          kRx2Magic.begin(), kRx2Magic.end(),
          output.bytes.begin())) {
    return false;
  }
  const std::uint32_t section_count =
      ReadBe32(output.bytes, 0x20);
  const std::uint32_t section_table =
      ReadBe32(output.bytes, 0x30);
  const std::uint32_t gpu_arena =
      ReadBe32(output.bytes, 0x44);
  if (section_count == 0 || section_count > 128 ||
      !RangeValid(
          output.bytes, section_table,
          std::size_t(section_count) * 24)) {
    return false;
  }
  output.sections.reserve(section_count);
  for (std::uint32_t index = 0; index < section_count; ++index) {
    const std::size_t record =
        std::size_t(section_table) + std::size_t(index) * 24;
    Section section;
    section.index = index;
    section.offset = ReadBe32(output.bytes, record);
    section.size = ReadBe32(output.bytes, record + 8);
    section.type = ReadBe32(output.bytes, record + 20);
    section.file_offset =
        section.type == kTypeRawBuffer
            ? gpu_arena + section.offset
            : section.offset;
    if (!RangeValid(
            output.bytes, section.file_offset,
            section.size)) {
      // Some extracted CAC assets retain descriptors for lower-detail LODs
      // whose raw GPU buffers were not included in the fixed-size extraction.
      // The primary/high-detail mesh is still complete, so ignore only those
      // unavailable raw buffers and let descriptor matching select a complete
      // buffer pair below.
      if (section.type == kTypeRawBuffer) {
        continue;
      }
      return false;
    }
    output.sections.push_back(section);
  }
  output.model = FindSection(output.sections, kTypeModelData);
  output.mesh = FindSection(output.sections, kTypeMeshDesc);
  output.vertex_desc =
      FindSection(output.sections, kTypeVertexDesc);
  output.vertex_buffer_desc =
      FindSection(output.sections, kTypeVertexBufferDesc);
  output.index_buffer_desc =
      FindSection(output.sections, kTypeIndexBufferDesc);
  if (output.model == nullptr || output.mesh == nullptr ||
      output.vertex_desc == nullptr ||
      output.vertex_buffer_desc == nullptr ||
      output.index_buffer_desc == nullptr) {
    return false;
  }
  const std::uint32_t vertex_bytes = ReadBe32(
      output.bytes, output.vertex_buffer_desc->file_offset + 0x20);
  const std::uint32_t index_bytes = ReadBe32(
      output.bytes, output.index_buffer_desc->file_offset + 0x1C);
  output.index_count = ReadBe32(
      output.bytes, output.index_buffer_desc->file_offset + 0x20);
  output.vertex_buffer =
      FindRawBySize(output.sections, vertex_bytes, nullptr);
  output.index_buffer =
      FindRawBySize(
          output.sections, index_bytes, output.vertex_buffer);
  if (output.vertex_buffer == nullptr ||
      output.index_buffer == nullptr ||
      output.index_count == 0 ||
      output.index_count > 4 * 1024 * 1024 ||
      std::size_t(output.index_count) * 2 >
          output.index_buffer->size) {
    return false;
  }
  const std::size_t descriptor =
      output.vertex_desc->file_offset;
  const std::uint16_t element_count =
      ReadBe16(output.bytes, descriptor + 8);
  if (element_count == 0 || element_count > 16 ||
      !RangeValid(
          output.bytes, descriptor + 0x10,
          std::size_t(element_count) * 16 +
              element_count)) {
    return false;
  }
  output.elements.reserve(element_count);
  for (std::uint16_t index = 0;
       index < element_count; ++index) {
    const std::size_t element =
        descriptor + 0x10 + std::size_t(index) * 16;
    VertexElement parsed;
    parsed.offset = ReadBe16(output.bytes, element + 2);
    parsed.format = ReadBe32(output.bytes, element + 4);
    parsed.usage = output.bytes[element + 9];
    parsed.usage_index = output.bytes[element + 10];
    output.elements.push_back(parsed);
  }
  output.stride = output.bytes[
      descriptor + 0x10 + std::size_t(element_count) * 16];
  if (output.stride == 0 ||
      vertex_bytes % output.stride != 0) {
    return false;
  }
  output.vertex_count = vertex_bytes / output.stride;
  return output.vertex_count != 0;
}

bool DecodeIndices(
    const ParsedLayout& parsed,
    std::vector<std::uint16_t>& indices) {
  if (parsed.index_buffer == nullptr ||
      std::size_t(parsed.index_count) * 2 >
          parsed.index_buffer->size) {
    return false;
  }
  indices.resize(parsed.index_count);
  for (std::uint32_t index = 0;
       index < parsed.index_count; ++index) {
    indices[index] = ReadBe16(
        parsed.bytes,
        parsed.index_buffer->file_offset +
            std::size_t(index) * 2);
    if (indices[index] >= parsed.vertex_count) {
      return false;
    }
  }
  return true;
}

const VertexElement* FindElement(
    const ParsedLayout& parsed, std::uint8_t usage,
    std::uint8_t usage_index = 0) {
  const auto found = std::find_if(
      parsed.elements.begin(), parsed.elements.end(),
      [usage, usage_index](const VertexElement& element) {
        return element.usage == usage &&
               element.usage_index == usage_index;
      });
  return found == parsed.elements.end() ? nullptr : &*found;
}

std::array<float, 3> DecodePackedVector(std::uint32_t packed) {
  const auto sign_extend = [](
                               std::uint32_t value,
                               unsigned bits) {
    const std::uint32_t sign = 1u << (bits - 1);
    return static_cast<std::int32_t>(
        (value ^ sign) - sign);
  };
  return {
      float(sign_extend(packed & 0x7FFu, 11)) / 1023.0f,
      float(sign_extend((packed >> 11) & 0x7FFu, 11)) / 1023.0f,
      float(sign_extend((packed >> 22) & 0x3FFu, 10)) / 511.0f};
}

std::string NormalizeBoneName(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (unsigned char character : value) {
    if (character >= 'a' && character <= 'z') {
      normalized.push_back(
          static_cast<char>(character - 'a' + 'A'));
    } else {
      normalized.push_back(static_cast<char>(character));
    }
  }
  return normalized;
}

std::optional<std::uint16_t> CanonicalBoneIndex(
    std::string_view name) {
  // Absolute CAC/ABIN hierarchy order. RX2 palettes are name-keyed views
  // into this skeleton, exactly as the retail Sk8::RemapBones routine does.
  static constexpr std::array<std::string_view, 131> kNames = {
      "TRAJECTORY", "HIPS", "SPINE", "SPINE1", "SPINE2", "SPINE3",
      "NECK", "NECK1", "HEAD",
      "RIGHTSHOULDER", "RIGHTARM", "RIGHTFOREARM", "RIGHTHAND",
      "LEFTSHOULDER", "LEFTARM", "LEFTFOREARM", "LEFTHAND",
      "RIGHTUPLEG", "RIGHTLEG", "RIGHTFOOT", "RIGHTTOEBASE",
      "LEFTUPLEG", "LEFTLEG", "LEFTFOOT", "LEFTTOEBASE",
      "SKATEBOARD_ROOT", "TRUCK_FRONT", "RIGHT_WHEELFRONT",
      "LEFT_WHEELFRONT", "TRUCK_BACK", "LEFT_WHEELBACK",
      "RIGHT_WHEELBACK",
      "LEFTHANDTHUMB1", "LEFTHANDTHUMB2", "LEFTHANDTHUMB3",
      "LEFTHANDINDEX1", "LEFTHANDINDEX2", "LEFTHANDINDEX3",
      "LEFTHANDMIDDLE1", "LEFTHANDMIDDLE2", "LEFTHANDMIDDLE3",
      "LEFTINHANDRING", "LEFTHANDRING1", "LEFTHANDRING2",
      "LEFTHANDRING3", "LEFTINHANDPINKY", "LEFTHANDPINKY1",
      "LEFTHANDPINKY2", "LEFTHANDPINKY3",
      "RIGHTHANDTHUMB1", "RIGHTHANDTHUMB2", "RIGHTHANDTHUMB3",
      "RIGHTHANDINDEX1", "RIGHTHANDINDEX2", "RIGHTHANDINDEX3",
      "RIGHTHANDMIDDLE1", "RIGHTHANDMIDDLE2", "RIGHTHANDMIDDLE3",
      "RIGHTINHANDRING", "RIGHTHANDRING1", "RIGHTHANDRING2",
      "RIGHTHANDRING3", "RIGHTINHANDPINKY", "RIGHTHANDPINKY1",
      "RIGHTHANDPINKY2", "RIGHTHANDPINKY3",
      "RIGHTTOEBASE_REPARENTED", "LEFTTOEBASE_REPARENTED",
      "RIGHTHAND_REPARENTED", "LEFTHAND_REPARENTED",
      "RIGHTSHOULDERHLP", "RIGHTARMTWIST", "RIGHTFOREARMTWIST",
      "RIGHTFOREARMTWIST1", "LEFTSHOULDERHLP", "LEFTARMTWIST",
      "LEFTFOREARMTWIST", "LEFTFOREARMTWIST1", "RIGHTUPLEGHLP",
      "RIGHTUPLEGTWIST", "LEFTUPLEGHLP", "LEFTUPLEGTWIST",
      "FACE", "OFFSET_JAW", "JAW", "OFFSET_CHIN", "CHIN",
      "OFFSET_LOWERLIP", "OFFSET_LEFTLOWERLIP",
      "OFFSET_RIGHTLOWERLIP", "OFFSET_TONGUE", "OFFSET_LEFTCHEEK",
      "OFFSET_LEFTEYE", "OFFSET_LEFTMOUTH", "OFFSET_LEFTUPCHEEK",
      "OFFSET_LEFTUPPERLIP", "OFFSET_RIGHTCHEEK", "OFFSET_RIGHTEYE",
      "OFFSET_RIGHTMOUTH", "OFFSET_RIGHTUPCHEEK",
      "OFFSET_RIGHTUPPERLIP", "OFFSET_UPPERLIP", "TONGUE",
      "OFFSET_TONGUETIP", "LEFTLOWERLIP", "LOWERLIP",
      "RIGHTLOWERLIP", "TONGUETIP", "LEFTCHEEK", "LEFTCREASE",
      "LEFTEYE", "LEFTINNEREYEBROW", "LEFTLOWEYELID", "LEFTMOUTH",
      "LEFTNOSE", "LEFTOUTEREYEBROW", "LEFTUPCHEEK",
      "LEFTUPEYELID", "LEFTUPPERLIP", "RIGHTCHEEK",
      "RIGHTCREASE", "RIGHTEYE", "RIGHTINNEREYEBROW",
      "RIGHTLOWEYELID", "RIGHTMOUTH", "RIGHTNOSE",
      "RIGHTOUTEREYEBROW", "RIGHTUPCHEEK", "RIGHTUPEYELID",
      "RIGHTUPPERLIP", "UPPERLIP"};
  const std::string normalized = NormalizeBoneName(name);
  const auto found =
      std::find(kNames.begin(), kNames.end(), normalized);
  if (found == kNames.end()) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(
      std::distance(kNames.begin(), found));
}

bool DecodePalette(
    const ParsedLayout& parsed,
    std::vector<std::uint16_t>& remap) {
  const std::size_t model = parsed.model->file_offset;
  const std::uint32_t name_table =
      ReadBe32(parsed.bytes, model + 0x28);
  const std::uint16_t bone_count =
      ReadBe16(parsed.bytes, model + 0x34);
  const std::size_t mesh = parsed.mesh->file_offset;
  const std::uint32_t used_count =
      ReadBe32(parsed.bytes, mesh + 0x48);
  const std::uint32_t used_offset =
      ReadBe32(parsed.bytes, mesh + 0x4C);
  if (bone_count == 0 || bone_count > 256 ||
      used_count == 0 || used_count > 256 ||
      !RangeValid(
          parsed.bytes, model + name_table,
          std::size_t(bone_count) * 4) ||
      !RangeValid(
          parsed.bytes, mesh + used_offset,
          std::size_t(used_count) * 2)) {
    return false;
  }
  remap.clear();
  remap.reserve(used_count);
  for (std::uint32_t slot = 0; slot < used_count; ++slot) {
    const std::uint16_t model_bone = ReadBe16(
        parsed.bytes, mesh + used_offset + slot * 2);
    if (model_bone >= bone_count) {
      return false;
    }
    const std::uint32_t name_offset = ReadBe32(
        parsed.bytes, model + name_table +
                          std::size_t(model_bone) * 4);
    const std::size_t name_begin = model + name_offset;
    if (name_begin >= parsed.bytes.size()) {
      return false;
    }
    std::size_t name_end = name_begin;
    while (name_end < parsed.bytes.size() &&
           parsed.bytes[name_end] != 0 &&
           name_end - name_begin < 128) {
      ++name_end;
    }
    if (name_end >= parsed.bytes.size() ||
        name_end == name_begin) {
      return false;
    }
    const std::string_view name(
        reinterpret_cast<const char*>(
            parsed.bytes.data() + name_begin),
        name_end - name_begin);
    const std::optional<std::uint16_t> canonical =
        CanonicalBoneIndex(name);
    if (!canonical.has_value()) {
      REXLOG_WARN(
          "multiplayer-assets: RX2 bone '{}' has no canonical CAC index",
          name);
      return false;
    }
    remap.push_back(*canonical);
  }
  return true;
}

bool DecodeBindMesh(
    const std::filesystem::path& path, ParsedLayout& parsed,
    BindMesh& output) {
  const VertexElement* position =
      FindElement(parsed, 0);
  const VertexElement* weights =
      FindElement(parsed, 1);
  const VertexElement* bone_indices =
      FindElement(parsed, 2);
  const VertexElement* uv0 =
      FindElement(parsed, 5, 0);
  const VertexElement* uv1 =
      FindElement(parsed, 5, 1);
  const VertexElement* tangent =
      FindElement(parsed, 6);
  const VertexElement* binormal =
      FindElement(parsed, 7);
  if (position == nullptr || weights == nullptr ||
      bone_indices == nullptr || uv0 == nullptr ||
      tangent == nullptr || binormal == nullptr ||
      position->format != 0x001A215A ||
      weights->format != 0x001A2286 ||
      bone_indices->format != 0x001A2286 ||
      tangent->format != 0x002A2190 ||
      binormal->format != 0x002A2190 ||
      (uv0->format != 0x002C2159 &&
       uv0->format != 0x002C23A5)) {
    return false;
  }
  if (!DecodeIndices(parsed, output.indices) ||
      !DecodePalette(parsed, output.palette_to_canonical)) {
    return false;
  }
  output.vertices.assign(
      std::size_t(parsed.vertex_count) * 14, 0.0f);
  constexpr float kPositionScale = 2.0f / 32767.0f;
  constexpr float kUvScale = 1.0f / 32767.0f;
  const auto decode_uv =
      [&](const VertexElement& element, std::size_t source,
          float& u, float& v) {
        if (element.format == 0x002C2159) {
          // Xenos k_16_16 signed-normalized.
          u = ReadBeS16(
                  parsed.bytes, source + element.offset) *
              kUvScale;
          v = ReadBeS16(
                  parsed.bytes, source + element.offset + 2) *
              kUvScale;
          return;
        }
        // Xenos k_32_32_FLOAT. ROPA expands this format unchanged into its
        // live float vertex buffer; treating the first four bytes as two
        // signed shorts corrupts hair strand-coverage UVs and exposes the
        // normally transparent cards across the forehead.
        u = ReadBeFloat(
            parsed.bytes, source + element.offset);
        v = ReadBeFloat(
            parsed.bytes, source + element.offset + 4);
      };
  for (std::uint32_t vertex = 0;
       vertex < parsed.vertex_count; ++vertex) {
    const std::size_t source =
        parsed.vertex_buffer->file_offset +
        std::size_t(vertex) * parsed.stride;
    float* destination =
        output.vertices.data() + std::size_t(vertex) * 14;
    destination[0] =
        ReadBeS16(parsed.bytes, source + position->offset) *
        kPositionScale;
    destination[1] =
        ReadBeS16(
            parsed.bytes, source + position->offset + 2) *
            kPositionScale +
        0.8f;
    destination[2] =
        ReadBeS16(
            parsed.bytes, source + position->offset + 4) *
        kPositionScale;
    decode_uv(
        *uv0, source, destination[3], destination[4]);
    if (uv1 != nullptr &&
        (uv1->format == 0x002C2159 ||
         uv1->format == 0x002C23A5)) {
      decode_uv(
          *uv1, source, destination[5], destination[6]);
    }
    const std::uint32_t packed_weights =
        ReadBe32(parsed.bytes, source + weights->offset);
    const std::uint32_t packed_indices =
        ReadBe32(parsed.bytes, source + bone_indices->offset);
    std::memcpy(
        destination + 7, &packed_weights,
        sizeof(packed_weights));
    std::memcpy(
        destination + 8, &packed_indices,
        sizeof(packed_indices));
    const std::array<float, 3> tangent_vector =
        DecodePackedVector(
            ReadBe32(
                parsed.bytes, source + tangent->offset));
    const std::array<float, 3> binormal_vector =
        DecodePackedVector(
            ReadBe32(
                parsed.bytes, source + binormal->offset));
    std::array<float, 3> normal = {
        tangent_vector[1] * binormal_vector[2] -
            tangent_vector[2] * binormal_vector[1],
        tangent_vector[2] * binormal_vector[0] -
            tangent_vector[0] * binormal_vector[2],
        tangent_vector[0] * binormal_vector[1] -
            tangent_vector[1] * binormal_vector[0]};
    const float normal_length = std::sqrt(
        normal[0] * normal[0] +
        normal[1] * normal[1] +
        normal[2] * normal[2]);
    if (normal_length > 1.0e-5f) {
      for (float& component : normal) {
        component /= normal_length;
      }
    } else {
      normal = {0.0f, 1.0f, 0.0f};
    }
    destination[9] = normal[0];
    destination[10] = normal[1];
    destination[11] = normal[2];
    destination[12] = destination[3];
    destination[13] = destination[4];
  }
  const std::size_t mesh = parsed.mesh->file_offset;
  for (std::size_t component = 0; component < 3; ++component) {
    output.bbox_min[component] =
        ReadBeFloat(parsed.bytes, mesh + component * 4);
    output.bbox_max[component] =
        ReadBeFloat(parsed.bytes, mesh + 0x10 + component * 4);
  }
  output.source_path = path;
  const std::string stem = path.stem().string();
  try {
    output.asset_id = std::stoull(stem, nullptr, 16);
  } catch (...) {
    output.asset_id = 0;
  }
  return true;
}

struct ParsedRecipePiece {
  std::string category;
  std::uint64_t asset_id = 0;
  std::uint64_t model_id = 0;
  std::uint64_t material_id = 0;
  std::unordered_map<std::string, std::uint64_t> textures;
};

std::uint64_t ReadBe64(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (!RangeValid(bytes, offset, 8)) {
    return 0;
  }
  return (std::uint64_t(ReadBe32(bytes, offset)) << 32) |
         ReadBe32(bytes, offset + 4);
}

bool ReadRecipeString(
    const std::vector<std::uint8_t>& bytes, std::size_t& cursor,
    std::size_t maximum_length, std::string& output) {
  if (!RangeValid(bytes, cursor, 4)) {
    return false;
  }
  const std::uint32_t length = ReadBe32(bytes, cursor);
  cursor += 4;
  if (length == 0 || length > maximum_length ||
      !RangeValid(bytes, cursor, length)) {
    return false;
  }
  output.assign(
      reinterpret_cast<const char*>(bytes.data() + cursor),
      length);
  cursor += length;
  return std::all_of(
      output.begin(), output.end(),
      [](unsigned char character) {
        return character >= 0x20 && character <= 0x7E;
      });
}

bool ParseRecipe(
    const std::vector<std::uint8_t>& bytes,
    std::vector<ParsedRecipePiece>& pieces,
    std::size_t& structural_bytes,
    std::uint8_t& gender) {
  static constexpr std::array<std::uint8_t, 26> kHeader = {
      0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06,
      'c',  'a',  's',  '_',  'd',  'b',  0x00, 0x00,
      0x00, 0x03, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00,
      0x00, 0x02};
  if (bytes.size() < kHeader.size() ||
      !std::equal(kHeader.begin(), kHeader.end(), bytes.begin())) {
    return false;
  }
  const std::uint32_t recipe_name_length = ReadBe32(bytes, 4);
  if (recipe_name_length != 6 ||
      !RangeValid(bytes, 8, recipe_name_length + 16)) {
    return false;
  }
  std::size_t cursor = 24 + recipe_name_length;
  const std::uint32_t list_count = ReadBe32(bytes, cursor - 4);
  if (list_count == 0 || list_count > 32) {
    return false;
  }
  pieces.clear();
  for (std::uint32_t list_index = 0;
       list_index < list_count; ++list_index) {
    std::string category;
    if (!ReadRecipeString(bytes, cursor, 48, category) ||
        !RangeValid(bytes, cursor, 4)) {
      return false;
    }
    const std::uint32_t asset_count = ReadBe32(bytes, cursor);
    cursor += 4;
    if (asset_count == 0 || asset_count > 16) {
      return false;
    }
    for (std::uint32_t asset_index = 0;
         asset_index < asset_count; ++asset_index) {
      if (!RangeValid(bytes, cursor, 12)) {
        return false;
      }
      const std::uint64_t asset_id = ReadBe64(bytes, cursor);
      const std::uint32_t model_count =
          ReadBe32(bytes, cursor + 8);
      cursor += 12;
      if (model_count == 0 || model_count > 8) {
        return false;
      }
      for (std::uint32_t model_index = 0;
           model_index < model_count; ++model_index) {
        if (!RangeValid(bytes, cursor, 37)) {
          return false;
        }
        const std::uint8_t lod = bytes[cursor];
        const std::uint64_t model_id =
            ReadBe64(bytes, cursor + 9);
        const std::uint32_t texture_loops =
            ReadBe32(bytes, cursor + 17);
        const std::uint64_t material_id =
            ReadBe64(bytes, cursor + 25);
        std::uint32_t texture_count =
            ReadBe32(bytes, cursor + 33);
        cursor += 37;
        if (texture_loops == 0 || texture_loops > 8 ||
            texture_count > 32) {
          return false;
        }
        ParsedRecipePiece parsed;
        parsed.category = category;
        parsed.asset_id = asset_id;
        parsed.model_id = model_id;
        parsed.material_id = material_id;
        for (std::uint32_t texture_index = 0;
             texture_index < texture_count; ++texture_index) {
          std::string channel;
          if (!ReadRecipeString(bytes, cursor, 48, channel) ||
              !RangeValid(bytes, cursor, 8)) {
            return false;
          }
          parsed.textures.emplace(
              std::move(channel), ReadBe64(bytes, cursor));
          cursor += 8;
        }
        for (std::uint32_t loop = 1;
             loop < texture_loops; ++loop) {
          if (!RangeValid(bytes, cursor, 16)) {
            return false;
          }
          cursor += 16;
          texture_count = ReadBe32(bytes, cursor - 4);
          if (texture_count > 32) {
            return false;
          }
          for (std::uint32_t texture_index = 0;
               texture_index < texture_count; ++texture_index) {
            std::string ignored_channel;
            if (!ReadRecipeString(
                    bytes, cursor, 48, ignored_channel) ||
                !RangeValid(bytes, cursor, 8)) {
              return false;
            }
            cursor += 8;
          }
        }
        // LOD 0 is the normal high-detail presentation model. LOD 2 is
        // intentionally omitted; it carries the same material bindings and
        // would otherwise duplicate every recipe piece.
        if (lod == 0 && model_id != 0) {
          pieces.push_back(std::move(parsed));
        }
      }
    }
  }
  structural_bytes = cursor;
  if (pieces.empty() || pieces.size() > 64) {
    return false;
  }

  // Full CAC recipes append 01 00000006, gender, mirrored RGB-block counts,
  // then 44-byte tint records. This metadata is not required to locate
  // assets, but validating it prevents a truncated network payload from
  // being accepted as a complete recipe.
  gender = 0;
  if (!RangeValid(bytes, cursor, 14) ||
      bytes[cursor] != 1 ||
      ReadBe32(bytes, cursor + 1) != 6) {
    return false;
  }
  gender = bytes[cursor + 5];
  if (gender > 1) {
    return false;
  }
  const std::uint32_t rgb_count_a =
      ReadBe32(bytes, cursor + 6);
  const std::uint32_t rgb_count_b =
      ReadBe32(bytes, cursor + 10);
  if (rgb_count_a != rgb_count_b || rgb_count_a > 64) {
    return false;
  }
  cursor += 14;
  const std::size_t rgb_bytes =
      std::size_t(rgb_count_a) * 44;
  if (!RangeValid(bytes, cursor, rgb_bytes)) {
    return false;
  }
  cursor += rgb_bytes;
  structural_bytes = cursor;
  return true;
}

bool ExtractProfileRecipe(
    const std::vector<std::uint8_t>& profile_prefix,
    std::vector<std::uint8_t>& recipe) {
  static constexpr std::array<std::uint8_t, 26> kHeader = {
      0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06,
      'c',  'a',  's',  '_',  'd',  'b',  0x00, 0x00,
      0x00, 0x03, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00,
      0x00, 0x02};
  auto search = profile_prefix.begin();
  while (search != profile_prefix.end()) {
    const auto found = std::search(
        search, profile_prefix.end(),
        kHeader.begin(), kHeader.end());
    if (found == profile_prefix.end()) {
      return false;
    }
    std::vector<std::uint8_t> candidate(
        found, profile_prefix.end());
    std::vector<ParsedRecipePiece> parsed_pieces;
    std::size_t structural_bytes = 0;
    std::uint8_t gender = 0;
    if (ParseRecipe(
            candidate, parsed_pieces, structural_bytes, gender) &&
        structural_bytes >= kHeader.size() &&
        structural_bytes <= candidate.size()) {
      candidate.resize(structural_bytes);
      recipe = std::move(candidate);
      return true;
    }
    search = std::next(found);
  }
  return false;
}

std::string AssetFileName(std::uint64_t id) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::nouppercase
         << std::setw(16) << std::setfill('0') << id << ".rx2";
  return stream.str();
}

std::uint32_t X360TiledX(
    std::uint32_t offset, std::uint32_t width_units,
    std::uint32_t texel_pitch) {
  const std::uint32_t aligned_width =
      (width_units + 31u) & ~31u;
  const std::uint32_t log_bpp =
      (texel_pitch >> 2) +
      ((texel_pitch >> 1) >> (texel_pitch >> 2));
  const std::uint32_t offset_b = offset << log_bpp;
  const std::uint32_t offset_t =
      ((offset_b & ~4095u) >> 3) +
      ((offset_b & 1792u) >> 2) +
      (offset_b & 63u);
  const std::uint32_t offset_m =
      offset_t >> (7 + log_bpp);
  const std::uint32_t macro_x =
      (offset_m % (aligned_width >> 5)) << 2;
  const std::uint32_t tile =
      (((offset_t >> (5 + log_bpp)) & 2u) +
       (offset_b >> 6)) &
      3u;
  const std::uint32_t macro = (macro_x + tile) << 3;
  const std::uint32_t micro =
      ((((offset_t >> 1) & ~15u) + (offset_t & 15u)) &
       ((texel_pitch << 3) - 1u)) >>
      log_bpp;
  return macro + micro;
}

std::uint32_t X360TiledY(
    std::uint32_t offset, std::uint32_t width_units,
    std::uint32_t texel_pitch) {
  const std::uint32_t aligned_width =
      (width_units + 31u) & ~31u;
  const std::uint32_t log_bpp =
      (texel_pitch >> 2) +
      ((texel_pitch >> 1) >> (texel_pitch >> 2));
  const std::uint32_t offset_b = offset << log_bpp;
  const std::uint32_t offset_t =
      ((offset_b & ~4095u) >> 3) +
      ((offset_b & 1792u) >> 2) +
      (offset_b & 63u);
  const std::uint32_t offset_m =
      offset_t >> (7 + log_bpp);
  const std::uint32_t macro_y =
      (offset_m / (aligned_width >> 5)) << 2;
  const std::uint32_t tile =
      ((offset_t >> (6 + log_bpp)) & 1u) +
      ((offset_b & 2048u) >> 10);
  const std::uint32_t macro = (macro_y + tile) << 3;
  const std::uint32_t micro =
      ((((offset_t &
          (((texel_pitch << 6) - 1u) & ~31u)) +
         ((offset_t & 15u) << 1)) >>
        (3 + log_bpp)) &
       ~1u);
  return macro + micro + ((offset_t & 16u) >> 4);
}

bool DecodeRecipeTexture(
    const std::filesystem::path& path,
    std::uint64_t texture_id,
    RecipeTexture& output) {
  std::vector<std::uint8_t> bytes;
  if (!ReadFile(path, bytes) || bytes.size() < 0x5C ||
      !std::equal(
          kRx2Magic.begin(), kRx2Magic.end(), bytes.begin())) {
    return false;
  }
  const std::uint32_t section_count = ReadBe32(bytes, 0x20);
  const std::uint32_t section_table = ReadBe32(bytes, 0x30);
  const std::uint32_t data_base = ReadBe32(bytes, 0x44);
  if (section_count < 2 || section_count > 128 ||
      !RangeValid(
          bytes, section_table,
          std::size_t(section_count) * 24)) {
    return false;
  }
  constexpr std::uint32_t kTextureInfo = 0x000200E8;
  std::uint32_t data_offset = 0;
  std::uint32_t data_bytes = 0;
  std::uint32_t info_offset = 0;
  bool found = false;
  for (std::uint32_t index = 1;
       index < section_count; ++index) {
    const std::size_t record =
        std::size_t(section_table) + std::size_t(index) * 24;
    if (ReadBe32(bytes, record + 20) != kTextureInfo) {
      continue;
    }
    const std::size_t previous = record - 24;
    data_offset = ReadBe32(bytes, previous);
    data_bytes = ReadBe32(bytes, previous + 8);
    info_offset = ReadBe32(bytes, record);
    found = true;
    break;
  }
  if (!found || !RangeValid(bytes, info_offset, 40)) {
    return false;
  }
  const std::uint8_t file_format = bytes[info_offset + 35];
  const std::uint32_t height =
      (std::uint32_t(bytes[info_offset + 37]) + 1u) * 8u;
  const std::uint32_t width =
      (std::uint32_t(ReadBe16(bytes, info_offset + 38)) + 1u) &
      0x1FFFu;
  std::uint32_t block_bytes = 0;
  TextureFormat format;
  if (file_format == 0x52) {
    block_bytes = 8;
    format = TextureFormat::kBc1;
  } else if (file_format == 0x54) {
    block_bytes = 16;
    format = TextureFormat::kBc3;
  } else {
    REXLOG_WARN(
        "multiplayer-assets: unsupported CAC texture format "
        "id={:016X} format={:02X}",
        texture_id, file_format);
    return false;
  }
  if (width == 0 || height == 0 ||
      width > 4096 || height > 4096) {
    return false;
  }
  const std::uint32_t width_blocks = (width + 3u) >> 2;
  const std::uint32_t height_blocks = (height + 3u) >> 2;
  const std::size_t expected_bytes =
      std::size_t(width_blocks) * height_blocks * block_bytes;
  if (expected_bytes == 0 ||
      expected_bytes > 64u * 1024u * 1024u ||
      std::size_t(data_base) + data_offset >= bytes.size()) {
    return false;
  }
  std::vector<std::uint8_t> tiled(expected_bytes);
  const std::size_t source_offset =
      std::size_t(data_base) + data_offset;
  const std::size_t available =
      bytes.size() - source_offset;
  if (data_bytes < expected_bytes ||
      available < expected_bytes) {
    REXLOG_WARN(
        "multiplayer-assets: incomplete CAC texture "
        "id={:016X} dimensions={}x{} expected={} declared={} "
        "available={} path={}",
        texture_id, width, height, expected_bytes, data_bytes,
        available, path.string());
    return false;
  }
  std::copy_n(
      bytes.begin() + source_offset, expected_bytes,
      tiled.begin());
  std::vector<std::uint8_t> linear(expected_bytes);
  for (std::uint32_t y = 0; y < height_blocks; ++y) {
    for (std::uint32_t x = 0; x < width_blocks; ++x) {
      const std::uint32_t logical =
          y * width_blocks + x;
      const std::uint32_t tiled_x =
          X360TiledX(logical, width_blocks, block_bytes);
      const std::uint32_t tiled_y =
          X360TiledY(logical, width_blocks, block_bytes);
      const std::size_t destination =
          (std::size_t(tiled_y) * width_blocks + tiled_x) *
          block_bytes;
      const std::size_t source =
          std::size_t(logical) * block_bytes;
      if (destination + block_bytes <= linear.size() &&
          source + block_bytes <= tiled.size()) {
        std::copy_n(
            tiled.begin() + source, block_bytes,
            linear.begin() + destination);
      }
    }
  }
  for (std::size_t index = 0;
       index + 1 < linear.size(); index += 2) {
    std::swap(linear[index], linear[index + 1]);
  }
  output = {};
  output.texture_id = texture_id;
  output.format = format;
  output.width = width;
  output.height = height;
  output.row_pitch = width_blocks * block_bytes;
  output.bytes = std::move(linear);
  return true;
}

std::filesystem::path ConfiguredRoot() {
  const std::string configured =
      REXCVAR_GET(skate3_multiplayer_cac_asset_root);
  if (configured.empty()) {
    return {};
  }
  std::error_code error;
  std::filesystem::path root =
      std::filesystem::weakly_canonical(
          std::filesystem::path(configured), error);
  if (error || !std::filesystem::is_directory(root)) {
    return {};
  }
  return root;
}

void ResetForRoot(const std::filesystem::path& root) {
  if (root == g_indexed_root) {
    return;
  }
  g_indexed_root = root;
  g_outer_torso = {};
  g_hair = {};
  g_loaded.clear();
  g_recipe_meshes.clear();
  g_recipe_textures.clear();
}

void ScanCategory(
    const std::filesystem::path& root,
    std::string_view category, CategoryIndex& index) {
  if (index.scanned) {
    return;
  }
  index.scanned = true;
  const std::filesystem::path directory =
      root / std::string(category);
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    REXLOG_WARN(
        "multiplayer-assets: CAC category is unavailable: {}",
        directory.string());
    return;
  }
  std::size_t parsed_count = 0;
  std::size_t skipped_count = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      break;
    }
    if (!entry.is_regular_file() ||
        entry.path().extension() != ".rx2") {
      continue;
    }
    ParsedLayout parsed;
    std::vector<std::uint16_t> indices;
    if (!ParseLayout(entry.path(), parsed) ||
        !DecodeIndices(parsed, indices)) {
      ++skipped_count;
      continue;
    }
    ++parsed_count;
    const Signature signature = {
        parsed.vertex_count, parsed.index_count,
        HashIndices(indices)};
    const auto [found, inserted] =
        index.matches.emplace(signature, entry.path());
    if (!inserted && found->second != entry.path()) {
      index.collisions[signature] += 1;
    }
  }
  REXLOG_INFO(
      "multiplayer-assets: indexed {} RX2 category={} "
      "signatures={} collisions={} skipped={}",
      parsed_count, category, index.matches.size(),
      index.collisions.size(), skipped_count);
}

}  // namespace

bool ResolveRecipeAppearance(
    const std::vector<std::uint8_t>& recipe,
    bool load_textures,
    RecipeAppearance& output) {
  std::vector<ParsedRecipePiece> parsed_pieces;
  std::size_t structural_bytes = 0;
  std::uint8_t gender = 0;
  if (!ParseRecipe(
          recipe, parsed_pieces, structural_bytes, gender)) {
    REXLOG_WARN(
        "multiplayer-assets: rejected invalid or incomplete cas_db recipe "
        "bytes={}",
        recipe.size());
    return false;
  }
  const std::filesystem::path root = ConfiguredRoot();
  if (root.empty()) {
    REXLOG_WARN(
        "multiplayer-assets: cannot resolve cas_db recipe because the local "
        "CAC asset root is not configured");
    return false;
  }
  std::lock_guard lock(g_mutex);
  ResetForRoot(root);

  RecipeAppearance resolved;
  resolved.gender = gender;
  resolved.structural_bytes = structural_bytes;
  resolved.pieces.reserve(parsed_pieces.size());
  const std::filesystem::path texture_root =
      root.parent_path().parent_path() / "texture";
  const auto texture_for =
      [&resolved, &texture_root, load_textures](
          std::uint64_t texture_id) -> bool {
        if (!load_textures || texture_id == 0 ||
            resolved.textures.contains(texture_id)) {
          return true;
        }
        auto cached = g_recipe_textures.find(texture_id);
        if (cached == g_recipe_textures.end()) {
          RecipeTexture decoded;
          const std::filesystem::path path =
              texture_root / AssetFileName(texture_id);
          if (!DecodeRecipeTexture(path, texture_id, decoded)) {
            REXLOG_WARN(
                "multiplayer-assets: failed to decode recipe texture "
                "id={:016X} path={}",
                texture_id, path.string());
            return false;
          }
          cached = g_recipe_textures.emplace(
              texture_id, std::move(decoded)).first;
        }
        resolved.textures.emplace(texture_id, cached->second);
        return true;
      };
  const auto channel =
      [](const ParsedRecipePiece& piece,
         std::string_view name) {
        const auto found =
            piece.textures.find(std::string(name));
        return found == piece.textures.end()
                   ? std::uint64_t{0}
                   : found->second;
      };
  for (const ParsedRecipePiece& parsed : parsed_pieces) {
    RecipePiece piece;
    piece.category = parsed.category;
    piece.asset_id = parsed.asset_id;
    piece.model_id = parsed.model_id;
    piece.material_id = parsed.material_id;
    piece.diffuse_texture = channel(parsed, "diffuse");
    piece.normal_texture = channel(parsed, "normal");
    piece.alpha_texture = channel(parsed, "alpha");
    piece.specular_texture = channel(parsed, "specular");

    auto cached_mesh = g_recipe_meshes.find(parsed.model_id);
    if (cached_mesh == g_recipe_meshes.end()) {
      const std::filesystem::path path =
          root / parsed.category /
          AssetFileName(parsed.model_id);
      ParsedLayout layout;
      BindMesh mesh;
      if (!ParseLayout(path, layout) ||
          !DecodeBindMesh(path, layout, mesh)) {
        REXLOG_WARN(
            "multiplayer-assets: failed to decode recipe model "
            "category={} id={:016X} path={}",
            parsed.category, parsed.model_id, path.string());
        continue;
      }
      cached_mesh = g_recipe_meshes.emplace(
          parsed.model_id, std::move(mesh)).first;
    }
    piece.mesh = cached_mesh->second;
    if (!texture_for(piece.diffuse_texture) ||
        !texture_for(piece.normal_texture) ||
        !texture_for(piece.alpha_texture) ||
        !texture_for(piece.specular_texture)) {
      REXLOG_WARN(
          "multiplayer-assets: recipe piece has an incomplete required "
          "texture category={} model={:016X}",
          piece.category, piece.model_id);
      return false;
    }
    resolved.pieces.push_back(std::move(piece));
  }
  if (resolved.pieces.empty()) {
    return false;
  }
  REXLOG_INFO(
      "multiplayer-assets: resolved cas_db recipe pieces={}/{} "
      "textures={} structural_bytes={} gender={}",
      resolved.pieces.size(), parsed_pieces.size(),
      resolved.textures.size(), resolved.structural_bytes,
      resolved.gender);
  output = std::move(resolved);
  return true;
}

bool PollLocalProfileRecipe(
    std::vector<std::uint8_t>& recipe) {
  constexpr auto kPollInterval =
      std::chrono::milliseconds(500);
  constexpr std::uintmax_t kMaximumProfileBytes =
      8u * 1024u * 1024u;
  constexpr std::size_t kProfilePrefixBytes =
      64u * 1024u;

  const auto now = std::chrono::steady_clock::now();
  if (g_profile_recipe_poll.last_poll !=
          std::chrono::steady_clock::time_point{} &&
      now - g_profile_recipe_poll.last_poll < kPollInterval) {
    return false;
  }
  g_profile_recipe_poll.last_poll = now;

  const std::filesystem::path configured =
      REXCVAR_GET(skate3_multiplayer_local_profile_recipe);
  if (configured.empty()) {
    return false;
  }
  if (g_profile_recipe_poll.path != configured) {
    g_profile_recipe_poll = {};
    g_profile_recipe_poll.path = configured;
    g_profile_recipe_poll.last_poll = now;
  }

  std::error_code error;
  const std::uintmax_t file_size =
      std::filesystem::file_size(configured, error);
  if (error || file_size == 0 ||
      file_size > kMaximumProfileBytes) {
    return false;
  }
  const std::filesystem::file_time_type write_time =
      std::filesystem::last_write_time(configured, error);
  if (error) {
    return false;
  }
  if (g_profile_recipe_poll.accepted &&
      g_profile_recipe_poll.accepted_write_time == write_time &&
      g_profile_recipe_poll.accepted_file_size == file_size) {
    return false;
  }

  const auto read_started =
      std::chrono::steady_clock::now();
  std::ifstream stream(configured, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::vector<std::uint8_t> profile_prefix(
      static_cast<std::size_t>(
          std::min<std::uintmax_t>(
              file_size, kProfilePrefixBytes)));
  if (!stream.read(
          reinterpret_cast<char*>(
              profile_prefix.data()),
          static_cast<std::streamsize>(
              profile_prefix.size()))) {
    return false;
  }

  std::vector<std::uint8_t> updated_recipe;
  if (!ExtractProfileRecipe(
          profile_prefix, updated_recipe)) {
    REXLOG_WARN(
        "multiplayer-assets: local profile changed but no complete "
        "cas_db recipe was available; retaining previous appearance");
    return false;
  }

  g_profile_recipe_poll.accepted = true;
  g_profile_recipe_poll.accepted_write_time = write_time;
  g_profile_recipe_poll.accepted_file_size = file_size;
  if (updated_recipe == recipe) {
    return false;
  }
  recipe = std::move(updated_recipe);
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() -
          read_started)
          .count();
  REXLOG_INFO(
      "multiplayer-assets: adopted local profile recipe "
      "bytes={} read_ms={:.3f}",
      recipe.size(), elapsed_ms);
  return true;
}

bool ResolveRopaBindMesh(
    std::uint8_t character_family,
    std::uint32_t vertex_count,
    std::uint32_t index_count,
    std::uint64_t topology_hash,
    BindMesh& output) {
  const std::filesystem::path root = ConfiguredRoot();
  if (root.empty()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN(
          "multiplayer-assets: local CAC asset root is not configured; "
          "ROPA bind-mesh replacement is unavailable");
    }
    return false;
  }
  std::lock_guard lock(g_mutex);
  ResetForRoot(root);
  CategoryIndex* category_index = nullptr;
  std::string_view category;
  if (character_family == 2) {
    category = "OuterTorso";
    category_index = &g_outer_torso;
  } else if (
      character_family == 4 ||
      character_family == 5) {
    category = "Hair";
    category_index = &g_hair;
  } else {
    return false;
  }
  ScanCategory(root, category, *category_index);
  const Signature signature = {
      vertex_count, index_count, topology_hash};
  const auto loaded = g_loaded.find(signature);
  if (loaded != g_loaded.end()) {
    output = loaded->second;
    return true;
  }
  if (category_index->collisions.contains(signature)) {
    REXLOG_WARN(
        "multiplayer-assets: ambiguous ROPA topology fam={} "
        "verts={} indices={} hash={:016X}",
        character_family, vertex_count, index_count,
        topology_hash);
    return false;
  }
  const auto match = category_index->matches.find(signature);
  if (match == category_index->matches.end()) {
    REXLOG_WARN(
        "multiplayer-assets: no vanilla ROPA bind mesh fam={} "
        "verts={} indices={} hash={:016X}",
        character_family, vertex_count, index_count,
        topology_hash);
    return false;
  }
  ParsedLayout parsed;
  BindMesh resolved;
  if (!ParseLayout(match->second, parsed) ||
      parsed.vertex_count != vertex_count ||
      parsed.index_count != index_count ||
      !DecodeBindMesh(match->second, parsed, resolved)) {
    REXLOG_WARN(
        "multiplayer-assets: failed to decode matched RX2 {}",
        match->second.string());
    return false;
  }
  REXLOG_INFO(
      "multiplayer-assets: resolved ROPA fam={} asset={:016X} "
      "verts={} indices={} bones={} path={}",
      character_family, resolved.asset_id, vertex_count,
      index_count, resolved.palette_to_canonical.size(),
      resolved.source_path.string());
  g_loaded.emplace(signature, resolved);
  output = std::move(resolved);
  return true;
}

}  // namespace skate3::multiplayer_assets
