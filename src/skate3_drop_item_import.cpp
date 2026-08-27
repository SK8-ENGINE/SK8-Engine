#include "skate3_drop_item_import.h"

#include "skate3_cac_archive.h"

#include <skate/world/skate_object_package.h>
#include <skate/world/world_map.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skate3::drop_item_import {
namespace {

using skate::world::CollisionTriangle;
using skate::world::GrindRail;
using skate::world::ImageTexture;
using skate::world::MapObject;
using skate::world::MaterialId;
using skate::world::NativeGrindSegment;
using skate::world::ObjectCollisionShape;
using skate::world::ObjectPhysicsType;
using skate::world::RenderVertex;
using skate::world::RetailRenderFlags;
using skate::world::RetailShaderFamily;
using skate::world::SkateObjectAsset;
using skate::world::SurfaceFlags;
using skate::world::SurfaceMaterial;
using skate::world::TextureColorSpace;
using skate::world::TextureId;
using skate::world::Vec2;
using skate::world::Vec3;

constexpr std::size_t kTocRecordBytes = 24;
constexpr std::uint32_t kTypeVertexBuffer = 0x000200EAu;
constexpr std::uint32_t kTypeIndexBuffer = 0x000200EBu;
constexpr std::uint32_t kTypeMeshInfo = 0x000200E9u;
constexpr std::uint32_t kTypeCollision = 0x00080006u;
constexpr std::uint32_t kTypeSpline = 0x00EB0004u;
constexpr std::uint32_t kTypeTextureInfo = 0x000200E8u;
constexpr std::size_t kMaximumRx2Bytes = 64u * 1024u * 1024u;

bool RangeValid(
    std::size_t available, std::size_t offset, std::size_t size) {
  return offset <= available && size <= available - offset;
}

std::uint16_t ReadBe16(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (!RangeValid(bytes.size(), offset, 2)) {
    throw std::runtime_error("RX2 read exceeds the asset");
  }
  return static_cast<std::uint16_t>(
      (std::uint16_t(bytes[offset]) << 8u) | bytes[offset + 1]);
}

std::int16_t ReadBeS16(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return std::bit_cast<std::int16_t>(ReadBe16(bytes, offset));
}

std::uint32_t ReadBe32(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  if (!RangeValid(bytes.size(), offset, 4)) {
    throw std::runtime_error("RX2 read exceeds the asset");
  }
  return (std::uint32_t(bytes[offset]) << 24u) |
         (std::uint32_t(bytes[offset + 1]) << 16u) |
         (std::uint32_t(bytes[offset + 2]) << 8u) |
         bytes[offset + 3];
}

std::int32_t ReadBeS32(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return std::bit_cast<std::int32_t>(ReadBe32(bytes, offset));
}

std::uint64_t ReadBe64(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return (std::uint64_t(ReadBe32(bytes, offset)) << 32u) |
         ReadBe32(bytes, offset + 4);
}

float ReadBeFloat(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return std::bit_cast<float>(ReadBe32(bytes, offset));
}

float HalfToFloat(std::uint16_t half) {
  const std::uint32_t sign =
      (std::uint32_t(half & 0x8000u)) << 16u;
  std::uint32_t exponent = (half >> 10u) & 0x1Fu;
  std::uint32_t mantissa = half & 0x03FFu;
  std::uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127u - 15u + 1u;
      while ((mantissa & 0x0400u) == 0) {
        mantissa <<= 1u;
        --exponent;
      }
      mantissa &= 0x03FFu;
      bits = sign | (exponent << 23u) | (mantissa << 13u);
    }
  } else if (exponent == 0x1Fu) {
    bits = sign | 0x7F800000u | (mantissa << 13u);
  } else {
    bits =
        sign | ((exponent + (127u - 15u)) << 23u) |
        (mantissa << 13u);
  }
  return std::bit_cast<float>(bits);
}

Vec3 ReadVec3(
    const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return {
      ReadBeFloat(bytes, offset),
      ReadBeFloat(bytes, offset + 4),
      ReadBeFloat(bytes, offset + 8)};
}

std::string Fold(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return value;
}

std::optional<std::uint64_t> ParseId(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 16) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    value <<= 4u;
    if (character >= '0' && character <= '9') {
      value |= static_cast<std::uint64_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      value |= static_cast<std::uint64_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      value |= static_cast<std::uint64_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

std::string HexId(std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << std::nouppercase
         << std::setw(16) << std::setfill('0') << value;
  return output.str();
}

std::string DecodeXmlValue(std::string value) {
  const std::array<std::pair<std::string_view, std::string_view>, 5>
      entities{{
          {"&amp;", "&"},
          {"&quot;", "\""},
          {"&apos;", "'"},
          {"&lt;", "<"},
          {"&gt;", ">"},
      }};
  for (const auto& [encoded, decoded] : entities) {
    std::size_t position = 0;
    while ((position = value.find(encoded, position)) !=
           std::string::npos) {
      value.replace(position, encoded.size(), decoded);
      position += decoded.size();
    }
  }
  return value;
}

struct XmlTag {
  std::string name;
  bool closing = false;
  bool self_closing = false;
  std::unordered_map<std::string, std::string> attributes;
};

bool NextXmlTag(
    std::string_view xml, std::size_t& cursor,
    XmlTag& tag, std::string& error) {
  while (true) {
    const std::size_t begin = xml.find('<', cursor);
    if (begin == std::string_view::npos) {
      return false;
    }
    if (xml.substr(begin).starts_with("<!--")) {
      const std::size_t end = xml.find("-->", begin + 4);
      if (end == std::string_view::npos) {
        error = "retail recipe has an unterminated XML comment";
        return false;
      }
      cursor = end + 3;
      continue;
    }
    const std::size_t end = xml.find('>', begin + 1);
    if (end == std::string_view::npos) {
      error = "retail recipe has an unterminated XML tag";
      return false;
    }
    cursor = end + 1;
    std::string_view body = xml.substr(begin + 1, end - begin - 1);
    if (body.empty() || body.front() == '?' || body.front() == '!') {
      continue;
    }
    tag = {};
    if (body.front() == '/') {
      tag.closing = true;
      body.remove_prefix(1);
    }
    while (!body.empty() &&
           std::isspace(static_cast<unsigned char>(body.back()))) {
      body.remove_suffix(1);
    }
    if (!tag.closing && !body.empty() && body.back() == '/') {
      tag.self_closing = true;
      body.remove_suffix(1);
    }
    while (!body.empty() &&
           std::isspace(static_cast<unsigned char>(body.front()))) {
      body.remove_prefix(1);
    }
    const std::size_t name_end = body.find_first_of(" \t\r\n");
    tag.name = std::string(body.substr(0, name_end));
    if (name_end == std::string_view::npos) {
      return !tag.name.empty();
    }
    body.remove_prefix(name_end);
    while (!body.empty()) {
      while (!body.empty() &&
             std::isspace(static_cast<unsigned char>(body.front()))) {
        body.remove_prefix(1);
      }
      if (body.empty()) {
        break;
      }
      const std::size_t equals = body.find('=');
      if (equals == std::string_view::npos) {
        error = "retail recipe contains a malformed XML attribute";
        return false;
      }
      std::string key(body.substr(0, equals));
      while (!key.empty() &&
             std::isspace(static_cast<unsigned char>(key.back()))) {
        key.pop_back();
      }
      body.remove_prefix(equals + 1);
      while (!body.empty() &&
             std::isspace(static_cast<unsigned char>(body.front()))) {
        body.remove_prefix(1);
      }
      if (body.empty() || (body.front() != '"' && body.front() != '\'')) {
        error = "retail recipe XML attribute is not quoted";
        return false;
      }
      const char quote = body.front();
      body.remove_prefix(1);
      const std::size_t quote_end = body.find(quote);
      if (quote_end == std::string_view::npos) {
        error = "retail recipe XML attribute quote is unterminated";
        return false;
      }
      tag.attributes.emplace(
          std::move(key),
          DecodeXmlValue(std::string(body.substr(0, quote_end))));
      body.remove_prefix(quote_end + 1);
    }
    return !tag.name.empty();
  }
}

const std::string* Attribute(
    const XmlTag& tag, std::string_view name) {
  const auto found = tag.attributes.find(std::string(name));
  return found == tag.attributes.end() ? nullptr : &found->second;
}

bool SafeCategory(std::string_view category) {
  return !category.empty() && category != "." && category != ".." &&
         category.find('/') == std::string_view::npos &&
         category.find('\\') == std::string_view::npos &&
         category.find(':') == std::string_view::npos;
}

struct TocRecord {
  std::size_t table_offset = 0;
  std::array<std::int32_t, 5> values{};
  std::uint32_t type = 0;
};

bool ParseToc(
    const std::vector<std::uint8_t>& bytes,
    std::vector<TocRecord>& records,
    std::uint32_t& data_base,
    std::string& error) {
  records.clear();
  static constexpr std::array<std::uint8_t, 7> kRx2Magic{
      0x89, 'R', 'W', '4', 'x', 'b', '2'};
  if (bytes.size() < 0x5C || bytes.size() > kMaximumRx2Bytes ||
      !std::equal(
          kRx2Magic.begin(), kRx2Magic.end(), bytes.begin())) {
    error = "asset is not an Xbox 360 RW4 RX2 resource";
    return false;
  }
  try {
    const std::uint32_t count = ReadBe32(bytes, 0x20);
    const std::uint32_t table = ReadBe32(bytes, 0x30);
    data_base = ReadBe32(bytes, 0x44);
    if (count == 0 || count > 4096 ||
        !RangeValid(
            bytes.size(), table,
            std::size_t(count) * kTocRecordBytes)) {
      error = "RX2 section table is invalid";
      return false;
    }
    records.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      TocRecord record;
      record.table_offset =
          std::size_t(table) + std::size_t(index) * kTocRecordBytes;
      for (std::size_t value = 0; value < 5; ++value) {
        record.values[value] =
            ReadBeS32(bytes, record.table_offset + value * 4);
      }
      record.type = ReadBe32(bytes, record.table_offset + 20);
      records.push_back(record);
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

struct BufferResource {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t info_offset = 0;
};

enum class AttributeType {
  Float32,
  Float16,
  Int16,
  Uint16,
  Packed,
};

struct VertexAttribute {
  std::uint32_t offset = 0;
  AttributeType type = AttributeType::Float32;
  std::uint32_t components = 0;
};

struct MeshLayout {
  std::uint32_t stride = 0;
  VertexAttribute position;
  std::optional<VertexAttribute> uv0;
  std::optional<VertexAttribute> uv1;
  std::optional<VertexAttribute> tangent;
  std::optional<VertexAttribute> binormal;
};

struct DecodedMesh {
  std::vector<RenderVertex> vertices;
  std::vector<std::uint32_t> indices;
};

std::optional<VertexAttribute> ClassifyAttribute(
    const std::vector<std::uint8_t>& bytes, std::size_t descriptor,
    std::string_view semantic) {
  const std::uint32_t offset = ReadBe32(bytes, descriptor);
  const std::uint32_t format = ReadBe32(bytes, descriptor + 4);
  const std::uint8_t usage = bytes.at(descriptor + 9);
  const std::uint8_t usage_index = bytes.at(descriptor + 10);
  if (semantic == "position" && usage == 0) {
    if (format == 0x002A23B9u) {
      return VertexAttribute{0, AttributeType::Float32, 3};
    }
    if (format == 0x001A2360u) {
      return VertexAttribute{0, AttributeType::Float16, 3};
    }
    if (format == 0x001A23A6u) {
      return VertexAttribute{0, AttributeType::Uint16, 3};
    }
    if (format == 0x001A215Au) {
      return VertexAttribute{0, AttributeType::Int16, 3};
    }
  }
  if (semantic == "uv" && usage == 5) {
    AttributeType type;
    if (format == 0x002C23A5u) {
      type = AttributeType::Float32;
    } else if (
        format == 0x001A2360u || format == 0x002C235Fu) {
      type = AttributeType::Float16;
    } else if (format == 0x002C2159u) {
      type = AttributeType::Int16;
    } else if (format == 0x002C2059u) {
      type = AttributeType::Uint16;
    } else {
      return std::nullopt;
    }
    return VertexAttribute{offset, type, 2};
  }
  if (semantic == "tangent" && format == 0x002A2190u &&
      usage == 6 && usage_index == 0) {
    return VertexAttribute{offset, AttributeType::Packed, 3};
  }
  if (semantic == "binormal" && format == 0x002A2190u &&
      usage == 7 && usage_index == 0) {
    return VertexAttribute{offset, AttributeType::Packed, 3};
  }
  return std::nullopt;
}

std::array<float, 3> DecodePackedVector(std::uint32_t packed) {
  const auto sign_extend = [](
                               std::uint32_t value,
                               unsigned bits) {
    const std::uint32_t sign = 1u << (bits - 1u);
    return static_cast<std::int32_t>((value ^ sign) - sign);
  };
  return {
      float(sign_extend(packed & 0x7FFu, 11)) / 1023.0f,
      float(sign_extend((packed >> 11u) & 0x7FFu, 11)) /
          1023.0f,
      float(sign_extend((packed >> 22u) & 0x3FFu, 10)) /
          511.0f};
}

float ReadAttributeComponent(
    const std::vector<std::uint8_t>& bytes,
    std::size_t source, const VertexAttribute& attribute,
    std::size_t component) {
  const std::size_t offset = source + attribute.offset;
  switch (attribute.type) {
    case AttributeType::Float32:
      return ReadBeFloat(bytes, offset + component * 4);
    case AttributeType::Float16:
      return HalfToFloat(ReadBe16(bytes, offset + component * 2));
    case AttributeType::Int16:
      return float(ReadBeS16(bytes, offset + component * 2)) /
             32768.0f;
    case AttributeType::Uint16:
      return float(ReadBe16(bytes, offset + component * 2)) /
             65535.0f;
    case AttributeType::Packed:
      break;
  }
  throw std::runtime_error("unsupported RX2 vertex attribute");
}

bool ParseMeshLayout(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset, MeshLayout& layout,
    std::string& error) {
  try {
    if (!RangeValid(bytes.size(), offset, 16)) {
      throw std::runtime_error("RX2 mesh layout is truncated");
    }
    const std::uint16_t descriptor_count =
        ReadBe16(bytes, offset + 8);
    if (descriptor_count == 0 || descriptor_count > 32 ||
        !RangeValid(
            bytes.size(), offset + 16,
            std::size_t(descriptor_count) * 16 + 1)) {
      throw std::runtime_error(
          "RX2 vertex declaration is invalid");
    }
    layout = {};
    layout.stride =
        bytes[offset + 16 + std::size_t(descriptor_count) * 16];
    if (layout.stride == 0 || layout.stride > 256) {
      throw std::runtime_error("RX2 vertex stride is invalid");
    }
    bool has_position = false;
    for (std::uint16_t index = 0; index < descriptor_count; ++index) {
      const std::size_t descriptor =
          offset + 16 + std::size_t(index) * 16;
      const std::uint8_t usage = bytes[descriptor + 9];
      const std::uint8_t usage_index = bytes[descriptor + 10];
      if (usage == 0 && !has_position) {
        const auto parsed =
            ClassifyAttribute(bytes, descriptor, "position");
        if (parsed) {
          layout.position = *parsed;
          has_position = true;
        }
      } else if (usage == 5 && usage_index == 0) {
        layout.uv0 = ClassifyAttribute(bytes, descriptor, "uv");
      } else if (usage == 5 && usage_index == 1) {
        layout.uv1 = ClassifyAttribute(bytes, descriptor, "uv");
      } else if (usage == 6) {
        layout.tangent =
            ClassifyAttribute(bytes, descriptor, "tangent");
      } else if (usage == 7) {
        layout.binormal =
            ClassifyAttribute(bytes, descriptor, "binormal");
      }
    }
    if (!has_position) {
      throw std::runtime_error(
          "RX2 vertex declaration has no supported position");
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

void ComputeNormals(DecodedMesh& mesh) {
  std::vector<Vec3> normals(mesh.vertices.size());
  for (std::size_t triangle = 0;
       triangle + 2 < mesh.indices.size(); triangle += 3) {
    const std::uint32_t ia = mesh.indices[triangle];
    const std::uint32_t ib = mesh.indices[triangle + 1];
    const std::uint32_t ic = mesh.indices[triangle + 2];
    const Vec3 normal = skate::world::Normalize(
        skate::world::Cross(
            mesh.vertices[ib].position -
                mesh.vertices[ia].position,
            mesh.vertices[ic].position -
                mesh.vertices[ia].position));
    normals[ia] = normals[ia] + normal;
    normals[ib] = normals[ib] + normal;
    normals[ic] = normals[ic] + normal;
  }
  for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
    if (skate::world::LengthSquared(normals[index]) > 1.0e-12f) {
      mesh.vertices[index].normal =
          skate::world::Normalize(normals[index]);
    } else {
      mesh.vertices[index].normal = {0.0f, 1.0f, 0.0f};
    }
  }
}

bool DecodeModel(
    const std::vector<std::uint8_t>& bytes,
    std::vector<DecodedMesh>& meshes,
    std::string& error) {
  meshes.clear();
  std::vector<TocRecord> records;
  std::uint32_t data_base = 0;
  if (!ParseToc(bytes, records, data_base, error)) {
    return false;
  }
  try {
    if (ReadBe32(bytes, 0x58) != 4u) {
      error = "RX2 catalogue record is not a render model";
      return false;
    }
    std::vector<BufferResource> vertex_resources;
    std::vector<BufferResource> index_resources;
    std::vector<std::uint32_t> mesh_info;
    for (std::size_t index = 0; index < records.size(); ++index) {
      const TocRecord& record = records[index];
      if (record.type == kTypeMeshInfo &&
          record.values[0] >= 0) {
        mesh_info.push_back(
            static_cast<std::uint32_t>(record.values[0]));
        continue;
      }
      if (record.type != kTypeVertexBuffer &&
          record.type != kTypeIndexBuffer) {
        continue;
      }
      if (record.table_offset < kTocRecordBytes ||
          !RangeValid(
              bytes.size(), record.table_offset - kTocRecordBytes,
              48)) {
        continue;
      }
      const std::size_t metadata =
          record.table_offset - kTocRecordBytes;
      const std::int32_t raw_offset =
          ReadBeS32(bytes, metadata);
      const std::int32_t raw_size =
          ReadBeS32(bytes, metadata + 8);
      const std::int32_t info_offset =
          ReadBeS32(bytes, metadata + 24);
      if (raw_offset < 0 || raw_size <= 0 || info_offset < 0) {
        continue;
      }
      const std::uint32_t paired_type =
          index + 2 < records.size()
              ? records[index + 2].type
              : 0u;
      const BufferResource resource{
          static_cast<std::uint32_t>(raw_offset),
          static_cast<std::uint32_t>(raw_size),
          static_cast<std::uint32_t>(info_offset)};
      if (record.type == kTypeVertexBuffer &&
          paired_type == kTypeIndexBuffer) {
        vertex_resources.push_back(resource);
      } else if (
          record.type == kTypeIndexBuffer &&
          paired_type != kTypeIndexBuffer) {
        index_resources.push_back(resource);
      }
    }
    const std::size_t mesh_count = std::min(
        {vertex_resources.size(), index_resources.size(),
         mesh_info.size()});
    if (mesh_count == 0 ||
        vertex_resources.size() != mesh_count ||
        index_resources.size() != mesh_count ||
        mesh_info.size() != mesh_count) {
      error =
          "RX2 model does not contain complete mesh buffer sets";
      return false;
    }
    meshes.reserve(mesh_count);
    for (std::size_t mesh_index = 0;
         mesh_index < mesh_count; ++mesh_index) {
      const BufferResource& vertex_resource =
          vertex_resources[mesh_index];
      const BufferResource& index_resource =
          index_resources[mesh_index];
      MeshLayout layout;
      if (!ParseMeshLayout(
              bytes, mesh_info[mesh_index], layout, error)) {
        return false;
      }
      const std::size_t vertex_offset =
          std::size_t(data_base) + vertex_resource.offset;
      const std::size_t index_offset =
          std::size_t(data_base) + index_resource.offset;
      if (!RangeValid(
              bytes.size(), vertex_offset,
              vertex_resource.size) ||
          !RangeValid(
              bytes.size(), index_offset,
              index_resource.size) ||
          vertex_resource.size % layout.stride != 0 ||
          !RangeValid(
              bytes.size(),
              std::size_t(index_resource.info_offset) + 32, 4)) {
        error = "RX2 model buffer range is invalid";
        return false;
      }
      const std::uint32_t vertex_count =
          vertex_resource.size / layout.stride;
      std::uint32_t index_count =
          ReadBe32(
              bytes,
              std::size_t(index_resource.info_offset) + 32);
      index_count =
          std::min<std::uint32_t>(
              index_count, index_resource.size / 2);
      index_count -= index_count % 3u;
      if (vertex_count == 0 || index_count == 0) {
        error = "RX2 model mesh is empty";
        return false;
      }
      DecodedMesh mesh;
      mesh.vertices.resize(vertex_count);
      bool authored_normals =
          layout.tangent.has_value() &&
          layout.binormal.has_value();
      for (std::uint32_t vertex_index = 0;
           vertex_index < vertex_count; ++vertex_index) {
        const std::size_t source =
            vertex_offset +
            std::size_t(vertex_index) * layout.stride;
        RenderVertex& vertex = mesh.vertices[vertex_index];
        vertex.position = {
            ReadAttributeComponent(
                bytes, source, layout.position, 0),
            ReadAttributeComponent(
                bytes, source, layout.position, 1),
            ReadAttributeComponent(
                bytes, source, layout.position, 2)};
        if (layout.uv0) {
          vertex.uv = {
              ReadAttributeComponent(
                  bytes, source, *layout.uv0, 0),
              ReadAttributeComponent(
                  bytes, source, *layout.uv0, 1)};
        }
        vertex.lightmap_uv = vertex.uv;
        vertex.decal_uv = vertex.uv;
        if (layout.uv1) {
          vertex.lightmap_uv = {
              ReadAttributeComponent(
                  bytes, source, *layout.uv1, 0),
              ReadAttributeComponent(
                  bytes, source, *layout.uv1, 1)};
          vertex.decal_uv = vertex.lightmap_uv;
        }
        if (authored_normals) {
          const auto tangent = DecodePackedVector(
              ReadBe32(
                  bytes, source + layout.tangent->offset));
          const auto binormal = DecodePackedVector(
              ReadBe32(
                  bytes, source + layout.binormal->offset));
          const Vec3 tangent_vector{
              tangent[0], tangent[1], tangent[2]};
          const Vec3 binormal_vector{
              binormal[0], binormal[1], binormal[2]};
          const Vec3 normal = skate::world::Cross(
              tangent_vector, binormal_vector);
          if (skate::world::LengthSquared(normal) > 1.0e-10f) {
            vertex.normal = skate::world::Normalize(normal);
            vertex.tangent_binormal =
                skate::world::Normalize(binormal_vector);
            vertex.tangent_handedness = 1.0f;
          } else {
            authored_normals = false;
          }
        }
      }
      mesh.indices.reserve(index_count);
      for (std::uint32_t index = 0; index < index_count; ++index) {
        const std::uint32_t value =
            ReadBe16(bytes, index_offset + std::size_t(index) * 2);
        if (value >= vertex_count) {
          error = "RX2 model index is out of range";
          return false;
        }
        mesh.indices.push_back(value);
      }
      if (!authored_normals) {
        ComputeNormals(mesh);
      }
      meshes.push_back(std::move(mesh));
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

struct RetailCollisionTriangle {
  Vec3 a;
  Vec3 b;
  Vec3 c;
  std::uint16_t surface = 0;
  std::array<std::uint8_t, 3> edge_codes{};
  bool has_edge_codes = false;
};

std::uint32_t ReadLittleId(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& cursor, std::size_t end,
    std::uint8_t width) {
  if ((width != 1 && width != 2) ||
      cursor > end || width > end - cursor) {
    throw std::runtime_error(
        "RX2 collision unit ID is invalid");
  }
  std::uint32_t value = bytes[cursor++];
  if (width == 2) {
    value |= std::uint32_t(bytes[cursor++]) << 8u;
  }
  return value;
}

bool DecodeCollisionMesh(
    const std::vector<std::uint8_t>& bytes,
    std::size_t mesh_offset, std::size_t mesh_size,
    std::vector<RetailCollisionTriangle>& triangles,
    std::string& error) {
  try {
    if (mesh_size < 96 ||
        !RangeValid(bytes.size(), mesh_offset, mesh_size)) {
      throw std::runtime_error(
          "RX2 collision mesh is truncated");
    }
    const std::uint32_t expected_triangles =
        ReadBe32(bytes, mesh_offset + 40);
    const std::uint32_t cluster_table =
        ReadBe32(bytes, mesh_offset + 52);
    const float granularity =
        ReadBeFloat(bytes, mesh_offset + 56);
    const std::uint8_t group_width =
        bytes[mesh_offset + 62];
    const std::uint8_t surface_width =
        bytes[mesh_offset + 63];
    const std::uint32_t cluster_count =
        ReadBe32(bytes, mesh_offset + 64);
    const std::uint32_t declared_size =
        ReadBe32(bytes, mesh_offset + 80);
    if (!std::isfinite(granularity) || granularity <= 0.0f ||
        declared_size > mesh_size || declared_size < 96 ||
        cluster_count == 0 || cluster_count > 65536 ||
        cluster_table > declared_size ||
        std::uint64_t(cluster_table) +
                std::uint64_t(cluster_count) * 4u >
            declared_size) {
      throw std::runtime_error(
          "RX2 collision mesh header is invalid");
    }
    std::vector<std::uint32_t> cluster_offsets;
    cluster_offsets.reserve(cluster_count);
    for (std::uint32_t index = 0; index < cluster_count; ++index) {
      const std::uint32_t offset = ReadBe32(
          bytes,
          mesh_offset + cluster_table + std::size_t(index) * 4);
      if ((index != 0 && offset <= cluster_offsets.back()) ||
          offset >= declared_size) {
        throw std::runtime_error(
            "RX2 collision cluster table is invalid");
      }
      cluster_offsets.push_back(offset);
    }
    const std::size_t first_triangle = triangles.size();
    for (std::size_t cluster_index = 0;
         cluster_index < cluster_offsets.size(); ++cluster_index) {
      const std::size_t cluster =
          mesh_offset + cluster_offsets[cluster_index];
      const std::size_t span_end =
          mesh_offset +
          (cluster_index + 1 < cluster_offsets.size()
               ? cluster_offsets[cluster_index + 1]
               : declared_size);
      if (!RangeValid(bytes.size(), cluster, 16) ||
          span_end > mesh_offset + declared_size) {
        throw std::runtime_error(
            "RX2 collision cluster is truncated");
      }
      const std::uint16_t unit_count =
          ReadBe16(bytes, cluster);
      const std::uint16_t unit_bytes =
          ReadBe16(bytes, cluster + 2);
      const std::uint16_t vertex_blocks =
          ReadBe16(bytes, cluster + 4);
      const std::uint16_t cluster_bytes =
          ReadBe16(bytes, cluster + 8);
      const std::uint8_t vertex_count =
          bytes[cluster + 10];
      const std::uint8_t compression =
          bytes[cluster + 12];
      const std::size_t unit_offset =
          std::size_t(vertex_blocks + 1u) * 16u;
      const std::size_t unit_begin = cluster + unit_offset;
      const std::size_t unit_end = unit_begin + unit_bytes;
      if (cluster + cluster_bytes > span_end ||
          unit_offset < 16 || unit_end > cluster + cluster_bytes) {
        throw std::runtime_error(
            "RX2 collision cluster spans are invalid");
      }
      std::vector<Vec3> vertices;
      vertices.reserve(vertex_count);
      if (compression == 1) {
        const std::array<std::int32_t, 3> base{
            ReadBeS32(bytes, cluster + 16),
            ReadBeS32(bytes, cluster + 20),
            ReadBeS32(bytes, cluster + 24)};
        for (std::uint8_t index = 0; index < vertex_count; ++index) {
          const std::size_t source =
              cluster + 28 + std::size_t(index) * 6;
          if (source + 6 > unit_begin) {
            throw std::runtime_error(
                "RX2 compressed collision vertices overlap units");
          }
          vertices.push_back({
              float(base[0] + ReadBeS16(bytes, source)) *
                  granularity,
              float(base[1] + ReadBeS16(bytes, source + 2)) *
                  granularity,
              float(base[2] + ReadBeS16(bytes, source + 4)) *
                  granularity});
        }
      } else if (compression == 2) {
        for (std::uint8_t index = 0; index < vertex_count; ++index) {
          const std::size_t source =
              cluster + 16 + std::size_t(index) * 12;
          if (source + 12 > unit_begin) {
            throw std::runtime_error(
                "RX2 compressed collision vertices overlap units");
          }
          vertices.push_back({
              float(ReadBeS32(bytes, source)) * granularity,
              float(ReadBeS32(bytes, source + 4)) * granularity,
              float(ReadBeS32(bytes, source + 8)) * granularity});
        }
      } else if (compression == 0) {
        for (std::uint8_t index = 0; index < vertex_count; ++index) {
          const std::size_t source =
              cluster + 16 + std::size_t(index) * 16;
          if (source + 16 > unit_begin) {
            throw std::runtime_error(
                "RX2 collision vertices overlap units");
          }
          vertices.push_back(ReadVec3(bytes, source));
        }
      } else {
        throw std::runtime_error(
            "unsupported RX2 collision vertex compression");
      }

      std::size_t cursor = unit_begin;
      for (std::uint16_t unit = 0; unit < unit_count; ++unit) {
        if (cursor >= unit_end) {
          throw std::runtime_error(
              "RX2 collision unit stream is truncated");
        }
        const std::uint8_t flags = bytes[cursor++];
        const std::uint8_t type = flags & 0x0Fu;
        std::size_t triangle_count = 0;
        if (type == 1) {
          triangle_count = 1;
        } else if (type == 2) {
          triangle_count = 2;
        } else {
          throw std::runtime_error(
              "unsupported RX2 collision unit type");
        }
        const std::size_t index_count = triangle_count + 2;
        if (cursor > unit_end || index_count > unit_end - cursor) {
          throw std::runtime_error(
              "RX2 collision unit indices are truncated");
        }
        std::vector<std::uint8_t> indices(
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(cursor + index_count));
        cursor += index_count;
        if (std::any_of(
                indices.begin(), indices.end(),
                [vertex_count](std::uint8_t value) {
                  return value >= vertex_count;
                })) {
          throw std::runtime_error(
              "RX2 collision unit index is out of range");
        }
        std::vector<std::uint8_t> edge_codes;
        if ((flags & 0x20u) != 0) {
          if (cursor > unit_end ||
              index_count > unit_end - cursor) {
            throw std::runtime_error(
                "RX2 collision edge codes are truncated");
          }
          edge_codes.assign(
              bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
              bytes.begin() +
                  static_cast<std::ptrdiff_t>(cursor + index_count));
          cursor += index_count;
        }
        if ((flags & 0x40u) != 0) {
          (void)ReadLittleId(
              bytes, cursor, unit_end, group_width);
        }
        std::uint16_t surface = 0;
        if ((flags & 0x80u) != 0) {
          surface = static_cast<std::uint16_t>(
              ReadLittleId(
                  bytes, cursor, unit_end, surface_width));
        }
        const auto append_triangle =
            [&](std::uint8_t a, std::uint8_t b,
                std::uint8_t c,
                std::array<std::uint8_t, 3> codes) {
              RetailCollisionTriangle triangle;
              triangle.a = vertices[a];
              triangle.b = vertices[b];
              triangle.c = vertices[c];
              triangle.surface = surface;
              triangle.edge_codes = codes;
              triangle.has_edge_codes = !edge_codes.empty();
              triangles.push_back(triangle);
            };
        if (type == 1) {
          append_triangle(
              indices[0], indices[1], indices[2],
              edge_codes.empty()
                  ? std::array<std::uint8_t, 3>{}
                  : std::array<std::uint8_t, 3>{
                        edge_codes[0], edge_codes[1],
                        edge_codes[2]});
        } else {
          append_triangle(
              indices[0], indices[1], indices[2],
              edge_codes.empty()
                  ? std::array<std::uint8_t, 3>{}
                  : std::array<std::uint8_t, 3>{
                        edge_codes[0], edge_codes[1], 0x1Au});
          append_triangle(
              indices[3], indices[2], indices[1],
              edge_codes.empty()
                  ? std::array<std::uint8_t, 3>{}
                  : std::array<std::uint8_t, 3>{
                        edge_codes[3], edge_codes[2], 0x1Au});
        }
      }
      if (cursor != unit_end) {
        throw std::runtime_error(
            "RX2 collision unit stream has trailing bytes");
      }
    }
    if (triangles.size() - first_triangle != expected_triangles) {
      throw std::runtime_error(
          "RX2 collision triangle count does not match its header");
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool DecodeCollision(
    const std::vector<std::uint8_t>& bytes,
    std::vector<RetailCollisionTriangle>& triangles,
    std::string& error) {
  triangles.clear();
  std::vector<TocRecord> records;
  std::uint32_t data_base = 0;
  if (!ParseToc(bytes, records, data_base, error)) {
    return false;
  }
  (void)data_base;
  for (const TocRecord& record : records) {
    if (record.type != kTypeCollision) {
      continue;
    }
    if (record.values[0] < 0 || record.values[2] <= 0 ||
        !DecodeCollisionMesh(
            bytes,
            static_cast<std::size_t>(record.values[0]),
            static_cast<std::size_t>(record.values[2]),
            triangles, error)) {
      if (error.empty()) {
        error = "RX2 collision section has an invalid range";
      }
      return false;
    }
  }
  if (triangles.empty()) {
    error = "RX2 model has no supported collision geometry";
    return false;
  }
  return true;
}

bool DecodeGrinds(
    const std::vector<std::uint8_t>& bytes,
    std::string_view item_name,
    std::vector<GrindRail>& rails,
    std::string& error) {
  rails.clear();
  std::vector<TocRecord> records;
  std::uint32_t data_base = 0;
  if (!ParseToc(bytes, records, data_base, error)) {
    return false;
  }
  (void)data_base;
  try {
    for (const TocRecord& record : records) {
      if (record.type != kTypeSpline) {
        continue;
      }
      if (record.values[0] < 0 || record.values[2] <= 0) {
        throw std::runtime_error(
            "RX2 grind section has an invalid range");
      }
      const std::size_t section =
          static_cast<std::size_t>(record.values[0]);
      const std::size_t size =
          static_cast<std::size_t>(record.values[2]);
      if (!RangeValid(bytes.size(), section, size) || size < 16) {
        throw std::runtime_error("RX2 grind section is truncated");
      }
      const std::uint32_t rail_count =
          ReadBe32(bytes, section);
      const std::uint32_t segment_count =
          ReadBe32(bytes, section + 4);
      const std::uint32_t rail_table =
          ReadBe32(bytes, section + 8);
      const std::uint32_t segment_table =
          ReadBe32(bytes, section + 12);
      if (rail_table != 16u ||
          segment_table != 16u + rail_count * 32u ||
          std::uint64_t(segment_table) +
                  std::uint64_t(segment_count) * 144u !=
              size) {
        throw std::runtime_error(
            "RX2 grind section has inconsistent tables");
      }
      std::unordered_set<std::uint32_t> claimed;
      for (std::uint32_t rail_index = 0;
           rail_index < rail_count; ++rail_index) {
        const std::size_t source =
            section + rail_table +
            std::size_t(rail_index) * 32;
        const std::uint32_t first =
            ReadBe32(bytes, source + 20);
        const std::uint32_t last =
            ReadBe32(bytes, source + 24);
        if (first < segment_table || last < first ||
            (first - segment_table) % 144u != 0 ||
            (last - first) % 144u != 0 ||
            std::uint64_t(last) + 144u > size) {
          throw std::runtime_error(
              "RX2 grind rail has invalid segment links");
        }
        GrindRail rail;
        rail.id = static_cast<std::uint32_t>(rails.size() + 1);
        rail.name =
            std::string(item_name) + " rail " +
            std::to_string(rail.id);
        rail.retail_spline_id = ReadBe64(bytes, source);
        rail.retail_type_signature =
            ReadBe64(bytes, source + 8);
        rail.retail_flags = ReadBe32(bytes, source + 16);
        rail.retail_trailing_word =
            ReadBe32(bytes, source + 28);
        std::optional<Vec3> first_start;
        Vec3 previous_end;
        for (std::uint32_t relative = first;
             relative <= last; relative += 144u) {
          const std::uint32_t segment_index =
              (relative - segment_table) / 144u;
          if (!claimed.insert(segment_index).second) {
            throw std::runtime_error(
                "RX2 grind rails share a segment");
          }
          const std::size_t segment = section + relative;
          NativeGrindSegment native;
          for (std::size_t word = 0;
               word < native.words.size(); ++word) {
            native.words[word] =
                ReadBe32(bytes, segment + word * 4);
            if (!std::isfinite(
                    std::bit_cast<float>(native.words[word]))) {
              throw std::runtime_error(
                  "RX2 grind segment contains non-finite data");
            }
          }
          const Vec3 a = ReadVec3(bytes, segment);
          const Vec3 b = ReadVec3(bytes, segment + 16);
          const Vec3 c = ReadVec3(bytes, segment + 32);
          const Vec3 d = ReadVec3(bytes, segment + 48);
          if (!first_start) {
            first_start = d;
          }
          previous_end = d + c + b + a;
          rail.native_segments.push_back(native);
        }
        rail.closed =
            first_start.has_value() &&
            skate::world::LengthSquared(
                *first_start - previous_end) <= 1.0e-6f;
        rails.push_back(std::move(rail));
      }
      if (claimed.size() != segment_count) {
        throw std::runtime_error(
            "RX2 grind section has unreferenced segments");
      }
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

std::uint32_t X360TiledX(
    std::uint32_t offset, std::uint32_t width_units,
    std::uint32_t texel_pitch) {
  const std::uint32_t aligned_width =
      (width_units + 31u) & ~31u;
  const std::uint32_t log_bpp =
      (texel_pitch >> 2u) +
      ((texel_pitch >> 1u) >> (texel_pitch >> 2u));
  const std::uint32_t offset_b = offset << log_bpp;
  const std::uint32_t offset_t =
      ((offset_b & ~4095u) >> 3u) +
      ((offset_b & 1792u) >> 2u) +
      (offset_b & 63u);
  const std::uint32_t offset_m =
      offset_t >> (7u + log_bpp);
  const std::uint32_t macro_x =
      (offset_m % (aligned_width >> 5u)) << 2u;
  const std::uint32_t tile =
      (((offset_t >> (5u + log_bpp)) & 2u) +
       (offset_b >> 6u)) &
      3u;
  const std::uint32_t macro = (macro_x + tile) << 3u;
  const std::uint32_t micro =
      ((((offset_t >> 1u) & ~15u) + (offset_t & 15u)) &
       ((texel_pitch << 3u) - 1u)) >>
      log_bpp;
  return macro + micro;
}

std::uint32_t X360TiledY(
    std::uint32_t offset, std::uint32_t width_units,
    std::uint32_t texel_pitch) {
  const std::uint32_t aligned_width =
      (width_units + 31u) & ~31u;
  const std::uint32_t log_bpp =
      (texel_pitch >> 2u) +
      ((texel_pitch >> 1u) >> (texel_pitch >> 2u));
  const std::uint32_t offset_b = offset << log_bpp;
  const std::uint32_t offset_t =
      ((offset_b & ~4095u) >> 3u) +
      ((offset_b & 1792u) >> 2u) +
      (offset_b & 63u);
  const std::uint32_t offset_m =
      offset_t >> (7u + log_bpp);
  const std::uint32_t macro_y =
      (offset_m / (aligned_width >> 5u)) << 2u;
  const std::uint32_t tile =
      ((offset_t >> (6u + log_bpp)) & 1u) +
      ((offset_b & 2048u) >> 10u);
  const std::uint32_t macro = (macro_y + tile) << 3u;
  const std::uint32_t micro =
      ((((offset_t &
          (((texel_pitch << 6u) - 1u) & ~31u)) +
         ((offset_t & 15u) << 1u)) >>
        (3u + log_bpp)) &
       ~1u);
  return macro + micro + ((offset_t & 16u) >> 4u);
}

std::vector<std::uint8_t> Untile(
    const std::vector<std::uint8_t>& source,
    std::uint32_t width_units, std::uint32_t pitch) {
  if (width_units == 0 || pitch == 0) {
    throw std::runtime_error("RX2 texture tiling dimensions are invalid");
  }
  std::vector<std::uint8_t> linear(source.size());
  const std::size_t units = source.size() / pitch;
  const std::size_t rows = units / width_units;
  for (std::size_t y = 0; y < rows; ++y) {
    for (std::uint32_t x = 0; x < width_units; ++x) {
      const std::uint32_t logical =
          static_cast<std::uint32_t>(y) * width_units + x;
      const std::uint32_t tiled_x =
          X360TiledX(logical, width_units, pitch);
      const std::uint32_t tiled_y =
          X360TiledY(logical, width_units, pitch);
      const std::size_t destination =
          (std::size_t(tiled_y) * width_units + tiled_x) * pitch;
      const std::size_t input = std::size_t(logical) * pitch;
      if (destination + pitch <= linear.size() &&
          input + pitch <= source.size()) {
        std::copy_n(
            source.begin() + static_cast<std::ptrdiff_t>(input),
            pitch,
            linear.begin() +
                static_cast<std::ptrdiff_t>(destination));
      }
    }
  }
  return linear;
}

std::array<std::uint8_t, 3> Rgb565(std::uint16_t value) {
  return {
      static_cast<std::uint8_t>(
          ((value >> 11u) & 31u) * 255u / 31u),
      static_cast<std::uint8_t>(
          ((value >> 5u) & 63u) * 255u / 63u),
      static_cast<std::uint8_t>(
          (value & 31u) * 255u / 31u)};
}

std::array<std::array<std::uint8_t, 4>, 16> DecodeBc1Block(
    const std::uint8_t* block, bool transparent) {
  const std::uint16_t c0 =
      (std::uint16_t(block[0]) << 8u) | block[1];
  const std::uint16_t c1 =
      (std::uint16_t(block[2]) << 8u) | block[3];
  const auto first = Rgb565(c0);
  const auto second = Rgb565(c1);
  std::array<std::array<std::uint8_t, 4>, 4> palette{};
  palette[0] = {first[0], first[1], first[2], 255};
  palette[1] = {second[0], second[1], second[2], 255};
  if (c0 > c1) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
      palette[2][channel] = static_cast<std::uint8_t>(
          (2u * first[channel] + second[channel] + 1u) / 3u);
      palette[3][channel] = static_cast<std::uint8_t>(
          (first[channel] + 2u * second[channel] + 1u) / 3u);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (std::size_t channel = 0; channel < 3; ++channel) {
      palette[2][channel] = static_cast<std::uint8_t>(
          (std::uint16_t(first[channel]) + second[channel]) / 2u);
    }
    palette[2][3] = 255;
    palette[3] = {0, 0, 0, transparent ? std::uint8_t{0}
                                       : std::uint8_t{255}};
  }
  std::array<std::array<std::uint8_t, 4>, 16> pixels{};
  for (std::size_t y = 0; y < 4; ++y) {
    const std::uint8_t packed = block[4 + (y ^ 1u)];
    for (std::size_t x = 0; x < 4; ++x) {
      pixels[y * 4 + x] =
          palette[(packed >> (x * 2u)) & 3u];
    }
  }
  return pixels;
}

std::array<std::uint8_t, 8> AlphaPalette(
    std::uint8_t first, std::uint8_t second) {
  std::array<std::uint8_t, 8> values{first, second};
  if (first > second) {
    for (std::size_t index = 0; index < 6; ++index) {
      values[index + 2] = static_cast<std::uint8_t>(
          ((6u - index) * first + (index + 1u) * second) /
          7u);
    }
  } else {
    values[2] = static_cast<std::uint8_t>(
        (4u * first + second) / 5u);
    values[3] = static_cast<std::uint8_t>(
        (3u * first + 2u * second) / 5u);
    values[4] = static_cast<std::uint8_t>(
        (2u * first + 3u * second) / 5u);
    values[5] = static_cast<std::uint8_t>(
        (first + 4u * second) / 5u);
    values[6] = 0;
    values[7] = 255;
  }
  return values;
}

std::array<std::array<std::uint8_t, 4>, 16> DecodeBc3Block(
    const std::uint8_t* block) {
  auto pixels = DecodeBc1Block(block + 8, false);
  const auto alpha = AlphaPalette(block[1], block[0]);
  const std::array<std::uint8_t, 6> swapped{
      block[3], block[2], block[5],
      block[4], block[7], block[6]};
  std::uint64_t bits = 0;
  for (std::size_t byte = 0; byte < swapped.size(); ++byte) {
    bits |= std::uint64_t(swapped[byte]) << (byte * 8u);
  }
  for (std::size_t pixel = 0; pixel < 16; ++pixel) {
    pixels[pixel][3] =
        alpha[(bits >> (pixel * 3u)) & 7u];
  }
  return pixels;
}

template <typename Decoder>
std::vector<std::uint8_t> DecodeBlocks(
    const std::vector<std::uint8_t>& blocks,
    std::uint32_t width, std::uint32_t height,
    std::uint32_t block_bytes, Decoder decoder) {
  std::vector<std::uint8_t> rgba(
      std::size_t(width) * height * 4u);
  const std::uint32_t blocks_x = (width + 3u) / 4u;
  const std::uint32_t blocks_y = (height + 3u) / 4u;
  if (std::uint64_t(blocks_x) * blocks_y * block_bytes >
      blocks.size()) {
    throw std::runtime_error("RX2 block texture payload is truncated");
  }
  for (std::uint32_t by = 0; by < blocks_y; ++by) {
    for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
      const std::size_t source =
          (std::size_t(by) * blocks_x + bx) * block_bytes;
      const auto pixels = decoder(blocks.data() + source);
      for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
          const std::uint32_t output_x = bx * 4u + x;
          const std::uint32_t output_y = by * 4u + y;
          if (output_x >= width || output_y >= height) {
            continue;
          }
          const std::size_t destination =
              (std::size_t(output_y) * width + output_x) * 4u;
          std::copy(
              pixels[y * 4u + x].begin(),
              pixels[y * 4u + x].end(),
              rgba.begin() +
                  static_cast<std::ptrdiff_t>(destination));
        }
      }
    }
  }
  return rgba;
}

std::uint32_t AlignUp(
    std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

std::uint32_t Log2Ceil(std::uint32_t value) {
  std::uint32_t result = 0;
  --value;
  while (value != 0) {
    value >>= 1u;
    ++result;
  }
  return result;
}

std::pair<std::uint32_t, std::uint32_t> PackedBaseOffset(
    std::uint32_t width, std::uint32_t height) {
  const std::uint32_t log_width = Log2Ceil(width);
  const std::uint32_t log_height = Log2Ceil(height);
  if (std::min(log_width, log_height) > 4u) {
    return {0, 0};
  }
  return log_width > log_height
             ? std::pair<std::uint32_t, std::uint32_t>{0, 16}
             : std::pair<std::uint32_t, std::uint32_t>{16, 0};
}

std::uint32_t PackedTiledOffset(
    std::uint32_t x, std::uint32_t y,
    std::uint32_t pitch, std::uint32_t log_bpp) {
  pitch = AlignUp(pitch, 32);
  const std::uint32_t macro =
      ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) <<
      (log_bpp + 7u);
  const std::uint32_t micro =
      ((x & 7u) + ((y & 0xEu) << 2u)) << log_bpp;
  const std::uint32_t offset =
      macro + ((micro & ~0xFu) << 1u) +
      (micro & 0xFu) + ((y & 1u) << 4u);
  return ((offset & ~0x1FFu) << 3u) +
         ((y & 16u) << 7u) +
         ((offset & 0x1C0u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) +
         (offset & 0x3Fu);
}

bool DecodeTexture(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t texture_id,
    TextureColorSpace color_space,
    ImageTexture& texture, std::string& error) {
  std::vector<TocRecord> records;
  std::uint32_t data_base = 0;
  if (!ParseToc(bytes, records, data_base, error)) {
    return false;
  }
  try {
    for (std::size_t index = 1; index < records.size(); ++index) {
      const TocRecord& record = records[index];
      if (record.type != kTypeTextureInfo ||
          record.values[0] < 0 ||
          records[index - 1].values[0] < 0 ||
          records[index - 1].values[2] <= 0) {
        continue;
      }
      const std::size_t info =
          static_cast<std::size_t>(record.values[0]);
      if (!RangeValid(bytes.size(), info, 40)) {
        throw std::runtime_error(
            "RX2 texture header is truncated");
      }
      const std::uint8_t format = bytes[info + 35];
      const std::uint32_t dimensions =
          ReadBe32(bytes, info + 36);
      const std::uint32_t width =
          (dimensions & 0x1FFFu) + 1u;
      const std::uint32_t height =
          ((dimensions >> 13u) & 0x1FFFu) + 1u;
      if (width == 0 || height == 0 ||
          width > 4096 || height > 4096 ||
          std::uint64_t(width) * height * 4u >
              256u * 1024u * 1024u) {
        throw std::runtime_error(
            "RX2 texture dimensions are invalid");
      }
      const std::size_t payload =
          std::size_t(data_base) +
          static_cast<std::uint32_t>(
              records[index - 1].values[0]);
      const std::size_t payload_size =
          static_cast<std::uint32_t>(
              records[index - 1].values[2]);
      if (!RangeValid(bytes.size(), payload, payload_size)) {
        throw std::runtime_error(
            "RX2 texture payload is truncated");
      }
      std::vector<std::uint8_t> source(
          bytes.begin() + static_cast<std::ptrdiff_t>(payload),
          bytes.begin() +
              static_cast<std::ptrdiff_t>(payload + payload_size));
      std::vector<std::uint8_t> rgba;
      if (format == 0x52u || format == 0x54u) {
        const std::uint32_t block_bytes =
            format == 0x52u ? 8u : 16u;
        const std::uint32_t blocks_x = (width + 3u) / 4u;
        source = Untile(source, blocks_x, block_bytes);
        if (format == 0x52u) {
          rgba = DecodeBlocks(
              source, width, height, block_bytes,
              [](const std::uint8_t* block) {
                return DecodeBc1Block(block, true);
              });
        } else {
          rgba = DecodeBlocks(
              source, width, height, block_bytes,
              [](const std::uint8_t* block) {
                return DecodeBc3Block(block);
              });
        }
      } else if (format == 0x86u) {
        source = Untile(source, width, 4);
        rgba.resize(std::size_t(width) * height * 4u);
        if (source.size() < rgba.size()) {
          throw std::runtime_error(
              "RX2 A8R8G8B8 payload is truncated");
        }
        for (std::size_t pixel = 0;
             pixel < std::size_t(width) * height; ++pixel) {
          rgba[pixel * 4] = source[pixel * 4 + 1];
          rgba[pixel * 4 + 1] = source[pixel * 4 + 2];
          rgba[pixel * 4 + 2] = source[pixel * 4 + 3];
          rgba[pixel * 4 + 3] = source[pixel * 4];
        }
      } else if (format == 0x44u) {
        const auto [packed_x, packed_y] =
            PackedBaseOffset(width, height);
        rgba.resize(std::size_t(width) * height * 4u);
        for (std::uint32_t y = 0; y < height; ++y) {
          for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t source_offset =
                PackedTiledOffset(
                    x + packed_x, y + packed_y, width, 1);
            if (!RangeValid(source.size(), source_offset, 2)) {
              throw std::runtime_error(
                  "RX2 B5G6R5 payload is truncated");
            }
            const std::uint16_t value =
                (std::uint16_t(source[source_offset]) << 8u) |
                source[source_offset + 1];
            const std::size_t destination =
                (std::size_t(y) * width + x) * 4u;
            rgba[destination] = static_cast<std::uint8_t>(
                ((value >> 11u) & 31u) * 255u / 31u);
            rgba[destination + 1] = static_cast<std::uint8_t>(
                ((value >> 5u) & 63u) * 255u / 63u);
            rgba[destination + 2] = static_cast<std::uint8_t>(
                (value & 31u) * 255u / 31u);
            rgba[destination + 3] = 255;
          }
        }
      } else {
        throw std::runtime_error(
            "unsupported RX2 texture format " +
            std::to_string(format));
      }
      texture = {};
      texture.name = HexId(texture_id);
      texture.width = width;
      texture.height = height;
      texture.color_space = color_space;
      texture.rgba8 = std::move(rgba);
      return true;
    }
    error = "RX2 texture has no texture-info section";
    return false;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

std::string SanitizedFilename(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' ||
        character == '_' || character == '.') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('_');
    }
  }
  while (!result.empty() &&
         (result.back() == '.' || result.back() == ' ')) {
    result.pop_back();
  }
  if (result.empty()) {
    result = "item";
  }
  return result;
}

RetailShaderFamily ClassifyShaderFamily(std::string_view type) {
  if (type == "dynamicObject" ||
      type == "environmentPark" ||
      type == "environmentParkDiffuse") {
    return RetailShaderFamily::DynamicObject;
  }
  if (type == "dynamicObjectAlphaTest" ||
      type == "environmentParkAlphaTest") {
    return RetailShaderFamily::DynamicObjectAlphaTest;
  }
  if (type == "environmentParkDecal") {
    return RetailShaderFamily::DynamicObjectDecal;
  }
  if (type == "environmentParkWater") {
    return RetailShaderFamily::FlowingWater;
  }
  if (type == "videoscreen") {
    return RetailShaderFamily::Incandescent;
  }
  return RetailShaderFamily::EnvironmentDiffuse;
}

RetailRenderFlags RenderFlags(std::string_view type) {
  if (type == "environmentParkDecal") {
    return RetailRenderFlags::Decal;
  }
  if (type == "environmentParkAlphaTest" ||
      type == "dynamicObjectAlphaTest") {
    return RetailRenderFlags::AlphaTest;
  }
  if (type == "environmentParkWater") {
    return RetailRenderFlags::Water |
           RetailRenderFlags::AlphaBlend;
  }
  return RetailRenderFlags::None;
}

TextureColorSpace ColorSpaceForChannel(std::string_view channel) {
  return channel == "diffuse" || channel == "decal" ||
                 channel == "transparency"
             ? TextureColorSpace::Srgb
             : TextureColorSpace::Linear;
}

std::string ClassifyRetailSemantic(std::string channel) {
  if (channel == "transparency") {
    return "transparent";
  }
  if (channel == "detailnormal") {
    return "detail";
  }
  return channel;
}

bool CurrentGeneratedAsset(const SkateObjectAsset& asset) {
  return !asset.objects.empty() &&
         std::all_of(
             asset.objects.begin(), asset.objects.end(),
             [](const MapObject& object) {
               return object.physics.type ==
                      ObjectPhysicsType::Disabled;
             }) &&
         std::all_of(
             asset.materials.begin(), asset.materials.end(),
             [](const SurfaceMaterial& material) {
               return !material.retail.enabled ||
                      material.retail.shader_family ==
                          ClassifyShaderFamily(
                              material.retail.shader_name);
             });
}

struct AssetIndex {
  std::unordered_map<std::uint64_t, std::string> models;
  std::unordered_map<std::uint64_t, std::string> textures;
};

void IndexPaths(
    const std::vector<std::string>& paths,
    std::unordered_map<std::uint64_t, std::string>& index) {
  for (const std::string& path : paths) {
    const std::filesystem::path parsed(path);
    if (Fold(parsed.extension().string()) != ".rx2") {
      continue;
    }
    const auto id = ParseId(parsed.stem().string());
    if (id) {
      index.emplace(*id, path);
    }
  }
}

bool ReadArchiveFile(
    const cac_archive::Archive& archive,
    std::string_view path,
    std::vector<std::uint8_t>& bytes,
    std::string& error) {
  if (!archive.Read(path, bytes, error)) {
    return false;
  }
  if (bytes.empty() || bytes.size() > kMaximumRx2Bytes) {
    error = "archive member has an unsupported size";
    return false;
  }
  return true;
}

bool BuildAsset(
    const Item& item, const Recipe& recipe,
    const AssetIndex& index,
    const cac_archive::Archive& archive,
    SkateObjectAsset& asset, std::string& error) {
  const auto model_path = index.models.find(item.model_id);
  if (model_path == index.models.end()) {
    error =
        "model " + HexId(item.model_id) +
        " is absent from parkassets.big";
    return false;
  }
  std::vector<std::uint8_t> model_bytes;
  if (!ReadArchiveFile(
          archive, model_path->second, model_bytes, error)) {
    error = "could not read model " + HexId(item.model_id) +
            ": " + error;
    return false;
  }
  std::vector<DecodedMesh> meshes;
  if (!DecodeModel(model_bytes, meshes, error)) {
    return false;
  }
  if (meshes.size() != item.material_ids.size()) {
    error =
        "model mesh/material count does not match the retail recipe";
    return false;
  }
  std::vector<RetailCollisionTriangle> retail_collision;
  if (!DecodeCollision(model_bytes, retail_collision, error)) {
    return false;
  }
  std::vector<GrindRail> rails;
  if (!DecodeGrinds(model_bytes, item.name, rails, error)) {
    return false;
  }

  asset = {};
  asset.format_version = 2;
  asset.name = item.name;
  std::unordered_map<std::uint64_t, TextureId> texture_ids;
  for (std::size_t material_index = 0;
       material_index < item.material_ids.size();
       ++material_index) {
    const std::uint64_t source_id =
        item.material_ids[material_index];
    const auto source = recipe.materials.find(source_id);
    if (source == recipe.materials.end()) {
      error =
          "retail material " + HexId(source_id) +
          " is absent from the recipe";
      return false;
    }
    SurfaceMaterial material;
    material.id =
        static_cast<MaterialId>(asset.materials.size() + 1);
    material.name =
        item.name + " material " +
        std::to_string(material_index + 1);
    material.flags = SurfaceFlags::Skateable;
    // Retail textures already contain the authored colour. A neutral tint
    // also keeps the generic fallback from imposing the old grey/brown cast
    // while exact lighting rows are still being captured.
    material.display_color = {1.0f, 1.0f, 1.0f};
    material.roughness = 0.68f;
    material.retail.enabled = true;
    material.retail.material_guid = source_id;
    material.retail.shader_name = source->second.type;
    material.retail.shader_family =
        ClassifyShaderFamily(source->second.type);
    material.retail.render_flags =
        RenderFlags(source->second.type);
    if (skate::world::HasFlag(
            material.retail.render_flags,
            RetailRenderFlags::AlphaTest)) {
      material.alpha_mode =
          SurfaceMaterial::AlphaMode::Mask;
    } else if (skate::world::HasFlag(
                   material.retail.render_flags,
                   RetailRenderFlags::AlphaBlend)) {
      material.alpha_mode =
          SurfaceMaterial::AlphaMode::Blend;
    }
    for (const TextureBinding& binding : source->second.textures) {
      TextureId package_texture = 0;
      const auto reused = texture_ids.find(binding.texture_id);
      if (reused != texture_ids.end()) {
        package_texture = reused->second;
      } else {
        const auto path = index.textures.find(binding.texture_id);
        if (path == index.textures.end()) {
          error =
              "texture " + HexId(binding.texture_id) +
              " is absent from parkassets.big";
          return false;
        }
        std::vector<std::uint8_t> texture_bytes;
        if (!ReadArchiveFile(
                archive, path->second, texture_bytes, error)) {
          error =
              "could not read texture " +
              HexId(binding.texture_id) + ": " + error;
          return false;
        }
        ImageTexture texture;
        if (!DecodeTexture(
                texture_bytes, binding.texture_id,
                ColorSpaceForChannel(binding.channel),
                texture, error)) {
          error =
              "could not decode texture " +
              HexId(binding.texture_id) + ": " + error;
          return false;
        }
        texture.id =
            static_cast<TextureId>(asset.textures.size() + 1);
        package_texture = texture.id;
        texture_ids.emplace(binding.texture_id, package_texture);
        asset.textures.push_back(std::move(texture));
      }
      material.retail.texture_bindings.push_back(
          {ClassifyRetailSemantic(binding.channel),
           package_texture,
           binding.channel == "decal" ? 2u : 0u,
           binding.channel == "decal" ? 1u : 0u,
           binding.channel == "decal" ? 1u : 0u});
      if (binding.channel == "diffuse" &&
          material.albedo_texture == 0) {
        material.albedo_texture = package_texture;
      } else if (
          binding.channel == "normal" &&
          material.normal_texture == 0) {
        material.normal_texture = package_texture;
      }
    }
    asset.materials.push_back(std::move(material));
  }

  MapObject object;
  object.id = 1;
  object.name = item.name;
  // These packages are editor placement assets, not host-simulated rigid
  // bodies. Their retained RX2 triangles are registered through Skate's
  // native collision path. Box3D participation made DMO props move/fly,
  // could reset the skater, and caused the grind transformer to exclude
  // their otherwise valid native spline records.
  object.physics.type = ObjectPhysicsType::Disabled;
  for (std::size_t mesh_index = 0;
       mesh_index < meshes.size(); ++mesh_index) {
    const std::uint32_t vertex_base =
        static_cast<std::uint32_t>(
            object.render_mesh.vertices.size());
    for (RenderVertex vertex : meshes[mesh_index].vertices) {
      vertex.material =
          static_cast<MaterialId>(mesh_index + 1);
      object.render_mesh.vertices.push_back(vertex);
    }
    for (const std::uint32_t source_index :
         meshes[mesh_index].indices) {
      object.render_mesh.indices.push_back(
          vertex_base + source_index);
    }
  }

  std::unordered_map<std::uint16_t, MaterialId>
      collision_materials;
  for (const RetailCollisionTriangle& source : retail_collision) {
    auto found = collision_materials.find(source.surface);
    if (found == collision_materials.end()) {
      SurfaceMaterial material;
      material.id =
          static_cast<MaterialId>(asset.materials.size() + 1);
      std::ostringstream name;
      name << item.name << " collision 0x" << std::hex
           << std::setw(4) << std::setfill('0')
           << source.surface;
      material.name = name.str();
      material.flags = SurfaceFlags::Skateable;
      material.display_color = {
          float((source.surface * 37u) & 255u) / 255.0f,
          float((source.surface * 67u) & 255u) / 255.0f,
          float((source.surface * 97u) & 255u) / 255.0f};
      material.skate_audio_surface =
          static_cast<std::uint8_t>(source.surface & 0x7Fu);
      material.skate_physics_surface =
          static_cast<std::uint8_t>(
              (source.surface >> 7u) & 0x1Fu);
      material.skate_surface_pattern =
          static_cast<std::uint8_t>(
              (source.surface >> 12u) & 0x0Fu);
      found = collision_materials
                  .emplace(source.surface, material.id)
                  .first;
      asset.materials.push_back(std::move(material));
    }
    CollisionTriangle triangle;
    triangle.a = source.a;
    triangle.b = source.b;
    triangle.c = source.c;
    triangle.surface = found->second;
    triangle.material = found->second;
    triangle.native_edge_codes = source.edge_codes;
    triangle.has_native_edge_codes = source.has_edge_codes;
    object.collision_triangles.push_back(triangle);
  }
  object.grind_rail_indices.reserve(rails.size());
  for (std::size_t rail = 0; rail < rails.size(); ++rail) {
    object.grind_rail_indices.push_back(
        static_cast<std::uint32_t>(rail));
  }
  asset.grind_rails = std::move(rails);
  asset.objects.push_back(std::move(object));
  return true;
}

std::filesystem::path OutputPath(
    const std::filesystem::path& root, const Item& item) {
  return root / item.category /
         (SanitizedFilename(item.name) + "_" +
          HexId(item.model_id).substr(2) + ".skateobj");
}

void Report(
    const ProgressCallback& callback, const Progress& progress) {
  if (callback) {
    callback(progress);
  }
}

}  // namespace

bool ParseRecipe(
    std::string_view xml, bool dynamic,
    Recipe& recipe, std::string& error) {
  recipe = {};
  error.clear();
  std::size_t cursor = 0;
  XmlTag tag;
  std::optional<Material> current_material;
  std::optional<Item> current_item;
  std::string category;
  bool in_lod_zero = false;
  bool saw_root = false;
  while (NextXmlTag(xml, cursor, tag, error)) {
    if (tag.closing) {
      if (tag.name == "mat" && current_material) {
        if (current_material->id == 0 ||
            current_material->type.empty() ||
            !recipe.materials
                 .emplace(
                     current_material->id,
                     std::move(*current_material))
                 .second) {
          error =
              "retail recipe contains an invalid or duplicate material";
          return false;
        }
        current_material.reset();
      } else if (tag.name == "lod") {
        in_lod_zero = false;
      } else if (tag.name == "mod" && current_item) {
        if (current_item->name.empty() ||
            current_item->model_id == 0 ||
            current_item->material_ids.empty()) {
          error =
              "retail recipe contains an incomplete droppable item";
          return false;
        }
        recipe.items.push_back(std::move(*current_item));
        current_item.reset();
      } else if (tag.name == "comp") {
        category.clear();
      }
      continue;
    }

    if (tag.name == "compositeasset") {
      saw_root = true;
    } else if (tag.name == "mat" && !current_item) {
      const std::string* id_text = Attribute(tag, "id");
      const std::string* type = Attribute(tag, "type");
      const auto id =
          id_text == nullptr ? std::nullopt : ParseId(*id_text);
      if (!id || type == nullptr) {
        error = "retail recipe material header is invalid";
        return false;
      }
      current_material = Material{*id, *type, {}};
    } else if (tag.name == "sp" && current_material) {
      const std::string* id_text = Attribute(tag, "id");
      const std::string* channel = Attribute(tag, "chn");
      const auto id =
          id_text == nullptr ? std::nullopt : ParseId(*id_text);
      if (!id || channel == nullptr || channel->empty()) {
        error = "retail recipe texture binding is invalid";
        return false;
      }
      current_material->textures.push_back({*channel, *id});
    } else if (tag.name == "comp") {
      const std::string* name = Attribute(tag, "n");
      if (name == nullptr || !SafeCategory(*name)) {
        error = "retail recipe category name is unsafe";
        return false;
      }
      category = *name;
    } else if (tag.name == "mod") {
      const std::string* name = Attribute(tag, "n");
      if (category.empty() || name == nullptr || name->empty()) {
        error = "retail recipe droppable item header is invalid";
        return false;
      }
      current_item =
          Item{category, *name, 0, {}, dynamic};
    } else if (tag.name == "lod" && current_item) {
      const std::string* index = Attribute(tag, "idx");
      in_lod_zero = index == nullptr || *index == "0";
      if (in_lod_zero) {
        const std::string* arena_id =
            Attribute(tag, "arenaid");
        const auto id = arena_id == nullptr
                            ? std::nullopt
                            : ParseId(*arena_id);
        if (!id) {
          error = "retail recipe item model ID is invalid";
          return false;
        }
        current_item->model_id = *id;
      }
    } else if (
        tag.name == "matvar" && current_item &&
        in_lod_zero) {
      const std::string* id_text = Attribute(tag, "id");
      const auto id =
          id_text == nullptr ? std::nullopt : ParseId(*id_text);
      if (!id) {
        error = "retail recipe item material ID is invalid";
        return false;
      }
      current_item->material_ids.push_back(*id);
    }

    if (tag.self_closing) {
      if (tag.name == "lod") {
        in_lod_zero = false;
      }
    }
  }
  if (!error.empty()) {
    return false;
  }
  if (!saw_root || current_material || current_item ||
      recipe.materials.empty() || recipe.items.empty()) {
    error = "retail recipe is incomplete";
    return false;
  }
  return true;
}

RetailShaderFamily ShaderFamilyForMaterialType(
    std::string_view type) {
  return ClassifyShaderFamily(type);
}

std::string RetailTextureSemantic(std::string_view channel) {
  return ClassifyRetailSemantic(std::string(channel));
}

Result ImportDefaults(
    const std::filesystem::path& game_data_root,
    const std::filesystem::path& object_library_root,
    const ProgressCallback& callback) {
  Result result;
  Progress& progress = result;
  progress.message = "Opening the installed Skate 3 park catalogue";
  Report(callback, progress);
  const std::filesystem::path archive_path =
      game_data_root / "data" / "content" / "parkassets.big";
  if (!std::filesystem::is_regular_file(archive_path)) {
    result.errors.push_back(
        "Missing data/content/parkassets.big under the configured Skate 3 "
        "game-data folder. Reinstall or select a complete game-data folder.");
    progress.message = result.errors.back();
    Report(callback, progress);
    return result;
  }

  cac_archive::Archive archive;
  std::string error;
  if (!archive.Open(archive_path, error)) {
    result.errors.push_back(
        "Could not open parkassets.big: " + error);
    progress.message = result.errors.back();
    Report(callback, progress);
    return result;
  }
  const std::vector<std::string> recipe_paths =
      archive.PathsWithPrefix("data/content/recipe/");
  const auto find_recipe =
      [&recipe_paths](std::string_view kind) {
        return std::find_if(
            recipe_paths.begin(), recipe_paths.end(),
            [kind](const std::string& path) {
              const std::string folded = Fold(path);
              return folded.ends_with(".xml") &&
                     folded.find(
                         "/recipe/" + std::string(kind) + "/") !=
                         std::string::npos;
            });
      };
  const auto static_recipe_path = find_recipe("static");
  const auto dynamic_recipe_path = find_recipe("dynamic");
  if (static_recipe_path == recipe_paths.end() ||
      dynamic_recipe_path == recipe_paths.end()) {
    result.errors.push_back(
        "parkassets.big does not contain the supported Skate Park Recipe "
        "static and dynamic catalogues.");
    progress.message = result.errors.back();
    Report(callback, progress);
    return result;
  }

  Recipe static_recipe;
  Recipe dynamic_recipe;
  for (const auto& [path, dynamic, destination] :
       {std::tuple<const std::string*, bool, Recipe*>{
            &*static_recipe_path, false, &static_recipe},
        std::tuple<const std::string*, bool, Recipe*>{
            &*dynamic_recipe_path, true, &dynamic_recipe}}) {
    std::vector<std::uint8_t> bytes;
    if (!archive.Read(*path, bytes, error) ||
        !ParseRecipe(
            std::string_view(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size()),
            dynamic, *destination, error)) {
      result.errors.push_back(
          "Could not read the retail " +
          std::string(dynamic ? "dynamic" : "static") +
          " drop-item recipe: " + error);
      progress.message = result.errors.back();
      Report(callback, progress);
      return result;
    }
  }

  AssetIndex static_index;
  AssetIndex dynamic_index;
  IndexPaths(
      archive.PathsWithPrefix("data/content/static/model/"),
      static_index.models);
  IndexPaths(
      archive.PathsWithPrefix("data/content/static/texture/"),
      static_index.textures);
  IndexPaths(
      archive.PathsWithPrefix("data/content/dynamic/model/"),
      dynamic_index.models);
  IndexPaths(
      archive.PathsWithPrefix("data/content/dynamic/texture/"),
      dynamic_index.textures);
  progress.total =
      static_recipe.items.size() + dynamic_recipe.items.size();
  progress.message =
      "Found " + std::to_string(progress.total) +
      " retail catalogue records";
  Report(callback, progress);

  std::error_code filesystem_error;
  std::filesystem::create_directories(
      object_library_root, filesystem_error);
  if (filesystem_error) {
    result.errors.push_back(
        "Could not create the local object library: " +
        filesystem_error.message());
    progress.message = result.errors.back();
    Report(callback, progress);
    return result;
  }
  const auto import_recipe =
      [&](const Recipe& recipe, const AssetIndex& index) {
        for (const Item& item : recipe.items) {
          const std::filesystem::path output =
              OutputPath(object_library_root, item);
          progress.message =
              "Converting " + item.category + " / " + item.name;
          Report(callback, progress);
          bool reused = false;
          if (std::filesystem::is_regular_file(
                  output, filesystem_error) &&
              !filesystem_error) {
            try {
              reused = CurrentGeneratedAsset(
                  skate::world::LoadSkateObjectPackage(output));
            } catch (...) {
              reused = false;
            }
          }
          filesystem_error.clear();
          if (reused) {
            ++progress.reused;
            ++progress.completed;
            Report(callback, progress);
            continue;
          }

          const auto model_path = index.models.find(item.model_id);
          std::vector<std::uint8_t> model_header;
          if (model_path != index.models.end() &&
              archive.Read(
                  model_path->second, model_header, error) &&
              model_header.size() >= 0x5C &&
              ReadBe32(model_header, 0x58) != 4u) {
            ++progress.unsupported;
            ++progress.completed;
            progress.message =
                "Skipped non-model helper " + item.category +
                " / " + item.name;
            Report(callback, progress);
            continue;
          }

          SkateObjectAsset asset;
          if (!BuildAsset(
                  item, recipe, index, archive, asset, error)) {
            result.errors.push_back(
                item.category + " / " + item.name + ": " + error);
            ++progress.completed;
            Report(callback, progress);
            continue;
          }
          try {
            skate::world::SaveSkateObjectPackage(output, asset);
            ++progress.written;
          } catch (const std::exception& exception) {
            result.errors.push_back(
                item.category + " / " + item.name + ": " +
                exception.what());
          }
          ++progress.completed;
          Report(callback, progress);
        }
      };
  import_recipe(static_recipe, static_index);
  import_recipe(dynamic_recipe, dynamic_index);

  if (result.errors.empty()) {
    progress.message =
        "Default library ready: " +
        std::to_string(progress.written) + " written, " +
        std::to_string(progress.reused) + " reused, " +
        std::to_string(progress.unsupported) +
        " non-model helpers skipped";
    std::ofstream state(
        object_library_root / ".default-items-import-state",
        std::ios::trunc);
    state << "schema=2\narchive=" << archive.CacheKey()
          << "\nwritten=" << progress.written
          << "\nreused=" << progress.reused
          << "\nunsupported=" << progress.unsupported << '\n';
  } else {
    progress.message =
        "Default library incomplete: " +
        std::to_string(result.errors.size()) +
        " item(s) failed. Rerun setup after correcting the errors.";
  }
  Report(callback, progress);
  return result;
}

}  // namespace skate3::drop_item_import
