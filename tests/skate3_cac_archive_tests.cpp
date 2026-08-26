#include "skate3_cac_archive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

void WriteBe16(
    std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteBe32(
    std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

bool WriteSyntheticArchive(
    const std::filesystem::path& path, std::string_view directory,
    bool compressed = true, std::uint32_t chunk_method = 2) {
  constexpr std::size_t kNameOffset = 80;
  constexpr std::size_t kNameBytes = 96;
  constexpr std::size_t kDataOffset = 192;
  constexpr std::array<std::uint8_t, 16> kPayload = {
      0x89, 'R',  'W',  '4',  'x', 'b', '2', 0x00,
      0x0D, 0x0A, 0x1A, 0x0A, 1,   2,   3,   4};
  if (directory.size() >= 54) {
    return false;
  }
  std::vector<std::uint8_t> stored;
  if (compressed) {
    // One compressed stream inside one aligned chunkref chunk.
    stored.resize(48);
    constexpr std::string_view kChunkMagic = "chunkref";
    std::copy(
        kChunkMagic.begin(), kChunkMagic.end(), stored.begin());
    WriteBe32(stored, 8, 2);
    WriteBe32(stored, 12, kPayload.size());
    WriteBe32(stored, 16, 128 * 1024);
    WriteBe32(stored, 20, 1);
    WriteBe32(stored, 24, 16);
    if (chunk_method == 3) {
      std::vector<std::uint8_t> packed(
          static_cast<std::size_t>(compressBound(kPayload.size())));
      uLongf packed_size = static_cast<uLongf>(packed.size());
      if (compress2(
              reinterpret_cast<Bytef*>(packed.data()),
              &packed_size,
              reinterpret_cast<const Bytef*>(kPayload.data()),
              static_cast<uLong>(kPayload.size()),
              Z_BEST_COMPRESSION) != Z_OK) {
        return false;
      }
      packed.resize(static_cast<std::size_t>(packed_size));
      WriteBe32(
          stored, 40,
          static_cast<std::uint32_t>(packed.size()));
      WriteBe32(stored, 44, 3);
      stored.insert(stored.end(), packed.begin(), packed.end());
    } else if (chunk_method == 4) {
      WriteBe32(stored, 40, kPayload.size());
      WriteBe32(stored, 44, 4);
      stored.insert(
          stored.end(), kPayload.begin(), kPayload.end());
    } else {
      // Literal-only RefPack.
      WriteBe32(stored, 40, 5 + 1 + kPayload.size() + 1);
      WriteBe32(stored, 44, 2);
      stored.push_back(0x10);
      stored.push_back(0xFB);
      stored.push_back(0);
      stored.push_back(0);
      stored.push_back(
          static_cast<std::uint8_t>(kPayload.size()));
      stored.push_back(0xE3);
      stored.insert(stored.end(), kPayload.begin(), kPayload.end());
      stored.push_back(0xFC);
    }
  } else {
    stored.assign(kPayload.begin(), kPayload.end());
  }
  std::vector<std::uint8_t> bytes(kDataOffset + stored.size());
  bytes[0] = 'E';
  bytes[1] = 'B';
  WriteBe16(bytes, 2, 3);
  WriteBe32(bytes, 4, 1);
  WriteBe16(bytes, 8, 0x10);
  bytes[10] = 6;
  WriteBe32(bytes, 12, kNameOffset);
  WriteBe32(bytes, 16, kNameBytes);
  bytes[20] = 31;
  bytes[21] = 54;
  bytes[23] = 1;
  WriteBe32(bytes, 48, kDataOffset >> 6);
  WriteBe32(bytes, 52, stored.size());
  WriteBe32(bytes, 56, kPayload.size());
  bytes[64] = compressed ? 2 : 0;
  WriteBe16(bytes, kNameOffset, 0);
  constexpr std::string_view kFilename = "fixture.rx2";
  std::copy(kFilename.begin(), kFilename.end(),
            bytes.begin() + kNameOffset + 2);
  std::copy(directory.begin(), directory.end(),
            bytes.begin() + kNameOffset + 32);
  std::copy(stored.begin(), stored.end(), bytes.begin() + kDataOffset);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  return output
      .write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()))
      .good();
}

bool HasRx2Magic(
    const std::filesystem::path& path) {
  constexpr std::array<std::uint8_t, 12> kMagic = {
      0x89, 'R', 'W', '4', 'x', 'b', '2', 0x00, 0x0D, 0x0A, 0x1A, 0x0A};
  std::array<std::uint8_t, kMagic.size()> bytes{};
  std::ifstream input(path, std::ios::binary);
  return input
             .read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))
             .good() &&
         bytes == kMagic;
}

bool RunSyntheticTests(
    const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::create_directories(root, error);
  const std::filesystem::path archive_path = root / "synthetic-cac.big";
  constexpr std::string_view kDirectory =
      "data/content/createacharacter/model/cas_db/Test";
  if (!WriteSyntheticArchive(archive_path, kDirectory)) {
    std::cerr << "failed to write synthetic archive\n";
    return false;
  }

  skate3::cac_archive::Archive archive;
  std::string archive_error;
  if (!archive.Open(archive_path, archive_error)) {
    std::cerr << archive_error << '\n';
    return false;
  }
  const std::vector<std::string> paths =
      archive.PathsWithPrefix("data/content/createacharacter/model/cas_db/");
  if (paths.size() != 1 ||
      paths.front() != std::string(kDirectory) + "/fixture.rx2" ||
      archive.CacheKey().empty()) {
    std::cerr << "synthetic index mismatch\n";
    return false;
  }
  const std::filesystem::path extracted =
      root / "synthetic-output" / "fixture.rx2";
  std::vector<std::uint8_t> in_memory;
  if (!archive.Read(paths.front(), in_memory, archive_error) ||
      in_memory.size() != 16 || in_memory[0] != 0x89) {
    std::cerr << "synthetic in-memory read failed: "
              << archive_error << '\n';
    return false;
  }
  if (!archive.Extract(paths.front(), extracted, archive_error) ||
      !HasRx2Magic(extracted) ||
      !archive.Extract(paths.front(), extracted, archive_error)) {
    std::cerr << "synthetic extraction failed: " << archive_error << '\n';
    return false;
  }

  const std::filesystem::path zlib_path =
      root / "synthetic-zlib-cac.big";
  if (!WriteSyntheticArchive(
          zlib_path, kDirectory, true, 3)) {
    std::cerr << "failed to write synthetic zlib archive\n";
    return false;
  }
  skate3::cac_archive::Archive zlib_archive;
  if (!zlib_archive.Open(zlib_path, archive_error) ||
      !zlib_archive.Read(
          std::string(kDirectory) + "/fixture.rx2",
          in_memory, archive_error) ||
      in_memory.size() != 16 || in_memory[0] != 0x89) {
    std::cerr << "synthetic zlib read failed: "
              << archive_error << '\n';
    return false;
  }

  const std::filesystem::path method_four_path =
      root / "synthetic-method-four-cac.big";
  if (!WriteSyntheticArchive(
          method_four_path, kDirectory, true, 4)) {
    std::cerr << "failed to write synthetic method-four archive\n";
    return false;
  }
  skate3::cac_archive::Archive method_four_archive;
  if (!method_four_archive.Open(
          method_four_path, archive_error) ||
      !method_four_archive.Read(
          std::string(kDirectory) + "/fixture.rx2",
          in_memory, archive_error) ||
      in_memory.size() != 16 || in_memory[0] != 0x89) {
    std::cerr << "synthetic method-four read failed: "
              << archive_error << '\n';
    return false;
  }

  const std::filesystem::path unsafe_path = root / "unsafe-cac.big";
  if (!WriteSyntheticArchive(unsafe_path, "../escape", false)) {
    return false;
  }
  skate3::cac_archive::Archive unsafe;
  if (unsafe.Open(unsafe_path, archive_error)) {
    std::cerr << "unsafe archive path was accepted\n";
    return false;
  }
  return true;
}

bool VerifyInstalledCatalogue(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& output_root) {
  skate3::cac_archive::Archive archive;
  std::string error;
  if (!archive.Open(archive_path, error)) {
    std::cerr << "installed archive open failed: " << error << '\n';
    return false;
  }
  const std::vector<std::string> models =
      archive.PathsWithPrefix("data/content/createacharacter/model/cas_db/");
  if (models.empty()) {
    std::cerr << "installed archive contains no CAC models\n";
    return false;
  }
  std::size_t extracted = 0;
  for (const std::string& model : models) {
    const std::filesystem::path destination =
        output_root / std::filesystem::path(model);
    if (!archive.Extract(model, destination, error) ||
        !HasRx2Magic(destination)) {
      std::cerr << "installed model extraction failed path=" << model
                << " error=" << error << '\n';
      return false;
    }
    ++extracted;
  }
  const std::vector<std::string> textures =
      archive.PathsWithPrefix("data/content/createacharacter/texture/");
  if (textures.empty()) {
    std::cerr << "installed archive contains no CAC textures\n";
    return false;
  }
  const std::array<std::size_t, 3> texture_samples = {0, textures.size() / 2,
                                                      textures.size() - 1};
  for (const std::size_t index : texture_samples) {
    const std::string& texture = textures[index];
    const std::filesystem::path destination =
        output_root / std::filesystem::path(texture);
    if (!archive.Extract(texture, destination, error) ||
        !HasRx2Magic(destination)) {
      std::cerr << "installed texture extraction failed path=" << texture
                << " error=" << error << '\n';
      return false;
    }
  }
  std::cout << "verified installed CAC archive models=" << extracted
            << " texture_samples=" << texture_samples.size()
            << " cache_key=" << archive.CacheKey() << '\n';
  return true;
}

}  // namespace

int main(
    int argc, char** argv) {
  const std::filesystem::path test_root =
      argc >= 3
          ? std::filesystem::path(argv[2])
          : std::filesystem::temp_directory_path() / "skate3-cac-archive-tests";
  std::error_code error;
  std::filesystem::remove_all(test_root, error);
  if (!RunSyntheticTests(test_root)) {
    return 1;
  }
  if (argc >= 2 &&
      !VerifyInstalledCatalogue(argv[1], test_root / "installed")) {
    return 1;
  }
  std::cout << "skate3_cac_archive_tests passed\n";
  return 0;
}
