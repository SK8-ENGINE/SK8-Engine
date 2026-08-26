#include "skate/world/map_editor.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace skate::world {
namespace {

constexpr float kEpsilon = 1.0e-6f;

bool RayTriangle(const EditorRay& ray, Vec3 a, Vec3 b, Vec3 c,
                 float& distance, Vec3& normal) {
  const Vec3 edge1 = b - a;
  const Vec3 edge2 = c - a;
  const Vec3 p = Cross(ray.direction, edge2);
  const float determinant = Dot(edge1, p);
  if (std::abs(determinant) <= kEpsilon) {
    return false;
  }
  const float inverse = 1.0f / determinant;
  const Vec3 t = ray.origin - a;
  const float u = Dot(t, p) * inverse;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }
  const Vec3 q = Cross(t, edge1);
  const float v = Dot(ray.direction, q) * inverse;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }
  distance = Dot(edge2, q) * inverse;
  if (distance < 0.0f) {
    return false;
  }
  normal = Normalize(Cross(edge1, edge2));
  if (Dot(normal, ray.direction) > 0.0f) {
    normal = normal * -1.0f;
  }
  return true;
}

bool RayBounds(const EditorRay& ray, Vec3 minimum, Vec3 maximum,
               float maximum_distance) {
  float entry = 0.0f;
  float exit = maximum_distance;
  const float origins[3] = {
      ray.origin.x, ray.origin.y, ray.origin.z};
  const float directions[3] = {
      ray.direction.x, ray.direction.y, ray.direction.z};
  const float minima[3] = {minimum.x, minimum.y, minimum.z};
  const float maxima[3] = {maximum.x, maximum.y, maximum.z};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (std::abs(directions[axis]) <= kEpsilon) {
      if (origins[axis] < minima[axis] ||
          origins[axis] > maxima[axis]) {
        return false;
      }
      continue;
    }
    const float inverse = 1.0f / directions[axis];
    float first = (minima[axis] - origins[axis]) * inverse;
    float second = (maxima[axis] - origins[axis]) * inverse;
    if (first > second) {
      std::swap(first, second);
    }
    entry = std::max(entry, first);
    exit = std::min(exit, second);
    if (entry > exit) {
      return false;
    }
  }
  return exit >= 0.0f;
}

}  // namespace

EditorRay BuildEditorCameraRay(const float view[16],
                               const float projection[16],
                               const float camera_position[3],
                               float ndc_x, float ndc_y) {
  if (view == nullptr || projection == nullptr ||
      camera_position == nullptr ||
      !std::isfinite(ndc_x) || !std::isfinite(ndc_y) ||
      std::abs(projection[0]) <= kEpsilon ||
      std::abs(projection[5]) <= kEpsilon) {
    throw std::invalid_argument("editor camera ray inputs are invalid");
  }
  const Vec3 right{view[0], view[4], view[8]};
  const Vec3 up{view[1], view[5], view[9]};
  const Vec3 forward{view[2], view[6], view[10]};
  const Vec3 direction =
      right * (ndc_x / projection[0]) +
      up * (ndc_y / projection[5]) + forward;
  if (LengthSquared(direction) <= kEpsilon) {
    throw std::invalid_argument("editor camera ray is degenerate");
  }
  return {
      {camera_position[0], camera_position[1], camera_position[2]},
      Normalize(direction),
  };
}

MapObjectHit PickMapObject(
    const MapDefinition& map,
    std::span<const EditorObjectTransform> object_transforms,
    const EditorRay& ray,
    float maximum_distance) {
  MapObjectHit result;
  if (object_transforms.size() != map.editable_objects.size() ||
      !std::isfinite(maximum_distance) || maximum_distance < 0.0f ||
      LengthSquared(ray.direction) <= kEpsilon) {
    return result;
  }
  EditorRay normalized = ray;
  normalized.direction = Normalize(normalized.direction);
  for (std::size_t object_index = 0;
       object_index < map.editable_objects.size(); ++object_index) {
    const MapObject& object = map.editable_objects[object_index];
    const EditorObjectTransform& transform =
        object_transforms[object_index];
    const Vec3 offset = normalized.origin - transform.translation;
    EditorRay local_ray{
        {
            Dot(offset, transform.x_axis),
            Dot(offset, transform.y_axis),
            Dot(offset, transform.z_axis),
        },
        {
            Dot(normalized.direction, transform.x_axis),
            Dot(normalized.direction, transform.y_axis),
            Dot(normalized.direction, transform.z_axis),
        },
    };
    local_ray.direction = Normalize(local_ray.direction);
    if (!RayBounds(
            local_ray, object.local_bounds_min,
            object.local_bounds_max,
            std::min(maximum_distance, result.distance))) {
      continue;
    }
    for (std::size_t index = 0;
         index + 2 < object.render_mesh.indices.size(); index += 3) {
      const Vec3 a =
          object.render_mesh
              .vertices[object.render_mesh.indices[index]]
              .position;
      const Vec3 b =
          object.render_mesh
              .vertices[object.render_mesh.indices[index + 1]]
              .position;
      const Vec3 c =
          object.render_mesh
              .vertices[object.render_mesh.indices[index + 2]]
              .position;
      float distance = 0.0f;
      Vec3 local_normal;
      if (!RayTriangle(local_ray, a, b, c, distance, local_normal) ||
          distance > maximum_distance || distance >= result.distance) {
        continue;
      }
      result.hit = true;
      result.object_index = object_index;
      result.distance = distance;
      result.point =
          normalized.origin + normalized.direction * distance;
      result.normal = Normalize(
          transform.x_axis * local_normal.x +
          transform.y_axis * local_normal.y +
          transform.z_axis * local_normal.z);
    }
  }
  return result;
}

bool IntersectEditorDragPlane(const EditorRay& ray,
                              Vec3 plane_point,
                              Vec3 plane_normal,
                              Vec3& intersection) {
  plane_normal = Normalize(plane_normal);
  const float denominator = Dot(ray.direction, plane_normal);
  if (LengthSquared(plane_normal) <= kEpsilon ||
      std::abs(denominator) <= kEpsilon) {
    return false;
  }
  const float distance =
      Dot(plane_point - ray.origin, plane_normal) / denominator;
  if (!std::isfinite(distance) || distance < 0.0f) {
    return false;
  }
  intersection = ray.origin + ray.direction * distance;
  return true;
}

Vec3 RotateEditorVector(Vec3 value, Vec3 axis, float radians) {
  axis = Normalize(axis);
  if (LengthSquared(axis) <= kEpsilon || !std::isfinite(radians)) {
    return value;
  }
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  return value * cosine + Cross(axis, value) * sine +
         axis * (Dot(axis, value) * (1.0f - cosine));
}

Vec3 TransformEditorPoint(const EditorObjectTransform& transform,
                          Vec3 local_point) {
  return transform.translation +
         transform.x_axis * local_point.x +
         transform.y_axis * local_point.y +
         transform.z_axis * local_point.z;
}

CollisionTriangle TransformEditorCollisionTriangle(
    const EditorObjectTransform& transform,
    CollisionTriangle triangle) {
  triangle.a = TransformEditorPoint(transform, triangle.a);
  triangle.b = TransformEditorPoint(transform, triangle.b);
  triangle.c = TransformEditorPoint(transform, triangle.c);
  triangle.normal = Normalize(
      transform.x_axis * triangle.normal.x +
      transform.y_axis * triangle.normal.y +
      transform.z_axis * triangle.normal.z);
  return triangle;
}

std::vector<GrindRail> TransformEditorGrindRails(
    const MapDefinition& map,
    std::span<const EditorObjectTransform> object_transforms) {
  if (object_transforms.size() != map.editable_objects.size()) {
    throw std::invalid_argument(
        "editor grind transforms do not match object count");
  }
  std::vector<GrindRail> rails = map.grind_rails;
  std::vector<bool> dynamic_owned_rails(rails.size(), false);
  for (std::size_t object_index = 0;
       object_index < map.editable_objects.size(); ++object_index) {
    const MapObject& object = map.editable_objects[object_index];
    const EditorObjectTransform& transform =
        object_transforms[object_index];
    for (std::uint32_t rail_index : object.grind_rail_indices) {
      if (rail_index >= rails.size()) {
        throw std::invalid_argument(
            "editor object references an invalid grind rail");
      }
      if (object.physics.type == ObjectPhysicsType::Dynamic) {
        dynamic_owned_rails[rail_index] = true;
        continue;
      }
      for (Vec3& point : rails[rail_index].points) {
        point = TransformEditorPoint(transform, point - object.origin);
      }
      for (NativeGrindSegment& segment :
           rails[rail_index].native_segments) {
        const auto read = [&segment](std::size_t word) {
          return Vec3{
              std::bit_cast<float>(segment.words[word]),
              std::bit_cast<float>(segment.words[word + 1]),
              std::bit_cast<float>(segment.words[word + 2])};
        };
        const auto write =
            [&segment](std::size_t word, Vec3 value) {
              segment.words[word] =
                  std::bit_cast<std::uint32_t>(value.x);
              segment.words[word + 1] =
                  std::bit_cast<std::uint32_t>(value.y);
              segment.words[word + 2] =
                  std::bit_cast<std::uint32_t>(value.z);
            };
        const auto rotate = [&transform](Vec3 value) {
          return transform.x_axis * value.x +
                 transform.y_axis * value.y +
                 transform.z_axis * value.z;
        };
        const Vec3 a = rotate(read(0));
        const Vec3 b = rotate(read(4));
        const Vec3 c = rotate(read(8));
        const Vec3 d = TransformEditorPoint(
            transform, read(12) - object.origin);
        write(0, a);
        write(4, b);
        write(8, c);
        write(12, d);

        Vec3 minimum = d;
        Vec3 maximum = d;
        const auto include = [&minimum, &maximum](Vec3 value) {
          minimum.x = std::min(minimum.x, value.x);
          minimum.y = std::min(minimum.y, value.y);
          minimum.z = std::min(minimum.z, value.z);
          maximum.x = std::max(maximum.x, value.x);
          maximum.y = std::max(maximum.y, value.y);
          maximum.z = std::max(maximum.z, value.z);
        };
        const auto position = [a, b, c, d](float t) {
          return d + c * t + b * (t * t) +
                 a * (t * t * t);
        };
        include(position(1.0f));
        for (std::size_t axis = 0; axis < 3; ++axis) {
          const auto component = [axis](Vec3 value) {
            return axis == 0 ? value.x
                             : (axis == 1 ? value.y : value.z);
          };
          const float qa = 3.0f * component(a);
          const float qb = 2.0f * component(b);
          const float qc = component(c);
          if (std::abs(qa) <= 1.0e-8f) {
            if (std::abs(qb) > 1.0e-8f) {
              const float t = -qc / qb;
              if (t > 0.0f && t < 1.0f) {
                include(position(t));
              }
            }
            continue;
          }
          const float discriminant = qb * qb - 4.0f * qa * qc;
          if (discriminant < 0.0f) {
            continue;
          }
          const float root = std::sqrt(discriminant);
          for (const float t :
               {(-qb - root) / (2.0f * qa),
                (-qb + root) / (2.0f * qa)}) {
            if (t > 0.0f && t < 1.0f) {
              include(position(t));
            }
          }
        }
        write(20, minimum);
        write(24, maximum);
      }
    }
  }
  std::size_t source_index = 0;
  std::erase_if(
      rails,
      [&dynamic_owned_rails, &source_index](const GrindRail&) {
        return dynamic_owned_rails[source_index++];
      });
  return rails;
}

bool EditorAxisDragParameter(const EditorRay& source_ray, Vec3 pivot,
                             Vec3 axis, float& parameter) {
  EditorRay ray = source_ray;
  ray.direction = Normalize(ray.direction);
  axis = Normalize(axis);
  const float cross_dot = Dot(ray.direction, axis);
  const float denominator = 1.0f - cross_dot * cross_dot;
  if (LengthSquared(ray.direction) <= kEpsilon ||
      LengthSquared(axis) <= kEpsilon ||
      std::abs(denominator) <= 1.0e-4f) {
    return false;
  }
  const Vec3 offset = ray.origin - pivot;
  parameter =
      (Dot(offset, axis) -
       Dot(offset, ray.direction) * cross_dot) /
      denominator;
  return std::isfinite(parameter);
}

bool EditorRotationDragVector(const EditorRay& ray, Vec3 pivot, Vec3 axis,
                              Vec3& direction) {
  Vec3 point;
  if (!IntersectEditorDragPlane(ray, pivot, axis, point)) {
    return false;
  }
  direction = Normalize(point - pivot);
  return LengthSquared(direction) > kEpsilon;
}

float EditorSignedRotation(Vec3 from, Vec3 to, Vec3 axis) {
  from = Normalize(from);
  to = Normalize(to);
  axis = Normalize(axis);
  if (LengthSquared(from) <= kEpsilon ||
      LengthSquared(to) <= kEpsilon ||
      LengthSquared(axis) <= kEpsilon) {
    return 0.0f;
  }
  return std::atan2(Dot(axis, Cross(from, to)),
                    std::clamp(Dot(from, to), -1.0f, 1.0f));
}

EditorGizmoHit PickEditorGizmo(const EditorRay& source_ray, Vec3 pivot,
                               float scale) {
  EditorGizmoHit result;
  if (!std::isfinite(scale) || scale <= kEpsilon) {
    return result;
  }
  const EditorRay ray{source_ray.origin, Normalize(source_ray.direction)};
  if (LengthSquared(ray.direction) <= kEpsilon) {
    return result;
  }
  constexpr Vec3 axes[3] = {
      {1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
  };
  constexpr EditorGizmoHandle translation_handles[3] = {
      EditorGizmoHandle::TranslateX,
      EditorGizmoHandle::TranslateY,
      EditorGizmoHandle::TranslateZ,
  };
  constexpr EditorGizmoHandle rotation_handles[3] = {
      EditorGizmoHandle::RotateX,
      EditorGizmoHandle::RotateY,
      EditorGizmoHandle::RotateZ,
  };

  // Arrow shafts occupy 0.18..1.25 gizmo units. The closest-points test is
  // stable at any camera distance because both length and hit radius scale.
  for (int index = 0; index < 3; ++index) {
    const Vec3 axis = axes[index];
    const float parallel = Dot(ray.direction, axis);
    const float denominator = 1.0f - parallel * parallel;
    if (std::abs(denominator) <= 1.0e-4f) {
      continue;
    }
    const Vec3 offset = ray.origin - pivot;
    const float axis_parameter =
        (Dot(offset, axis) -
         Dot(offset, ray.direction) * parallel) /
        denominator;
    const float ray_parameter =
        (Dot(pivot - ray.origin, ray.direction) +
         axis_parameter * parallel);
    if (ray_parameter < 0.0f ||
        axis_parameter < scale * 0.18f ||
        axis_parameter > scale * 1.25f) {
      continue;
    }
    const Vec3 on_ray = ray.origin + ray.direction * ray_parameter;
    const Vec3 on_axis = pivot + axis * axis_parameter;
    if (Length(on_ray - on_axis) <= scale * 0.095f &&
        ray_parameter < result.distance) {
      result.handle = translation_handles[index];
      result.distance = ray_parameter;
    }
  }

  // Rotation rings sit inside the arrow tips. Use a narrow radial band and
  // retain the nearest world hit when rings overlap in screen space.
  for (int index = 0; index < 3; ++index) {
    Vec3 point;
    if (!IntersectEditorDragPlane(ray, pivot, axes[index], point)) {
      continue;
    }
    const float radius = Length(point - pivot);
    const float error = std::abs(radius - scale * 0.78f);
    const float distance = Length(point - ray.origin);
    if (error <= scale * 0.075f && distance < result.distance) {
      result.handle = rotation_handles[index];
      result.distance = distance;
    }
  }
  return result;
}

}  // namespace skate::world
