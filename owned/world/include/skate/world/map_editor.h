#pragma once

#include "skate/world/world_map.h"

#include <cstddef>
#include <limits>
#include <span>

namespace skate::world {

struct EditorRay {
  Vec3 origin;
  Vec3 direction;
};

struct EditorObjectTransform {
  Vec3 translation;
  Vec3 x_axis{1.0f, 0.0f, 0.0f};
  Vec3 y_axis{0.0f, 1.0f, 0.0f};
  Vec3 z_axis{0.0f, 0.0f, 1.0f};
};

struct MapObjectHit {
  bool hit = false;
  std::size_t object_index = 0;
  float distance = std::numeric_limits<float>::infinity();
  Vec3 point;
  Vec3 normal;
};

enum class EditorGizmoHandle {
  None,
  TranslateX,
  TranslateY,
  TranslateZ,
  RotateX,
  RotateY,
  RotateZ,
};

struct EditorGizmoHit {
  EditorGizmoHandle handle = EditorGizmoHandle::None;
  float distance = std::numeric_limits<float>::infinity();
};

// Builds a world-space ray from the row-vector camera convention used by the
// Skate 3 native scene. ndc_x/ndc_y are in [-1, 1], with +Y upward.
EditorRay BuildEditorCameraRay(const float view[16],
                               const float projection[16],
                               const float camera_position[3],
                               float ndc_x, float ndc_y);

// Tests actual object render triangles after applying each authoritative pose.
MapObjectHit PickMapObject(
    const MapDefinition& map,
    std::span<const EditorObjectTransform> object_transforms,
    const EditorRay& ray,
    float maximum_distance = 10000.0f);

bool IntersectEditorDragPlane(const EditorRay& ray,
                              Vec3 plane_point,
                              Vec3 plane_normal,
                              Vec3& intersection);

Vec3 RotateEditorVector(Vec3 value, Vec3 axis, float radians);
Vec3 TransformEditorPoint(const EditorObjectTransform& transform,
                          Vec3 local_point);
CollisionTriangle TransformEditorCollisionTriangle(
    const EditorObjectTransform& transform,
    CollisionTriangle triangle);

// Rebuilds authored grind paths from the same per-object poses used by
// rendering and collision. Rails not associated with an editable object are
// preserved unchanged.
std::vector<GrindRail> TransformEditorGrindRails(
    const MapDefinition& map,
    std::span<const EditorObjectTransform> object_transforms);

EditorGizmoHit PickEditorGizmo(const EditorRay& ray, Vec3 pivot,
                               float scale);

bool EditorAxisDragParameter(const EditorRay& ray, Vec3 pivot, Vec3 axis,
                             float& parameter);

bool EditorRotationDragVector(const EditorRay& ray, Vec3 pivot, Vec3 axis,
                              Vec3& direction);

float EditorSignedRotation(Vec3 from, Vec3 to, Vec3 axis);

}  // namespace skate::world
