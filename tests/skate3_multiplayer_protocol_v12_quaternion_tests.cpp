#include "skate3_multiplayer_protocol_v12_quaternion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using namespace skate3::multiplayer::protocol_v12;
int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void TestRandomErrorBound() {
  std::uint32_t random = 0x91E10DA5u;
  double maximum_error_degrees = 0.0;
  for (std::size_t iteration = 0; iteration < 100000; ++iteration) {
    std::array<float, 4> source{};
    double length_squared = 0.0;
    for (float& component : source) {
      random = random * 1664525u + 1013904223u;
      component =
          static_cast<float>(
              static_cast<std::int32_t>(random)) /
          2147483648.0f;
      length_squared +=
          static_cast<double>(component) * component;
    }
    const float inverse_length =
        static_cast<float>(1.0 / std::sqrt(length_squared));
    for (float& component : source) {
      component *= inverse_length;
    }
    SmallestThreeQuaternion encoded;
    std::array<float, 4> decoded{};
    Expect(EncodeSmallestThreeQuaternion(source, encoded),
           "random quaternion did not encode");
    Expect(DecodeSmallestThreeQuaternion(encoded, decoded),
           "random quaternion did not decode");
    double dot = 0.0;
    double source_length = 0.0;
    double decoded_length = 0.0;
    for (std::size_t index = 0; index < 4; ++index) {
      dot += static_cast<double>(source[index]) * decoded[index];
      source_length +=
          static_cast<double>(source[index]) * source[index];
      decoded_length +=
          static_cast<double>(decoded[index]) * decoded[index];
    }
    dot /= std::sqrt(source_length * decoded_length);
    dot = std::clamp(std::fabs(dot), 0.0, 1.0);
    const double error_degrees =
        2.0 * std::acos(dot) * 57.29577951308232;
    maximum_error_degrees =
        std::max(maximum_error_degrees, error_degrees);
  }
  if (maximum_error_degrees >= 0.02) {
    std::cerr << "maximum_error_degrees="
              << maximum_error_degrees << '\n';
  }
  Expect(maximum_error_degrees < 0.02,
         "smallest-three angular error exceeded 0.02 degrees");
}

void TestValidation() {
  SmallestThreeQuaternion encoded;
  Expect(!EncodeSmallestThreeQuaternion(
             {0.0f, 0.0f, 0.0f, 0.0f}, encoded),
         "zero quaternion encoded");
  std::array<float, 4> decoded{};
  encoded.omitted_component = 4;
  Expect(!DecodeSmallestThreeQuaternion(encoded, decoded),
         "invalid omitted component decoded");
}

float RandomSigned(std::uint32_t& random) {
  random = random * 1664525u + 1013904223u;
  return static_cast<float>(static_cast<std::int32_t>(random)) /
         2147483648.0f;
}

std::array<float, 4> RandomQuaternion(std::uint32_t& random) {
  std::array<float, 4> quaternion{};
  double length_squared = 0.0;
  for (float& component : quaternion) {
    component = RandomSigned(random);
    length_squared += static_cast<double>(component) * component;
  }
  const float inverse_length =
      static_cast<float>(1.0 / std::sqrt(length_squared));
  for (float& component : quaternion) {
    component *= inverse_length;
  }
  return quaternion;
}

std::array<float, 3> RotatePoint(const std::array<float, 4>& quaternion,
                                 const std::array<float, 3>& point) {
  const std::array<float, 3> vector = {
      quaternion[0],
      quaternion[1],
      quaternion[2],
  };
  const std::array<float, 3> cross = {
      vector[1] * point[2] - vector[2] * point[1],
      vector[2] * point[0] - vector[0] * point[2],
      vector[0] * point[1] - vector[1] * point[0],
  };
  const std::array<float, 3> second_cross = {
      vector[1] * cross[2] - vector[2] * cross[1],
      vector[2] * cross[0] - vector[0] * cross[2],
      vector[0] * cross[1] - vector[1] * cross[0],
  };
  return {
      point[0] + 2.0f * (quaternion[3] * cross[0] + second_cross[0]),
      point[1] + 2.0f * (quaternion[3] * cross[1] + second_cross[1]),
      point[2] + 2.0f * (quaternion[3] * cross[2] + second_cross[2]),
  };
}

double Distance(const std::array<float, 3>& first,
                const std::array<float, 3>& second) {
  double squared = 0.0;
  for (std::size_t component = 0; component < 3; ++component) {
    const double difference =
        static_cast<double>(first[component]) - second[component];
    squared += difference * difference;
  }
  return std::sqrt(squared);
}

void TestSyntheticSkinnedVertexErrorBound() {
  // This is an offline mathematical deformation proof, not a retail-mesh
  // visual test. Metres are used so the acceptance bounds are legible.
  constexpr std::size_t kBones = 96;
  constexpr std::size_t kVerticesPerFrame = 1024;
  constexpr std::size_t kFrames = 250;
  std::uint32_t random = 0xD41C6A77u;
  double maximum_vertex_error_metres = 0.0;
  double total_vertex_error_metres = 0.0;
  std::size_t sampled_vertices = 0;
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    std::array<std::array<float, 4>, kBones> source_rotations{};
    std::array<std::array<float, 4>, kBones> decoded_rotations{};
    std::array<std::array<float, 3>, kBones> translations{};
    for (std::size_t bone = 0; bone < kBones; ++bone) {
      source_rotations[bone] = RandomQuaternion(random);
      SmallestThreeQuaternion encoded;
      Expect(EncodeSmallestThreeQuaternion(source_rotations[bone], encoded),
             "synthetic bone rotation did not encode");
      Expect(DecodeSmallestThreeQuaternion(encoded, decoded_rotations[bone]),
             "synthetic bone rotation did not decode");
      translations[bone] = {
          RandomSigned(random) * 1.5f,
          RandomSigned(random) * 1.5f,
          RandomSigned(random) * 1.5f,
      };
    }
    for (std::size_t vertex = 0; vertex < kVerticesPerFrame; ++vertex) {
      const std::size_t influence_count = 1 + (random % 4);
      std::array<float, 4> weights{};
      float weight_sum = 0.0f;
      for (std::size_t influence = 0; influence < influence_count;
           ++influence) {
        weights[influence] = 0.05f + std::fabs(RandomSigned(random));
        weight_sum += weights[influence];
      }
      std::array<float, 3> source_position{};
      std::array<float, 3> decoded_position{};
      for (std::size_t influence = 0; influence < influence_count;
           ++influence) {
        const std::size_t bone = random % kBones;
        const float weight = weights[influence] / weight_sum;
        const std::array<float, 3> local_point = {
            RandomSigned(random) * 0.75f,
            RandomSigned(random) * 0.75f,
            RandomSigned(random) * 0.75f,
        };
        const auto source = RotatePoint(source_rotations[bone], local_point);
        const auto decoded = RotatePoint(decoded_rotations[bone], local_point);
        for (std::size_t component = 0; component < 3; ++component) {
          source_position[component] +=
              weight * (source[component] + translations[bone][component]);
          decoded_position[component] +=
              weight * (decoded[component] + translations[bone][component]);
        }
      }
      const double error = Distance(source_position, decoded_position);
      maximum_vertex_error_metres =
          std::max(maximum_vertex_error_metres, error);
      total_vertex_error_metres += error;
      ++sampled_vertices;
    }
  }
  std::cout << std::fixed << std::setprecision(6)
            << "synthetic_skinning samples=" << sampled_vertices
            << " mean_error_mm="
            << total_vertex_error_metres * 1000.0 /
                   static_cast<double>(sampled_vertices)
            << " max_error_mm=" << maximum_vertex_error_metres * 1000.0
            << '\n';
  Expect(maximum_vertex_error_metres < 0.001,
         "smallest-three moved a synthetic skinned vertex by 1 mm or more");
}

void TestBoardContactAndCanonicalSign() {
  std::uint32_t random = 0x7B51F009u;
  double maximum_probe_error_metres = 0.0;
  for (std::size_t iteration = 0; iteration < 100000; ++iteration) {
    const std::array<float, 4> source = RandomQuaternion(random);
    SmallestThreeQuaternion positive;
    SmallestThreeQuaternion negative;
    const std::array<float, 4> sign_flipped = {
        -source[0], -source[1], -source[2], -source[3]};
    Expect(EncodeSmallestThreeQuaternion(source, positive),
           "board probe source did not encode");
    Expect(EncodeSmallestThreeQuaternion(sign_flipped, negative),
           "board probe sign-equivalent source did not encode");
    Expect(positive.omitted_component == negative.omitted_component &&
               positive.components == negative.components,
           "equivalent quaternion signs did not canonicalize identically");
    std::array<float, 4> decoded{};
    Expect(DecodeSmallestThreeQuaternion(positive, decoded),
           "board probe did not decode");
    for (const std::array<float, 3>& probe :
         {std::array<float, 3>{0.42f, 0.0f, 0.13f},
          std::array<float, 3>{-0.42f, 0.0f, 0.13f},
          std::array<float, 3>{0.0f, 0.0f, -0.08f}}) {
      maximum_probe_error_metres =
          std::max(maximum_probe_error_metres,
                   Distance(RotatePoint(source, probe),
                            RotatePoint(decoded, probe)));
    }
  }
  std::cout << std::fixed << std::setprecision(6)
            << "synthetic_board_probe max_error_mm="
            << maximum_probe_error_metres * 1000.0 << '\n';
  Expect(maximum_probe_error_metres < 0.0005,
         "smallest-three moved a synthetic board/contact probe by 0.5 mm");
}

}  // namespace

int main() {
  TestRandomErrorBound();
  TestValidation();
  TestSyntheticSkinnedVertexErrorBound();
  TestBoardContactAndCanonicalSign();
  return failures == 0 ? 0 : 1;
}
