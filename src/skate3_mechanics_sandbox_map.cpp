#include "skate3_mechanics_sandbox_map.h"

#include <algorithm>
#include <cmath>

namespace skate3::mechanics_sandbox::map {
namespace {

void AddQuad(VisualMesh& mesh, const float a[3], const float b[3],
             const float c[3], const float d[3], const float normal[3]) {
  const uint16_t base = static_cast<uint16_t>(mesh.vertices.size());
  const float* positions[] = {a, b, c, d};
  const float uvs[][2] = {{0.0f, 0.0f}, {1.0f, 0.0f},
                          {1.0f, 1.0f}, {0.0f, 1.0f}};
  for (int i = 0; i < 4; ++i) {
    VisualVertex v{};
    std::copy_n(positions[i], 3, v.position);
    v.uv[0] = uvs[i][0];
    v.uv[1] = uvs[i][1];
    v.uv2[0] = uvs[i][0];
    v.uv2[1] = uvs[i][1];
    v.blend_weight[0] = 0;
    v.blend_index[0] = 0;
    std::copy_n(normal, 3, v.normal);
    v.uv3[0] = uvs[i][0];
    v.uv3[1] = uvs[i][1];
    mesh.vertices.push_back(v);
  }
  mesh.indices.insert(mesh.indices.end(), {uint16_t(base + 0), uint16_t(base + 1),
                                           uint16_t(base + 2), uint16_t(base + 0),
                                           uint16_t(base + 2), uint16_t(base + 3)});
}

void AddBox(VisualMesh& mesh, const float min[3], const float max[3]) {
  const float p000[] = {min[0], min[1], min[2]};
  const float p100[] = {max[0], min[1], min[2]};
  const float p110[] = {max[0], max[1], min[2]};
  const float p010[] = {min[0], max[1], min[2]};
  const float p001[] = {min[0], min[1], max[2]};
  const float p101[] = {max[0], min[1], max[2]};
  const float p111[] = {max[0], max[1], max[2]};
  const float p011[] = {min[0], max[1], max[2]};
  const float nx[] = {-1.0f, 0.0f, 0.0f};
  const float px[] = {1.0f, 0.0f, 0.0f};
  const float ny[] = {0.0f, -1.0f, 0.0f};
  const float py[] = {0.0f, 1.0f, 0.0f};
  const float nz[] = {0.0f, 0.0f, -1.0f};
  const float pz[] = {0.0f, 0.0f, 1.0f};
  AddQuad(mesh, p000, p001, p011, p010, nx);
  AddQuad(mesh, p100, p110, p111, p101, px);
  AddQuad(mesh, p000, p100, p101, p001, ny);
  AddQuad(mesh, p010, p011, p111, p110, py);
  AddQuad(mesh, p000, p010, p110, p100, nz);
  AddQuad(mesh, p001, p101, p111, p011, pz);
}

VisualMesh BuildMesh() {
  VisualMesh mesh;
  // Blender graybox dimensions, in meters: a broad deck, two ledges, and a
  // wedge ramp. The ground is slightly below the board-origin anchor.
  const float ground_min[] = {-20.0f, -0.18f, -20.0f};
  const float ground_max[] = {20.0f, -0.12f, 20.0f};
  AddBox(mesh, ground_min, ground_max);
  const float ledge_a_min[] = {-7.0f, -0.12f, -2.5f};
  const float ledge_a_max[] = {-2.0f, 0.55f, 2.5f};
  AddBox(mesh, ledge_a_min, ledge_a_max);
  const float ledge_b_min[] = {2.5f, -0.12f, -2.0f};
  const float ledge_b_max[] = {6.5f, 0.95f, 2.0f};
  AddBox(mesh, ledge_b_min, ledge_b_max);
  // A simple wedge, with a flat top at x=12 and a low edge at x=8.
  const float w0[] = {8.0f, -0.12f, -3.0f};
  const float w1[] = {12.0f, -0.12f, -3.0f};
  const float w2[] = {12.0f, 1.65f, -3.0f};
  const float w3[] = {8.0f, -0.12f, 3.0f};
  const float w4[] = {12.0f, -0.12f, 3.0f};
  const float w5[] = {12.0f, 1.65f, 3.0f};
  const float wn[] = {0.0f, 0.0f, -1.0f};
  const float wp[] = {0.0f, 0.0f, 1.0f};
  const float px[] = {1.0f, 0.0f, 0.0f};
  const float ny[] = {0.0f, -1.0f, 0.0f};
  const float wr[] = {-0.4f, 0.9f, 0.0f};
  AddQuad(mesh, w0, w1, w2, w0, wn);  // degenerate cap is harmless and omitted below
  mesh.indices.resize(mesh.indices.size() - 6);
  AddQuad(mesh, w3, w5, w4, w3, wp);
  mesh.indices.resize(mesh.indices.size() - 6);
  // Sloping top, two end caps, and underside.
  AddQuad(mesh, w0, w3, w5, w2, wr);
  AddQuad(mesh, w1, w4, w5, w2, px);
  AddQuad(mesh, w0, w1, w4, w3, ny);
  return mesh;
}

const std::vector<CollisionBox> kBoxes = {
    {1, {-20.0f, -0.18f, -20.0f}, {20.0f, -0.12f, 20.0f}},
    {2, {-7.0f, -0.12f, -2.5f}, {-2.0f, 0.55f, 2.5f}},
    {3, {2.5f, -0.12f, -2.0f}, {6.5f, 0.95f, 2.0f}},
    {4, {8.0f, -0.12f, -3.0f}, {12.0f, 1.65f, 3.0f}},
};

}  // namespace

const VisualMesh& TestVisualMesh() {
  static const VisualMesh mesh = BuildMesh();
  return mesh;
}

const std::vector<CollisionBox>& TestCollisionBoxes() { return kBoxes; }

bool QueryContact(const float position[3], float radius, Contact& out) {
  if (position == nullptr || radius < 0.0f) {
    return false;
  }
  float best_penetration = 0.0f;
  Contact best{};
  for (const CollisionBox& box : kBoxes) {
    const float qx = std::clamp(position[0], box.min[0], box.max[0]);
    const float qy = std::clamp(position[1], box.min[1], box.max[1]);
    const float qz = std::clamp(position[2], box.min[2], box.max[2]);
    const float dx = position[0] - qx;
    const float dy = position[1] - qy;
    const float dz = position[2] - qz;
    const float distance_sq = dx * dx + dy * dy + dz * dz;
    if (distance_sq > radius * radius) {
      continue;
    }
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float distance = std::sqrt(std::max(distance_sq, 0.0f));
    if (distance > 1e-5f) {
      nx = dx / distance;
      ny = dy / distance;
      nz = dz / distance;
    } else {
      const float bottom = position[1] - box.min[1];
      const float top = box.max[1] - position[1];
      if (bottom < top) {
        ny = -1.0f;
      }
    }
    const float penetration = radius - distance;
    if (penetration > best_penetration) {
      best_penetration = penetration;
      best.id = box.id;
      best.point[0] = qx;
      best.point[1] = qy;
      best.point[2] = qz;
      best.normal[0] = nx;
      best.normal[1] = ny;
      best.normal[2] = nz;
      best.penetration = penetration;
    }
  }
  if (best_penetration <= 0.0f) {
    return false;
  }
  out = best;
  return true;
}

}  // namespace skate3::mechanics_sandbox::map
