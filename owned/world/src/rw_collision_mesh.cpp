#include "skate/world/rw_collision_mesh.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skate::world {
namespace {

constexpr std::size_t kMeshHeaderSize = 96;
constexpr std::size_t kKdHeaderSize = 48;
constexpr std::size_t kClusterHeaderSize = 16;
constexpr std::size_t kBytesPerTriangleUnit = 9;
constexpr std::uint8_t kTriangleUnitFlags = 0xa1;
constexpr std::uint16_t kOneSidedClusterFlag = 0x10;
constexpr float kMinimumTriangleNormalSquared = 1.0e-10f;

std::size_t Align16(std::size_t value) {
  return (value + 15u) & ~std::size_t{15u};
}

std::uint32_t ReadBeU32(std::span<const std::uint8_t> bytes,
                        std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8u) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

void AppendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value) {
  bytes.push_back(value);
}

void AppendBeU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void AppendLeU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void AppendBeU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void AppendBeF32(std::vector<std::uint8_t>& bytes, float value) {
  AppendBeU32(bytes, std::bit_cast<std::uint32_t>(value));
}

void WriteBeU16(std::vector<std::uint8_t>& bytes,
                std::size_t offset,
                std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 8u);
  bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteBeU32(std::span<std::uint8_t> bytes,
                std::size_t offset,
                std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24u);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16u);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8u);
  bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void PadTo16(std::vector<std::uint8_t>& bytes) {
  bytes.resize(Align16(bytes.size()), 0);
}

bool IsFinite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

struct QuantizedVertex {
  std::int64_t x;
  std::int64_t y;
  std::int64_t z;

  bool operator==(const QuantizedVertex&) const = default;
};

struct QuantizedVertexHash {
  std::size_t operator()(const QuantizedVertex& value) const noexcept {
    std::size_t result = std::hash<std::int64_t>{}(value.x);
    result ^= std::hash<std::int64_t>{}(value.y) +
              0x9e3779b97f4a7c15ull + (result << 6u) + (result >> 2u);
    result ^= std::hash<std::int64_t>{}(value.z) +
              0x9e3779b97f4a7c15ull + (result << 6u) + (result >> 2u);
    return result;
  }
};

QuantizedVertex Quantize(Vec3 value, float epsilon) {
  const double inverse = 1.0 / static_cast<double>(epsilon);
  return {
      static_cast<std::int64_t>(
          std::llround(static_cast<double>(value.x) * inverse)),
      static_cast<std::int64_t>(
          std::llround(static_cast<double>(value.y) * inverse)),
      static_cast<std::int64_t>(
          std::llround(static_cast<double>(value.z) * inverse)),
  };
}

struct Triangle {
  std::array<std::uint32_t, 3> vertices{};
  std::array<std::uint8_t, 3> edge_codes{};
  bool has_native_edge_codes = false;
  std::uint16_t surface = 0;
  Vec3 normal;
};

struct Bounds {
  Vec3 min;
  Vec3 max;
};

struct KdNode {
  Bounds bounds{};
  std::size_t first = 0;
  std::size_t count = 0;
  std::uint32_t axis = 0;
  std::uint32_t branch_index = 0;
  std::uint32_t parent_branch = 0;
  std::unique_ptr<KdNode> left;
  std::unique_ptr<KdNode> right;

  bool IsLeaf() const {
    return left == nullptr;
  }
};

constexpr std::size_t kMaximumClusterVertices = 255;
// Cluster sizes are stored in a big-endian uint16 and each serialized cluster
// is 16-byte aligned. 65520 is therefore the largest representable aligned
// cluster span.
constexpr std::size_t kMaximumClusterBytes =
    std::numeric_limits<std::uint16_t>::max() & ~std::size_t{15};

bool RangeFitsCluster(
    const std::vector<std::size_t>& order,
    std::size_t first,
    std::size_t count,
    const std::vector<Triangle>& triangles) {
  // Reject ranges that cannot fit even before accounting for vertices. This
  // avoids building large temporary sets near the root of a map-sized KD tree.
  if (count >
      (kMaximumClusterBytes - kClusterHeaderSize) /
          kBytesPerTriangleUnit) {
    return false;
  }

  std::unordered_set<std::uint32_t> unique_vertices;
  unique_vertices.reserve(
      std::min(kMaximumClusterVertices + 1, count * 3));
  for (std::size_t index = first; index < first + count; ++index) {
    for (std::uint32_t vertex : triangles[order[index]].vertices) {
      unique_vertices.insert(vertex);
      if (unique_vertices.size() > kMaximumClusterVertices) {
        return false;
      }
    }
  }

  const std::size_t serialized_bytes = Align16(
      kClusterHeaderSize +
      unique_vertices.size() * sizeof(float) * 4 +
      count * kBytesPerTriangleUnit);
  return serialized_bytes <= kMaximumClusterBytes;
}

Bounds TriangleBounds(const Triangle& triangle,
                      const std::vector<Vec3>& vertices) {
  Bounds bounds{vertices[triangle.vertices[0]],
                vertices[triangle.vertices[0]]};
  for (std::size_t corner = 1; corner < 3; ++corner) {
    const Vec3 vertex = vertices[triangle.vertices[corner]];
    bounds.min.x = std::min(bounds.min.x, vertex.x);
    bounds.min.y = std::min(bounds.min.y, vertex.y);
    bounds.min.z = std::min(bounds.min.z, vertex.z);
    bounds.max.x = std::max(bounds.max.x, vertex.x);
    bounds.max.y = std::max(bounds.max.y, vertex.y);
    bounds.max.z = std::max(bounds.max.z, vertex.z);
  }
  return bounds;
}

Bounds RangeBounds(const std::vector<std::size_t>& order,
                   std::size_t first,
                   std::size_t count,
                   const std::vector<Triangle>& triangles,
                   const std::vector<Vec3>& vertices) {
  Bounds bounds = TriangleBounds(triangles[order[first]], vertices);
  for (std::size_t index = first + 1; index < first + count; ++index) {
    const Bounds triangle =
        TriangleBounds(triangles[order[index]], vertices);
    bounds.min.x = std::min(bounds.min.x, triangle.min.x);
    bounds.min.y = std::min(bounds.min.y, triangle.min.y);
    bounds.min.z = std::min(bounds.min.z, triangle.min.z);
    bounds.max.x = std::max(bounds.max.x, triangle.max.x);
    bounds.max.y = std::max(bounds.max.y, triangle.max.y);
    bounds.max.z = std::max(bounds.max.z, triangle.max.z);
  }
  return bounds;
}

float Component(Vec3 value, std::uint32_t axis) {
  switch (axis) {
    case 0:
      return value.x;
    case 1:
      return value.y;
    default:
      return value.z;
  }
}

std::unique_ptr<KdNode> BuildKdTree(
    std::vector<std::size_t>& order,
    std::size_t first,
    std::size_t count,
    const std::vector<Triangle>& triangles,
    const std::vector<Vec3>& vertices) {
  auto node = std::make_unique<KdNode>();
  node->bounds =
      RangeBounds(order, first, count, triangles, vertices);
  node->first = first;
  node->count = count;
  // Retail ClusteredMesh leaves are bounded by the format, not an arbitrary
  // triangle count. Keeping connected, vertex-sharing strips together avoids
  // introducing unnecessary internal partitions in ramps and curved floors.
  if (RangeFitsCluster(order, first, count, triangles)) {
    return node;
  }

  const Vec3 extent = node->bounds.max - node->bounds.min;
  node->axis = extent.y > extent.x
                   ? (extent.z > extent.y ? 2u : 1u)
                   : (extent.z > extent.x ? 2u : 0u);
  const std::size_t middle = first + count / 2;
  std::nth_element(
      order.begin() + static_cast<std::ptrdiff_t>(first),
      order.begin() + static_cast<std::ptrdiff_t>(middle),
      order.begin() + static_cast<std::ptrdiff_t>(first + count),
      [&](std::size_t left, std::size_t right) {
        const Bounds left_bounds =
            TriangleBounds(triangles[left], vertices);
        const Bounds right_bounds =
            TriangleBounds(triangles[right], vertices);
        const float left_centroid =
            0.5f * (Component(left_bounds.min, node->axis) +
                    Component(left_bounds.max, node->axis));
        const float right_centroid =
            0.5f * (Component(right_bounds.min, node->axis) +
                    Component(right_bounds.max, node->axis));
        return left_centroid == right_centroid
                   ? left < right
                   : left_centroid < right_centroid;
      });

  const std::size_t left_count = middle - first;
  node->left =
      BuildKdTree(order, first, left_count, triangles, vertices);
  node->right = BuildKdTree(order, middle, count - left_count,
                            triangles, vertices);
  return node;
}

void FlattenKdBranches(KdNode& node,
                       std::uint32_t parent,
                       std::vector<KdNode*>& branches) {
  if (node.IsLeaf()) {
    return;
  }
  node.branch_index = static_cast<std::uint32_t>(branches.size());
  node.parent_branch = parent;
  branches.push_back(&node);
  FlattenKdBranches(*node.left, node.branch_index, branches);
  FlattenKdBranches(*node.right, node.branch_index, branches);
}

void CollectKdLeaves(KdNode& node, std::vector<KdNode*>& leaves) {
  if (node.IsLeaf()) {
    leaves.push_back(&node);
    return;
  }
  CollectKdLeaves(*node.left, leaves);
  CollectKdLeaves(*node.right, leaves);
}

struct ClusterTriangle {
  std::array<std::uint8_t, 3> vertices{};
  std::array<std::uint8_t, 3> edge_codes{};
  std::uint16_t surface = 0;
};

struct Cluster {
  std::vector<Vec3> vertices;
  std::vector<ClusterTriangle> triangles;
};

struct EdgeKey {
  std::uint32_t from;
  std::uint32_t to;

  bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash {
  std::size_t operator()(const EdgeKey& edge) const noexcept {
    std::size_t result = std::hash<std::uint32_t>{}(edge.from);
    result ^= std::hash<std::uint32_t>{}(edge.to) +
              0x9e3779b97f4a7c15ull + (result << 6u) + (result >> 2u);
    return result;
  }
};

struct EdgeOwner {
  std::size_t triangle;
  std::size_t edge;
};

float ExtendedEdgeCosine(Vec3 normal_a,
                         Vec3 normal_b,
                         Vec3 edge_direction) {
  const float cosine = Dot(normal_a, normal_b);
  const float orientation = Dot(edge_direction, Cross(normal_a, normal_b));
  if (orientation > -1.0e-6f) {
    return std::max(cosine, -1.0f);
  }
  return std::min(2.0f - cosine, 3.0f);
}

std::uint8_t EdgeCosineToAngleByte(float edge_cosine) {
  const float angle = std::max(
      edge_cosine > 1.0f ? std::acos(2.0f - edge_cosine)
                         : std::acos(edge_cosine),
      6.6e-5f);
  int value = static_cast<int>(
      -2.0f * std::log(angle / std::numbers::pi_v<float>) /
      std::log(2.0f));
  value = std::clamp(value, 0, 26);
  return static_cast<std::uint8_t>(value);
}

std::uint8_t MakeEdgeCode(bool has_neighbor, float edge_cosine) {
  float encoded_cosine = edge_cosine;
  if (!has_neighbor && encoded_cosine < 0.0f) {
    encoded_cosine = 1.0f;
  }
  std::uint8_t result = EdgeCosineToAngleByte(encoded_cosine);
  if (edge_cosine < 1.0f) {
    result |= 0x20u;
  }
  if (edge_cosine > 3.0f) {
    result = 26;
  }
  if (!has_neighbor) {
    result |= 0x80u;
  }
  return result;
}

std::uint32_t BitWidth(std::uint32_t value) {
  std::uint32_t width = 0;
  do {
    ++width;
    value >>= 1u;
  } while (value != 0);
  return width;
}

std::uint32_t ComputeClusterTagBits(std::uint32_t cluster_count) {
  return 1u + static_cast<std::uint32_t>(
                  std::log2(std::max(1u, cluster_count)));
}

std::uint32_t ComputeTagBits(std::uint32_t cluster_count,
                             std::uint32_t maximum_unit_bytes) {
  const std::uint32_t cluster_tag_bits =
      ComputeClusterTagBits(cluster_count);
  const std::uint32_t unit_tag_bits = BitWidth(maximum_unit_bytes);
  return cluster_tag_bits + unit_tag_bits + 1u;
}

std::uint16_t ResolveSurface(
    const CollisionTriangle& triangle,
    const RwCollisionBuildOptions& options) {
  const auto found = options.material_surface_ids.find(triangle.material);
  return found == options.material_surface_ids.end()
             ? options.default_surface_id
             : found->second;
}

}  // namespace

RwCollisionBuildResult BuildRwCollisionMesh(
    const MapDefinition& map,
    const RwCollisionBuildOptions& options) {
  RwCollisionBuildResult result;
  if (!(options.weld_epsilon > 0.0f) ||
      !std::isfinite(options.weld_epsilon)) {
    result.error = "weld epsilon must be finite and greater than zero";
    return result;
  }
  if (!(options.granularity > 0.0f) ||
      !std::isfinite(options.granularity)) {
    result.error = "granularity must be finite and greater than zero";
    return result;
  }
  if (!IsFinite(options.translation)) {
    result.error = "collision translation must be finite";
    return result;
  }
  if (map.collision_triangles.empty()) {
    result.error = "map has no collision triangles";
    return result;
  }

  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
  std::unordered_map<QuantizedVertex, std::uint32_t, QuantizedVertexHash>
      welded;

  auto add_vertex = [&](Vec3 position) -> std::optional<std::uint32_t> {
    if (!IsFinite(position)) {
      return std::nullopt;
    }
    const QuantizedVertex key = Quantize(position, options.weld_epsilon);
    if (const auto found = welded.find(key); found != welded.end()) {
      return found->second;
    }
    if (vertices.size() >=
        std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    const auto index = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back(position);
    welded.emplace(key, index);
    return index;
  };

  for (const CollisionTriangle& source : map.collision_triangles) {
    if (!IsFinite(source.a) || !IsFinite(source.b) ||
        !IsFinite(source.c)) {
      result.error = "collision geometry contains a non-finite vertex";
      return result;
    }
    const Vec3 cross = Cross(source.b - source.a, source.c - source.a);
    if (LengthSquared(cross) <= kMinimumTriangleNormalSquared) {
      continue;
    }
    const auto a = add_vertex(source.a + options.translation);
    const auto b = add_vertex(source.b + options.translation);
    const auto c = add_vertex(source.c + options.translation);
    if (!a || !b || !c) {
      result.error = "collision mesh exceeds the 32-bit source vertex limit";
      return result;
    }
    triangles.push_back(
        {{{*a, *b, *c}},
         source.native_edge_codes,
         source.has_native_edge_codes,
         ResolveSurface(source, options),
         Normalize(cross)});
  }

  if (triangles.empty()) {
    result.error = "all collision triangles are degenerate";
    return result;
  }
  if (triangles.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    result.error = "collision mesh exceeds the 32-bit triangle limit";
    return result;
  }

  std::vector<std::array<bool, 3>> has_neighbor(triangles.size());
  std::vector<std::array<float, 3>> edge_cosines(
      triangles.size(), {-1.0f, -1.0f, -1.0f});
  std::unordered_map<EdgeKey, EdgeOwner, EdgeKeyHash> open_edges;
  for (std::size_t triangle_index = 0;
       triangle_index < triangles.size(); ++triangle_index) {
    Triangle& triangle = triangles[triangle_index];
    for (std::size_t edge = 0; edge < 3; ++edge) {
      const std::uint32_t from = triangle.vertices[edge];
      const std::uint32_t to = triangle.vertices[(edge + 1u) % 3u];
      const auto reverse = open_edges.find({to, from});
      if (reverse == open_edges.end()) {
        open_edges.emplace(EdgeKey{from, to},
                           EdgeOwner{triangle_index, edge});
        continue;
      }

      const EdgeOwner other = reverse->second;
      has_neighbor[triangle_index][edge] = true;
      has_neighbor[other.triangle][other.edge] = true;
      const Vec3 direction =
          vertices[to] - vertices[from];
      const float cosine = ExtendedEdgeCosine(
          triangle.normal, triangles[other.triangle].normal, direction);
      edge_cosines[triangle_index][edge] = cosine;
      edge_cosines[other.triangle][other.edge] = cosine;
      open_edges.erase(reverse);
    }
  }

  for (std::size_t triangle_index = 0;
       triangle_index < triangles.size(); ++triangle_index) {
    for (std::size_t edge = 0; edge < 3; ++edge) {
      if (triangles[triangle_index].has_native_edge_codes) {
        continue;
      }
      triangles[triangle_index].edge_codes[edge] =
          MakeEdgeCode(has_neighbor[triangle_index][edge],
                       edge_cosines[triangle_index][edge]);
    }
  }

  // RenderWare's vertex smoothing bit removes false point features where all
  // triangles sharing a welded vertex are coplanar.
  std::vector<std::vector<std::size_t>> vertex_triangles(vertices.size());
  for (std::size_t triangle_index = 0;
       triangle_index < triangles.size(); ++triangle_index) {
    for (std::uint32_t vertex : triangles[triangle_index].vertices) {
      vertex_triangles[vertex].push_back(triangle_index);
    }
  }
  for (std::size_t vertex = 0; vertex < vertex_triangles.size(); ++vertex) {
    const auto& adjacent = vertex_triangles[vertex];
    if (adjacent.empty()) {
      continue;
    }
    const Vec3 reference = triangles[adjacent.front()].normal;
    const bool coplanar = std::all_of(
        adjacent.begin(), adjacent.end(), [&](std::size_t triangle) {
          return std::abs(Dot(reference, triangles[triangle].normal) -
                          1.0f) <= 0.01f;
        });
    if (!coplanar) {
      continue;
    }
    for (std::size_t triangle_index : adjacent) {
      Triangle& triangle = triangles[triangle_index];
      if (triangle.has_native_edge_codes) {
        continue;
      }
      for (std::size_t corner = 0; corner < 3; ++corner) {
        if (triangle.vertices[corner] == vertex) {
          triangle.edge_codes[corner] |= 0x40u;
        }
      }
    }
  }

  Vec3 bounds_min = vertices.front();
  Vec3 bounds_max = vertices.front();
  for (Vec3 vertex : vertices) {
    bounds_min.x = std::min(bounds_min.x, vertex.x);
    bounds_min.y = std::min(bounds_min.y, vertex.y);
    bounds_min.z = std::min(bounds_min.z, vertex.z);
    bounds_max.x = std::max(bounds_max.x, vertex.x);
    bounds_max.y = std::max(bounds_max.y, vertex.y);
    bounds_max.z = std::max(bounds_max.z, vertex.z);
  }

  std::vector<std::size_t> triangle_order(triangles.size());
  std::iota(triangle_order.begin(), triangle_order.end(), 0);
  std::unique_ptr<KdNode> kd_root =
      BuildKdTree(triangle_order, 0, triangle_order.size(),
                  triangles, vertices);
  std::vector<KdNode*> kd_branches;
  FlattenKdBranches(*kd_root, 0, kd_branches);
  std::vector<KdNode*> kd_leaves;
  CollectKdLeaves(*kd_root, kd_leaves);
  if (kd_leaves.size() > 65536u) {
    result.error =
        "collision mesh exceeds the 16-bit native cluster-index limit";
    return result;
  }

  std::vector<Cluster> clusters;
  clusters.reserve(kd_leaves.size());
  std::unordered_map<const KdNode*, std::uint32_t> leaf_cluster_ids;
  std::uint32_t maximum_cluster_vertices = 0;
  std::uint32_t maximum_unit_bytes = 0;
  for (KdNode* leaf : kd_leaves) {
    Cluster cluster;
    std::unordered_map<std::uint32_t, std::uint8_t> local_vertices;
    for (std::size_t ordered_index = leaf->first;
         ordered_index < leaf->first + leaf->count; ++ordered_index) {
      const Triangle& source = triangles[triangle_order[ordered_index]];
      ClusterTriangle output;
      output.edge_codes = source.edge_codes;
      output.surface = source.surface;
      for (std::size_t corner = 0; corner < 3; ++corner) {
        const std::uint32_t global_index = source.vertices[corner];
        const auto found = local_vertices.find(global_index);
        if (found != local_vertices.end()) {
          output.vertices[corner] = found->second;
          continue;
        }
        if (cluster.vertices.size() >= kMaximumClusterVertices) {
          result.error =
              "KD leaf exceeds the native 255-vertex cluster limit";
          return result;
        }
        const auto local_index =
            static_cast<std::uint8_t>(cluster.vertices.size());
        cluster.vertices.push_back(vertices[global_index]);
        local_vertices.emplace(global_index, local_index);
        output.vertices[corner] = local_index;
      }
      cluster.triangles.push_back(output);
    }
    const std::uint32_t cluster_id =
        static_cast<std::uint32_t>(clusters.size());
    leaf_cluster_ids.emplace(leaf, cluster_id);
    maximum_cluster_vertices = std::max(
        maximum_cluster_vertices,
        static_cast<std::uint32_t>(cluster.vertices.size()));
    maximum_unit_bytes = std::max(
        maximum_unit_bytes,
        static_cast<std::uint32_t>(
            cluster.triangles.size() * kBytesPerTriangleUnit));
    clusters.push_back(std::move(cluster));
  }

  std::vector<std::uint8_t> bytes(kMeshHeaderSize, 0);
  auto write_vec3 = [&](std::size_t offset, Vec3 value) {
    WriteBeU32(bytes, offset, std::bit_cast<std::uint32_t>(value.x));
    WriteBeU32(bytes, offset + 4,
               std::bit_cast<std::uint32_t>(value.y));
    WriteBeU32(bytes, offset + 8,
               std::bit_cast<std::uint32_t>(value.z));
  };
  write_vec3(0, bounds_min);
  write_vec3(16, bounds_max);

  const auto triangle_count =
      static_cast<std::uint32_t>(triangles.size());
  const auto cluster_count =
      static_cast<std::uint32_t>(clusters.size());
  WriteBeU32(bytes, 36,
             ComputeTagBits(cluster_count, maximum_unit_bytes));
  WriteBeU32(bytes, 40, triangle_count);

  const std::size_t kd_offset = bytes.size();
  AppendBeU32(
      bytes,
      kd_branches.empty()
          ? 0u
          : static_cast<std::uint32_t>(kd_offset + kKdHeaderSize));
  AppendBeU32(bytes,
              static_cast<std::uint32_t>(kd_branches.size()));
  AppendBeU32(bytes, triangle_count);
  AppendBeU32(bytes, 0);
  AppendBeF32(bytes, bounds_min.x);
  AppendBeF32(bytes, bounds_min.y);
  AppendBeF32(bytes, bounds_min.z);
  AppendBeU32(bytes, 0);
  AppendBeF32(bytes, bounds_max.x);
  AppendBeF32(bytes, bounds_max.y);
  AppendBeF32(bytes, bounds_max.z);
  AppendBeU32(bytes, 0);
  if (bytes.size() != kd_offset + kKdHeaderSize) {
    result.error = "internal KD header size mismatch";
    return result;
  }
  for (const KdNode* branch : kd_branches) {
    AppendBeU32(bytes, branch->parent_branch);
    AppendBeU32(bytes, branch->axis);
    const auto append_entry = [&](const KdNode& child) {
      if (child.IsLeaf()) {
        AppendBeU32(bytes, static_cast<std::uint32_t>(child.count));
        const auto cluster = leaf_cluster_ids.find(&child);
        if (cluster == leaf_cluster_ids.end()) {
          throw std::logic_error(
              "KD leaf has no collision cluster assignment");
        }
        // Native query entries pack cluster ID in the high 16 bits and byte
        // offset into that cluster's unit stream in the low 16. Each KD leaf
        // owns one compact cluster here, so its unit offset is zero.
        AppendBeU32(bytes, cluster->second << 16u);
      } else {
        AppendBeU32(bytes,
                    std::numeric_limits<std::uint32_t>::max());
        AppendBeU32(bytes, child.branch_index);
      }
    };
    append_entry(*branch->left);
    append_entry(*branch->right);
    AppendBeF32(
        bytes, Component(branch->left->bounds.max, branch->axis));
    AppendBeF32(
        bytes, Component(branch->right->bounds.min, branch->axis));
  }

  PadTo16(bytes);
  const std::size_t cluster_table_offset = bytes.size();
  for (std::size_t cluster = 0; cluster < clusters.size(); ++cluster) {
    AppendBeU32(bytes, 0);
  }
  PadTo16(bytes);
  for (std::size_t cluster_index = 0;
       cluster_index < clusters.size(); ++cluster_index) {
    const Cluster& cluster = clusters[cluster_index];
    const std::size_t cluster_offset = bytes.size();
    WriteBeU32(bytes,
               cluster_table_offset + cluster_index * sizeof(std::uint32_t),
               static_cast<std::uint32_t>(cluster_offset));

    AppendBeU16(
        bytes, static_cast<std::uint16_t>(cluster.triangles.size()));
    AppendBeU16(
        bytes,
        static_cast<std::uint16_t>(
            cluster.triangles.size() * kBytesPerTriangleUnit));
    AppendBeU16(
        bytes, static_cast<std::uint16_t>(cluster.vertices.size()));
    AppendBeU16(
        bytes, static_cast<std::uint16_t>(cluster.vertices.size()));
    const std::size_t cluster_size_offset = bytes.size();
    AppendBeU16(bytes, 0);
    AppendU8(bytes, static_cast<std::uint8_t>(cluster.vertices.size()));
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);  // uncompressed vertices
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);
    if (bytes.size() != cluster_offset + kClusterHeaderSize) {
      result.error = "internal cluster header size mismatch";
      return result;
    }

    for (Vec3 vertex : cluster.vertices) {
      AppendBeF32(bytes, vertex.x);
      AppendBeF32(bytes, vertex.y);
      AppendBeF32(bytes, vertex.z);
      AppendBeF32(bytes, 0.0f);
    }
    PadTo16(bytes);

    for (const ClusterTriangle& triangle : cluster.triangles) {
      AppendU8(bytes, kTriangleUnitFlags);
      AppendU8(bytes, triangle.vertices[0]);
      AppendU8(bytes, triangle.vertices[1]);
      AppendU8(bytes, triangle.vertices[2]);
      AppendU8(bytes, triangle.edge_codes[0]);
      AppendU8(bytes, triangle.edge_codes[1]);
      AppendU8(bytes, triangle.edge_codes[2]);
      AppendLeU16(bytes, triangle.surface);
    }
    PadTo16(bytes);

    const std::size_t cluster_size = bytes.size() - cluster_offset;
    if (cluster_size > std::numeric_limits<std::uint16_t>::max()) {
      result.error = "serialized cluster is larger than 65535 bytes";
      return result;
    }
    WriteBeU16(bytes, cluster_size_offset,
               static_cast<std::uint16_t>(cluster_size));
  }

  WriteBeU32(bytes, 48, static_cast<std::uint32_t>(kd_offset));
  WriteBeU32(bytes, 52,
             static_cast<std::uint32_t>(cluster_table_offset));

  const std::uint64_t cluster_params =
      (static_cast<std::uint64_t>(
           std::bit_cast<std::uint32_t>(options.granularity))
       << 32u) |
      (static_cast<std::uint64_t>(kOneSidedClusterFlag) << 16u) | 2u;
  WriteBeU32(bytes, 56,
             static_cast<std::uint32_t>(cluster_params >> 32u));
  WriteBeU32(bytes, 60, static_cast<std::uint32_t>(cluster_params));
  WriteBeU32(bytes, 64, cluster_count);
  WriteBeU32(bytes, 68, cluster_count);
  WriteBeU32(bytes, 72, triangle_count);
  WriteBeU32(bytes, 76, triangle_count);
  WriteBeU32(bytes, 80, static_cast<std::uint32_t>(bytes.size()));
  bytes[88] = 0x80;
  WriteBeU32(bytes, 92, ComputeClusterTagBits(cluster_count));

  result.ok = true;
  result.mesh.bytes = std::move(bytes);
  result.mesh.triangle_count = triangle_count;
  result.mesh.vertex_count = static_cast<std::uint32_t>(vertices.size());
  result.mesh.cluster_count = cluster_count;
  result.mesh.maximum_cluster_vertex_count = maximum_cluster_vertices;
  result.mesh.bounds_min = bounds_min;
  result.mesh.bounds_max = bounds_max;
  return result;
}

RwCollisionBuildResult LoadSerializedRwCollisionMesh(
    std::span<const std::uint8_t> source) {
  RwCollisionBuildResult result;
  if (source.size() < kMeshHeaderSize) {
    result.error = "serialized retail collision mesh is smaller than its header";
    return result;
  }
  const std::uint32_t mesh_bytes = ReadBeU32(source, 80);
  const std::uint32_t kd_offset = ReadBeU32(source, 48);
  const std::uint32_t cluster_table_offset = ReadBeU32(source, 52);
  const std::uint32_t cluster_count = ReadBeU32(source, 64);
  if (mesh_bytes != source.size() ||
      (kd_offset & 0x0fu) != 0 ||
      kd_offset > source.size() - kKdHeaderSize ||
      (cluster_table_offset & 0x0fu) != 0 ||
      cluster_table_offset > source.size() ||
      cluster_count >
          (source.size() - cluster_table_offset) / sizeof(std::uint32_t)) {
    result.error = "serialized retail collision mesh header is invalid";
    return result;
  }

  std::uint64_t vertex_count = 0;
  std::uint32_t maximum_cluster_vertices = 0;
  for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
    const std::uint32_t cluster_offset =
        ReadBeU32(source, cluster_table_offset +
                             cluster * sizeof(std::uint32_t));
    if (cluster_offset > source.size() - kClusterHeaderSize) {
      result.error = "serialized retail collision cluster offset is invalid";
      return result;
    }
    const std::uint32_t cluster_bytes =
        (static_cast<std::uint32_t>(source[cluster_offset + 8]) << 8u) |
        source[cluster_offset + 9];
    if (cluster_bytes < kClusterHeaderSize ||
        cluster_bytes > source.size() - cluster_offset) {
      result.error = "serialized retail collision cluster size is invalid";
      return result;
    }
    const std::uint32_t vertices = source[cluster_offset + 10];
    vertex_count += vertices;
    maximum_cluster_vertices =
        std::max(maximum_cluster_vertices, vertices);
  }
  if (vertex_count > std::numeric_limits<std::uint32_t>::max()) {
    result.error = "serialized retail collision vertex count is invalid";
    return result;
  }

  auto read_vec3 = [&](std::size_t offset) {
    return Vec3{
        std::bit_cast<float>(ReadBeU32(source, offset)),
        std::bit_cast<float>(ReadBeU32(source, offset + 4)),
        std::bit_cast<float>(ReadBeU32(source, offset + 8))};
  };
  result.mesh.bounds_min = read_vec3(0);
  result.mesh.bounds_max = read_vec3(16);
  if (!IsFinite(result.mesh.bounds_min) ||
      !IsFinite(result.mesh.bounds_max)) {
    result.error = "serialized retail collision bounds are invalid";
    return result;
  }
  result.mesh.bytes.assign(source.begin(), source.end());
  result.mesh.triangle_count = ReadBeU32(source, 40);
  result.mesh.vertex_count = static_cast<std::uint32_t>(vertex_count);
  result.mesh.cluster_count = cluster_count;
  result.mesh.maximum_cluster_vertex_count = maximum_cluster_vertices;
  result.ok = true;
  return result;
}

bool FixupRwCollisionMeshForGuest(std::span<std::uint8_t> bytes,
                                  std::uint32_t guest_address) {
  if (bytes.size() < kMeshHeaderSize || guest_address == 0 ||
      (guest_address & 0x0fu) != 0) {
    return false;
  }

  const std::uint32_t kd_offset = ReadBeU32(bytes, 48);
  const std::uint32_t cluster_table_offset = ReadBeU32(bytes, 52);
  const std::uint32_t cluster_count = ReadBeU32(bytes, 64);
  if ((kd_offset & 0x0fu) != 0 ||
      kd_offset > bytes.size() - kKdHeaderSize ||
      (cluster_table_offset & 0x0fu) != 0 ||
      cluster_table_offset > bytes.size() ||
      cluster_count >
          (bytes.size() - cluster_table_offset) / sizeof(std::uint32_t)) {
    return false;
  }

  const std::uint32_t branch_offset = ReadBeU32(bytes, kd_offset);
  const std::uint32_t branch_count = ReadBeU32(bytes, kd_offset + 4);
  // Retail branchless trees retain an unused serialized pointer value in
  // the branch-record field. Native traversal never dereferences that field
  // when branch_count is zero, so preserve it exactly instead of requiring
  // a value that only our rebuilt meshes initialize to zero.
  if (branch_count != 0 &&
      ((branch_offset & 0x0fu) != 0 ||
       branch_offset > bytes.size() ||
       branch_count >
           (bytes.size() - branch_offset) / 32u)) {
    return false;
  }

  for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
    const std::uint32_t cluster_offset =
        ReadBeU32(bytes, cluster_table_offset +
                            cluster * sizeof(std::uint32_t));
    if (cluster_offset >= bytes.size()) {
      return false;
    }
  }

  const std::uint64_t kd_address =
      static_cast<std::uint64_t>(guest_address) + kd_offset;
  const std::uint64_t cluster_table_address =
      static_cast<std::uint64_t>(guest_address) + cluster_table_offset;
  const std::uint64_t branch_address =
      static_cast<std::uint64_t>(guest_address) + branch_offset;
  if (kd_address > std::numeric_limits<std::uint32_t>::max() ||
      cluster_table_address >
          std::numeric_limits<std::uint32_t>::max() ||
      (branch_count != 0 &&
       branch_address > std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }

  WriteBeU32(bytes, 48, static_cast<std::uint32_t>(kd_address));
  WriteBeU32(bytes, 52,
             static_cast<std::uint32_t>(cluster_table_address));
  if (branch_count != 0) {
    WriteBeU32(bytes, kd_offset,
               static_cast<std::uint32_t>(branch_address));
  }
  return true;
}

}  // namespace skate::world
