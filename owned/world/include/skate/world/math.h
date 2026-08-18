#pragma once

#include <cmath>

namespace skate::world {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

constexpr Vec3 operator+(const Vec3& left, const Vec3& right) {
  return {left.x + right.x, left.y + right.y, left.z + right.z};
}

constexpr Vec3 operator-(const Vec3& left, const Vec3& right) {
  return {left.x - right.x, left.y - right.y, left.z - right.z};
}

constexpr Vec3 operator*(const Vec3& value, float scale) {
  return {value.x * scale, value.y * scale, value.z * scale};
}

constexpr Vec3 operator/(const Vec3& value, float scale) {
  return {value.x / scale, value.y / scale, value.z / scale};
}

constexpr float Dot(const Vec3& left, const Vec3& right) {
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

constexpr Vec3 Cross(const Vec3& left, const Vec3& right) {
  return {
      left.y * right.z - left.z * right.y,
      left.z * right.x - left.x * right.z,
      left.x * right.y - left.y * right.x,
  };
}

constexpr float LengthSquared(const Vec3& value) {
  return Dot(value, value);
}

inline float Length(const Vec3& value) {
  return std::sqrt(LengthSquared(value));
}

inline Vec3 Normalize(const Vec3& value) {
  const float length = Length(value);
  if (length <= 1.0e-6f) {
    return {};
  }
  return value / length;
}

}  // namespace skate::world
