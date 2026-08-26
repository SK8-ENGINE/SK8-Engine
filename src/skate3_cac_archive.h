#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace skate3::cac_archive {

// Read-only index for the EB v3 archive already present in the user's
// installed Skate 3 data. The archive owns no retail payload in memory:
// individual files are validated and decompressed only when requested.
class Archive {
 public:
  Archive();
  ~Archive();
  Archive(Archive&&) noexcept;
  Archive& operator=(Archive&&) noexcept;
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  bool Open(const std::filesystem::path& path, std::string& error);

  [[nodiscard]] std::vector<std::string> PathsWithPrefix(
      std::string_view prefix) const;
  [[nodiscard]] std::string CacheKey() const;

  // Reads one archive member directly into memory. This lets local importers
  // convert the user's assets without publishing intermediate extracted
  // game files.
  bool Read(std::string_view archive_path,
            std::vector<std::uint8_t>& bytes,
            std::string& error) const;

  // Extracts one indexed archive-relative path using an atomic file replace.
  // A complete existing file with the expected uncompressed size is reused.
  bool Extract(std::string_view archive_path,
               const std::filesystem::path& destination,
               std::string& error) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace skate3::cac_archive
