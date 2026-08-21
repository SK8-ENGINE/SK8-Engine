#include "skate3_multiplayer_assets.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    skate3_multiplayer_cac_asset_root, "", "Skate 3/Multiplayer",
    "Path to a locally extracted createacharacter/model/cas_db directory. "
    "This development bridge is ignored when empty; the retail BIG reader "
    "will replace it without changing the renderer-facing asset interface.");

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
