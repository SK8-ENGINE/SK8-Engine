#include "skate3_multiplayer_protocol_v12_quaternion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

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

}  // namespace

int main() {
  TestRandomErrorBound();
  TestValidation();
  return failures == 0 ? 0 : 1;
}
