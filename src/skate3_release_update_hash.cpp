#include "skate3_release_update_hash.h"

#include <cstddef>
#include <fstream>
#include <vector>

#include "../third_party/rexglue-sdk/thirdparty/crypto/sha256.h"

namespace skate3::release_update {

std::string Sha256OfFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  sha256::SHA256 hasher;
  constexpr std::size_t kBufferBytes = 1024 * 1024;
  std::vector<char> buffer(kBufferBytes);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hasher.add(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  return input.eof() ? hasher.getHash() : std::string{};
}

} // namespace skate3::release_update
