#include "skate/world/rw_collision_mesh.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "RW_COLLISION_ARCHIVE_FAIL " << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::uint32_t ReadLeU32(std::span<const std::uint8_t> bytes,
                        std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4) {
    Fail("little-endian field extends beyond the archive");
  }
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

std::uint32_t ReadBeU32(std::span<const std::uint8_t> bytes,
                        std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4) {
    Fail("big-endian field extends beyond a collision mesh");
  }
  return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8u) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    Fail("could not open archive: " + path.string());
  }
  const std::streamoff end = stream.tellg();
  if (end < 12 ||
      static_cast<std::uint64_t>(end) >
          std::numeric_limits<std::size_t>::max()) {
    Fail("archive size is invalid");
  }
  stream.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  stream.read(reinterpret_cast<char*>(bytes.data()), end);
  if (!stream) {
    Fail("could not read complete archive");
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: skate_rw_collision_archive_validate <archive>\n";
    return EXIT_FAILURE;
  }

  const std::vector<std::uint8_t> archive =
      ReadFile(std::filesystem::path(argv[1]));
  constexpr std::array<std::uint8_t, 8> kMagic{
      'R', 'W', 'C', 'M', 'S', 'E', 'T', '1'};
  if (!std::equal(kMagic.begin(), kMagic.end(), archive.begin())) {
    Fail("archive magic is invalid");
  }

  const std::uint32_t mesh_count = ReadLeU32(archive, 8);
  if (mesh_count == 0) {
    Fail("archive has no collision meshes");
  }
  std::size_t cursor = 12;
  std::uint64_t triangle_count = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t branchless_count = 0;
  for (std::uint32_t index = 0; index < mesh_count; ++index) {
    const std::uint32_t name_size = ReadLeU32(archive, cursor);
    cursor += 4;
    if (name_size == 0 || name_size > 4096 ||
        cursor > archive.size() ||
        name_size > archive.size() - cursor) {
      Fail("mesh name is invalid at record " + std::to_string(index));
    }
    const std::string name(
        reinterpret_cast<const char*>(archive.data() + cursor),
        name_size);
    cursor += name_size;

    const std::uint32_t mesh_size = ReadLeU32(archive, cursor);
    cursor += 4;
    if (cursor > archive.size() ||
        mesh_size > archive.size() - cursor) {
      Fail(name + ": mesh payload extends beyond the archive");
    }
    const std::span<const std::uint8_t> serialized(
        archive.data() + cursor, mesh_size);
    skate::world::RwCollisionBuildResult loaded =
        skate::world::LoadSerializedRwCollisionMesh(serialized);
    if (!loaded.ok) {
      Fail(name + ": " + loaded.error);
    }

    const std::uint32_t kd_offset = ReadBeU32(serialized, 48);
    const std::uint32_t cluster_table_offset =
        ReadBeU32(serialized, 52);
    const std::uint32_t branch_offset =
        ReadBeU32(serialized, kd_offset);
    const std::uint32_t branch_count =
        ReadBeU32(serialized, kd_offset + 4);
    const std::uint32_t first_cluster_offset =
        ReadBeU32(serialized, cluster_table_offset);
    skate::world::Vec3 applied_translation{};
    std::string translation_error;
    if (!skate::world::TranslateSerializedRwCollisionMesh(
            loaded.mesh, {123.456f, -78.9f, 42.125f},
            &applied_translation, &translation_error)) {
      Fail(name + ": rigid translation failed: " + translation_error);
    }
    std::vector<std::uint8_t> fixed = loaded.mesh.bytes;
    constexpr std::uint32_t kGuestAddress = 0x50000000u;
    if (!skate::world::FixupRwCollisionMeshForGuest(
            fixed, kGuestAddress)) {
      Fail(name + ": guest pointer fixup failed");
    }
    if (ReadBeU32(fixed, 48) != kGuestAddress + kd_offset ||
        ReadBeU32(fixed, 52) !=
            kGuestAddress + cluster_table_offset ||
        ReadBeU32(fixed, cluster_table_offset) !=
            first_cluster_offset) {
      Fail(name + ": top-level pointer fixup contract changed");
    }
    if (branch_count == 0) {
      ++branchless_count;
      if (ReadBeU32(fixed, kd_offset) != branch_offset) {
        Fail(name + ": unused branchless pointer was modified");
      }
    } else if (ReadBeU32(fixed, kd_offset) !=
               kGuestAddress + branch_offset) {
      Fail(name + ": KD branch pointer was not fixed up");
    }

    triangle_count += loaded.mesh.triangle_count;
    payload_bytes += loaded.mesh.bytes.size();
    cursor += mesh_size;
  }
  if (cursor != archive.size()) {
    Fail("archive has trailing bytes");
  }

  std::cout << "RW_COLLISION_ARCHIVE_OK"
            << " meshes=" << mesh_count
            << " triangles=" << triangle_count
            << " payload_bytes=" << payload_bytes
            << " archive_bytes=" << archive.size()
            << " branchless=" << branchless_count << '\n';
  return EXIT_SUCCESS;
}
