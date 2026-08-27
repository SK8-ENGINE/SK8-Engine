#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "skate3_release_update_hash.h"

namespace {

constexpr std::string_view kOneMiBZeroSha256 =
    "30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58";

class TemporaryFile {
public:
  TemporaryFile()
      : path_(std::filesystem::temp_directory_path() /
              "skate3-release-update-hash-test.bin") {}

  ~TemporaryFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

int main(int argc, char** argv) {
  if (argc == 3) {
    std::promise<std::string> result;
    auto future = result.get_future();
    std::thread worker([&result, path = std::filesystem::path(argv[1])] {
      result.set_value(skate3::release_update::Sha256OfFile(path));
    });
    worker.join();
    const std::string hash = future.get();
    if (hash != argv[2]) {
      std::cerr << "Unexpected SHA-256: " << hash << '\n';
      return 1;
    }
    std::cout << "RELEASE_UPDATE_ARCHIVE_HASH_OK " << hash << '\n';
    return 0;
  }
  if (argc != 1) {
    std::cerr << "Usage: skate3_release_update_hash_tests [file sha256]\n";
    return 1;
  }

  TemporaryFile file;
  {
    std::ofstream output(file.path(), std::ios::binary | std::ios::trunc);
    const std::vector<char> zeroes(1024 * 1024, '\0');
    output.write(zeroes.data(),
                 static_cast<std::streamsize>(zeroes.size()));
    if (!output) {
      std::cerr << "Could not create the hash regression fixture.\n";
      return 1;
    }
  }

  std::promise<std::string> result;
  auto future = result.get_future();
  std::thread worker([&result, &file] {
    result.set_value(skate3::release_update::Sha256OfFile(file.path()));
  });
  worker.join();

  const std::string hash = future.get();
  if (hash != kOneMiBZeroSha256) {
    std::cerr << "Unexpected SHA-256: " << hash << '\n';
    return 1;
  }
  std::cout << "RELEASE_UPDATE_HASH_OK " << hash << '\n';
  return 0;
}
