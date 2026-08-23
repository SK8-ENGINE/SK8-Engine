#include "skate3_multiplayer_protocol_v12_lossless.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace skate3::multiplayer::protocol_v12;
int failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void RoundTrip(const std::vector<std::uint8_t>& source) {
  std::vector<std::uint8_t> encoded;
  std::vector<std::uint8_t> decoded;
  Expect(EncodeLosslessBytes(source, encoded), "encode failed");
  Expect(LosslessEncodedByteCount(source) == encoded.size(),
         "encoded-size preflight disagreed with encoder");
  Expect(DecodeLosslessBytes(encoded, decoded), "decode failed");
  Expect(decoded == source, "round trip changed bytes");
}

void TestRoundTrips() {
  RoundTrip({1});
  RoundTrip(std::vector<std::uint8_t>(65536, 0));
  RoundTrip(std::vector<std::uint8_t>(1000, 0x7f));
  std::vector<std::uint8_t> mixed;
  for (std::uint32_t value = 0; value < 4096; ++value) {
    mixed.push_back(static_cast<std::uint8_t>(value * 73u));
    if ((value % 17) == 0) {
      mixed.insert(mixed.end(), 20, 0);
    }
  }
  RoundTrip(mixed);
  for (std::size_t length = 120; length <= 260; ++length) {
    std::vector<std::uint8_t> boundary(length);
    for (std::size_t index = 0; index < length; ++index) {
      boundary[index] = static_cast<std::uint8_t>(
          (index * 73 + index / 7) & 0xff);
    }
    RoundTrip(boundary);
  }
  std::uint32_t random = 0x12345678u;
  for (std::size_t iteration = 0; iteration < 1000; ++iteration) {
    random = random * 1664525u + 1013904223u;
    const std::size_t length = 1 + (random % 20000);
    std::vector<std::uint8_t> fuzz(length);
    for (std::uint8_t& byte : fuzz) {
      random = random * 1664525u + 1013904223u;
      byte = static_cast<std::uint8_t>(random >> 24);
    }
    RoundTrip(fuzz);
  }
}

void TestMalformed() {
  std::vector<std::uint8_t> decoded;
  Expect(!DecodeLosslessBytes({}, decoded), "empty packet decoded");
  Expect(!DecodeLosslessBytes(
             std::vector<std::uint8_t>{1, 0, 0, 0}, decoded),
         "missing token decoded");
  Expect(!DecodeLosslessBytes(
             std::vector<std::uint8_t>{3, 0, 0, 0, 2, 1},
             decoded),
         "truncated literal decoded");
  Expect(!DecodeLosslessBytes(
             std::vector<std::uint8_t>{3, 0, 0, 0, 0xC0},
             decoded),
         "missing repeat value decoded");
  Expect(!DecodeLosslessBytes(
             std::vector<std::uint8_t>{1, 0, 0, 0, 0x81},
             decoded),
         "oversized run decoded");
  Expect(!EncodeLosslessBytes({}, decoded), "empty input encoded");
}

void TestSelectionPolicy() {
  Expect(!LosslessPackingWorthwhile(1000, 999, 1136),
         "negligible same-fragment saving was selected");
  Expect(LosslessPackingWorthwhile(1000, 900, 1136),
         "ten-percent saving was not selected");
  Expect(LosslessPackingWorthwhile(1200, 1130, 1136),
         "one-fragment reduction was not selected");
  Expect(!LosslessPackingWorthwhile(1000, 1000, 1136),
         "non-saving encoding was selected");
}

}  // namespace

int main() {
  TestRoundTrips();
  TestMalformed();
  TestSelectionPolicy();
  return failures == 0 ? 0 : 1;
}
