#pragma once

#include <cstdint>
#include <vector>

namespace skate3::mechanics_sandbox::map {

// Matches the native scene renderer's decoded static vertex layout. The
// Blender export keeps this deliberately boring: positions, two UV sets,
// packed skin bytes, a normal, and a third UV set.
struct VisualVertex {
  float position[3];
  float uv[2];
  float uv2[2];
  uint8_t blend_weight[4];
  uint8_t blend_index[4];
  float normal[3];
  float uv3[2];
};

struct VisualMesh {
  std::vector<VisualVertex> vertices;
  std::vector<uint16_t> indices;
};

struct CollisionBox {
  uint32_t id;
  float min[3];
  float max[3];
};

struct Contact {
  uint32_t id = 0;
  float point[3] = {};
  float normal[3] = {};
  float penetration = 0.0f;
};

// The fallback is the same small graybox authored by
// research/sandbox/blender/skate3_test_map.py. It remains available when the
// local Blender export is not present, so a runtime asset is never required
// for the default-off build to start.
const VisualMesh& TestVisualMesh();
const std::vector<CollisionBox>& TestCollisionBoxes();

bool QueryContact(const float position[3], float radius, Contact& out);

}  // namespace skate3::mechanics_sandbox::map
