#include "skate3_cac_archive.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

#include <zlib.h>

namespace skate3::cac_archive {
namespace {

constexpr std::size_t kHeaderBytes = 48;
constexpr std::uint64_t kMaximumIndexedBytes = 16 * 1024 * 1024;
constexpr std::uint64_t kMaximumFileBytes = 64 * 1024 * 1024;

std::uint16_t ReadBe16(
    const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>((std::uint16_t(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t ReadBe24(
    const std::uint8_t* bytes) {
  return (std::uint32_t(bytes[0]) << 16) | (std::uint32_t(bytes[1]) << 8) |
         bytes[2];
}

std::uint32_t ReadBe32(
    const std::uint8_t* bytes) {
  return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
         (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

bool RangeValid(
    std::size_t available, std::size_t offset, std::size_t size) {
  return offset <= available && size <= available - offset;
}

bool AlignUp(
    std::size_t value, std::size_t alignment, std::size_t& output) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
    return false;
  }
  output = (value + alignment - 1) & ~(alignment - 1);
  return true;
}

std::string FixedString(
    const std::uint8_t* bytes, std::size_t size) {
  const auto end = std::find(bytes, bytes + size, std::uint8_t{0});
  return std::string(reinterpret_cast<const char*>(bytes),
                     reinterpret_cast<const char*>(end));
}

bool SafeArchivePath(
    std::string_view path) {
  if (path.empty() || path.front() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view component =
        path.substr(start, end == std::string_view::npos ? path.size() - start
                                                         : end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

bool CopyLiterals(
    const std::vector<std::uint8_t>& source, std::size_t& cursor,
    std::vector<std::uint8_t>& output, std::size_t count) {
  if (!RangeValid(source.size(), cursor, count)) {
    return false;
  }
  output.insert(output.end(), source.begin() + cursor,
                source.begin() + cursor + count);
  cursor += count;
  return true;
}

bool CopyReference(
    std::vector<std::uint8_t>& output, std::size_t distance, std::size_t count,
    std::size_t maximum_output) {
  if (distance == 0 || distance > output.size() ||
      count > maximum_output - output.size()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    output.push_back(output[output.size() - distance]);
  }
  return true;
}

bool DecompressRefPack(
    const std::vector<std::uint8_t>& source, std::vector<std::uint8_t>& output,
    std::string& error) {
  if (source.size() < 5 || ReadBe16(source.data()) != 0x10FBu) {
    error = "invalid RefPack header";
    return false;
  }
  const std::uint16_t flags = ReadBe16(source.data());
  std::size_t cursor = 2;
  std::uint32_t expected_size = 0;
  if ((flags & 0x8000u) != 0) {
    if ((flags & 0x0100u) != 0) {
      cursor += 4;
    }
    if (!RangeValid(source.size(), cursor, 4)) {
      error = "truncated RefPack extended size";
      return false;
    }
    expected_size = ReadBe32(source.data() + cursor);
    cursor += 4;
  } else {
    if ((flags & 0x0100u) != 0) {
      cursor += 3;
    }
    if (!RangeValid(source.size(), cursor, 3)) {
      error = "truncated RefPack size";
      return false;
    }
    expected_size = ReadBe24(source.data() + cursor);
    cursor += 3;
  }
  if (expected_size > kMaximumFileBytes) {
    error = "RefPack output exceeds safety limit";
    return false;
  }

  output.clear();
  output.reserve(expected_size);
  bool finished = false;
  while (!finished && cursor < source.size()) {
    const std::uint8_t control = source[cursor++];
    std::size_t literal_count = 0;
    std::size_t match_count = 0;
    std::size_t distance = 0;
    if (control < 0x80u) {
      if (!RangeValid(source.size(), cursor, 1)) {
        error = "truncated RefPack short command";
        return false;
      }
      const std::uint8_t next = source[cursor++];
      literal_count = control & 3u;
      match_count = ((control >> 2) & 7u) + 3u;
      distance = 1u + std::size_t(next) + (std::size_t(control & 0x60u) << 3);
    } else if (control < 0xC0u) {
      if (!RangeValid(source.size(), cursor, 2)) {
        error = "truncated RefPack medium command";
        return false;
      }
      const std::uint8_t next = source[cursor++];
      const std::uint8_t low = source[cursor++];
      literal_count = next >> 6;
      match_count = (control & 0x3Fu) + 4u;
      distance = 1u + (std::size_t(next & 0x3Fu) << 8) + low;
    } else if (control < 0xE0u) {
      if (!RangeValid(source.size(), cursor, 3)) {
        error = "truncated RefPack long command";
        return false;
      }
      const std::uint8_t high = source[cursor++];
      const std::uint8_t low = source[cursor++];
      const std::uint8_t length = source[cursor++];
      literal_count = control & 3u;
      match_count = (std::size_t(control & 0x0Cu) << 6) + length + 5u;
      distance = 1u + (std::size_t(control & 0x10u) << 12) +
                 (std::size_t(high) << 8) + low;
    } else if (control < 0xFCu) {
      literal_count = 4u * std::size_t(control & 0x1Fu) + 4u;
    } else {
      literal_count = control & 3u;
      finished = true;
    }

    if (literal_count > expected_size - output.size() ||
        !CopyLiterals(source, cursor, output, literal_count)) {
      error = "invalid RefPack literal range";
      return false;
    }
    if (match_count != 0 &&
        !CopyReference(output, distance, match_count, expected_size)) {
      error = "invalid RefPack back-reference";
      return false;
    }
  }
  if (!finished || output.size() != expected_size) {
    error = "RefPack output size mismatch";
    return false;
  }
  return true;
}

bool DecompressChunkRef(
    const std::vector<std::uint8_t>& source, std::uint32_t expected_size,
    std::vector<std::uint8_t>& output, std::string& error) {
  constexpr std::array<std::uint8_t, 8> kMagic = {'c', 'h', 'u', 'n',
                                                  'k', 'r', 'e', 'f'};
  if (source.size() < 28 ||
      !std::equal(kMagic.begin(), kMagic.end(), source.begin()) ||
      ReadBe32(source.data() + 8) != 2) {
    error = "invalid chunkref header";
    return false;
  }
  const std::uint32_t total_size = ReadBe32(source.data() + 12);
  const std::uint32_t chunk_size = ReadBe32(source.data() + 16);
  const std::uint32_t chunk_count = ReadBe32(source.data() + 20);
  const std::uint32_t alignment = ReadBe32(source.data() + 24);
  if (total_size != expected_size || total_size > kMaximumFileBytes ||
      chunk_size == 0 || chunk_size > kMaximumFileBytes || chunk_count == 0 ||
      chunk_count > (std::uint64_t(total_size) + chunk_size - 1) / chunk_size ||
      alignment == 0 || alignment > 4096 ||
      (alignment & (alignment - 1)) != 0) {
    error = "invalid chunkref dimensions";
    return false;
  }

  output.clear();
  output.reserve(total_size);
  std::size_t cursor = 28;
  for (std::uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
    std::size_t payload = 0;
    if (!AlignUp(cursor + 8, alignment, payload) || payload < 8) {
      error = "invalid chunkref alignment";
      return false;
    }
    const std::size_t descriptor = payload - 8;
    if (!RangeValid(source.size(), descriptor, 8)) {
      error = "truncated chunkref descriptor";
      return false;
    }
    const std::uint32_t packed_size = ReadBe32(source.data() + descriptor);
    const std::uint32_t method = ReadBe32(source.data() + descriptor + 4);
    if (!RangeValid(source.size(), payload, packed_size)) {
      error = "truncated chunkref payload";
      return false;
    }
    const std::size_t remaining = total_size - output.size();
    const std::size_t expected_chunk =
        std::min<std::size_t>(chunk_size, remaining);
    if (method == 0) {
      if (packed_size != expected_chunk) {
        error = "raw chunkref chunk size mismatch";
        return false;
      }
      output.insert(output.end(), source.begin() + payload,
                    source.begin() + payload + packed_size);
    } else if (method == 2) {
      std::vector<std::uint8_t> packed(source.begin() + payload,
                                       source.begin() + payload + packed_size);
      std::vector<std::uint8_t> unpacked;
      if (!DecompressRefPack(packed, unpacked, error) ||
          unpacked.size() != expected_chunk) {
        if (error.empty()) {
          error = "chunkref RefPack size mismatch";
        }
        return false;
      }
      output.insert(output.end(), unpacked.begin(), unpacked.end());
    } else if (method == 3) {
      std::vector<std::uint8_t> unpacked(expected_chunk);
      uLongf unpacked_size =
          static_cast<uLongf>(unpacked.size());
      const int status = uncompress(
          reinterpret_cast<Bytef*>(unpacked.data()),
          &unpacked_size,
          reinterpret_cast<const Bytef*>(source.data() + payload),
          static_cast<uLong>(packed_size));
      if (status != Z_OK ||
          unpacked_size != expected_chunk) {
        error = "chunkref zlib stream is invalid";
        return false;
      }
      output.insert(
          output.end(), unpacked.begin(), unpacked.end());
    } else if (method == 4) {
      // Retail parkassets uses method 4 for an exact-size aligned
      // passthrough block. Reject any other shape instead of guessing.
      if (packed_size != expected_chunk) {
        error = "method-4 chunkref chunk size mismatch";
        return false;
      }
      output.insert(
          output.end(), source.begin() + payload,
          source.begin() + payload + packed_size);
    } else {
      error =
          "unsupported chunkref compression method " +
          std::to_string(method);
      return false;
    }
    cursor = payload + packed_size;
  }
  if (output.size() != total_size || cursor != source.size()) {
    error = "chunkref output or packed size mismatch";
    return false;
  }
  return true;
}

std::uint64_t Fnv1a(
    std::uint64_t hash, const std::uint8_t* bytes, std::size_t size) {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  for (std::size_t index = 0; index < size; ++index) {
    hash = (hash ^ bytes[index]) * kPrime;
  }
  return hash;
}

}  // namespace

struct Archive::Impl {
  struct Entry {
    std::uint64_t offset = 0;
    std::uint32_t packed_size = 0;
    std::uint32_t unpacked_size = 0;
    std::uint8_t method = 0;
  };

  std::filesystem::path path;
  std::unordered_map<std::string, Entry> entries;
  std::uint64_t archive_size = 0;
  std::uint64_t index_hash = 14695981039346656037ull;
};

Archive::Archive() : impl_(std::make_unique<Impl>()) {}
Archive::~Archive() = default;
Archive::Archive(Archive&&) noexcept = default;
Archive& Archive::operator=(Archive&&) noexcept = default;

bool Archive::Open(
    const std::filesystem::path& path, std::string& error) {
  error.clear();
  std::error_code file_error;
  const std::uint64_t archive_size =
      std::filesystem::file_size(path, file_error);
  if (file_error || archive_size < kHeaderBytes) {
    error = "CAC archive is unavailable or truncated";
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  std::array<std::uint8_t, kHeaderBytes> header{};
  if (!stream.read(reinterpret_cast<char*>(header.data()), header.size())) {
    error = "failed to read CAC archive header";
    return false;
  }
  if (header[0] != 'E' || header[1] != 'B' ||
      ReadBe16(header.data() + 2) != 3) {
    error = "unsupported CAC archive format";
    return false;
  }
  const std::uint32_t entry_count = ReadBe32(header.data() + 4);
  const std::uint16_t flags = ReadBe16(header.data() + 8);
  const std::uint8_t offset_shift = header[10];
  const std::uint32_t name_offset = ReadBe32(header.data() + 12);
  const std::uint32_t name_bytes = ReadBe32(header.data() + 16);
  const std::uint8_t name_record_bytes = header[20];
  const std::uint8_t directory_record_bytes = header[21];
  const std::uint8_t directory_count = header[23];
  const std::size_t entry_bytes = (flags & 1u) != 0 ? 20u : 16u;
  if (entry_count == 0 || entry_count > 100000 || offset_shift > 31 ||
      name_record_bytes < 3 || directory_record_bytes < 2 ||
      directory_count == 0 ||
      std::uint64_t(entry_count) * entry_bytes > kMaximumIndexedBytes ||
      name_bytes > kMaximumIndexedBytes ||
      std::uint64_t(name_offset) + name_bytes > archive_size) {
    error = "invalid CAC archive index dimensions";
    return false;
  }

  const std::size_t entries_size = std::size_t(entry_count) * entry_bytes;
  std::size_t aligned_entries_size = 0;
  if (!AlignUp(entries_size, 16, aligned_entries_size)) {
    error = "CAC archive entry index overflow";
    return false;
  }
  const std::size_t methods_offset = kHeaderBytes + aligned_entries_size;
  if (std::uint64_t(methods_offset) + entry_count > name_offset) {
    error = "overlapping CAC archive indexes";
    return false;
  }
  std::vector<std::uint8_t> entries(entries_size);
  stream.seekg(kHeaderBytes);
  if (!stream.read(reinterpret_cast<char*>(entries.data()), entries.size())) {
    error = "failed to read CAC archive entries";
    return false;
  }
  std::vector<std::uint8_t> methods(entry_count);
  stream.seekg(methods_offset);
  if (!stream.read(reinterpret_cast<char*>(methods.data()), methods.size())) {
    error = "failed to read CAC archive compression index";
    return false;
  }
  std::vector<std::uint8_t> names(name_bytes);
  stream.seekg(name_offset);
  if (!stream.read(reinterpret_cast<char*>(names.data()), names.size())) {
    error = "failed to read CAC archive names";
    return false;
  }

  const std::size_t name_records_size =
      std::size_t(entry_count) * name_record_bytes;
  std::size_t directory_offset = 0;
  if (!AlignUp(name_records_size, 16, directory_offset) ||
      !RangeValid(names.size(), 0,
                  directory_offset +
                      std::size_t(directory_count) * directory_record_bytes)) {
    error = "invalid CAC archive name index";
    return false;
  }
  std::vector<std::string> directories;
  directories.reserve(directory_count);
  for (std::size_t index = 0; index < directory_count; ++index) {
    directories.push_back(FixedString(
        names.data() + directory_offset + index * directory_record_bytes,
        directory_record_bytes));
  }

  auto next = std::make_unique<Impl>();
  next->path = path;
  next->archive_size = archive_size;
  next->index_hash = Fnv1a(next->index_hash, header.data(), header.size());
  next->index_hash = Fnv1a(next->index_hash, entries.data(), entries.size());
  next->index_hash = Fnv1a(next->index_hash, methods.data(), methods.size());
  next->index_hash = Fnv1a(next->index_hash, names.data(), names.size());
  next->entries.reserve(entry_count);
  for (std::size_t index = 0; index < entry_count; ++index) {
    const std::uint8_t* name_record = names.data() + index * name_record_bytes;
    const std::uint16_t directory_index = ReadBe16(name_record);
    if (directory_index >= directories.size()) {
      error = "invalid CAC archive directory reference";
      return false;
    }
    const std::string filename =
        FixedString(name_record + 2, name_record_bytes - 2);
    std::string archive_path =
        directories[directory_index] == "."
            ? filename
            : directories[directory_index] + "/" + filename;
    if (!SafeArchivePath(archive_path)) {
      error = "unsafe CAC archive path";
      return false;
    }
    const std::uint8_t* entry = entries.data() + index * entry_bytes;
    const std::uint64_t offset = std::uint64_t(ReadBe32(entry)) << offset_shift;
    const std::uint32_t stored_packed_size = ReadBe32(entry + 4);
    const std::uint32_t unpacked_size =
        ReadBe32(entry + 8) != 0 ? ReadBe32(entry + 8) : stored_packed_size;
    const std::uint32_t packed_size =
        stored_packed_size != 0 ? stored_packed_size : unpacked_size;
    if (packed_size == 0 || unpacked_size == 0 ||
        unpacked_size > kMaximumFileBytes || offset > archive_size ||
        packed_size > archive_size - offset ||
        (methods[index] != 0 && methods[index] != 2)) {
      error = "invalid CAC archive file entry";
      return false;
    }
    if (!next->entries
             .emplace(std::move(archive_path),
                      Impl::Entry{offset, packed_size, unpacked_size,
                                  methods[index]})
             .second) {
      error = "duplicate CAC archive path";
      return false;
    }
  }
  impl_ = std::move(next);
  return true;
}

std::vector<std::string> Archive::PathsWithPrefix(
    std::string_view prefix) const {
  std::vector<std::string> paths;
  for (const auto& [path, entry] : impl_->entries) {
    (void)entry;
    if (path.starts_with(prefix)) {
      paths.push_back(path);
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string Archive::CacheKey() const {
  std::ostringstream key;
  key << "ebv3-" << impl_->archive_size << '-' << std::hex << std::setw(16)
      << std::setfill('0') << impl_->index_hash;
  return key.str();
}

bool Archive::Read(
    std::string_view archive_path,
    std::vector<std::uint8_t>& bytes,
    std::string& error) const {
  error.clear();
  bytes.clear();
  const auto found = impl_->entries.find(std::string(archive_path));
  if (found == impl_->entries.end()) {
    error = "CAC asset is not present in the archive";
    return false;
  }
  const Impl::Entry& entry = found->second;
  std::ifstream stream(impl_->path, std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(entry.offset));
  std::vector<std::uint8_t> packed(entry.packed_size);
  if (!stream.read(
          reinterpret_cast<char*>(packed.data()), packed.size())) {
    error = "failed to read CAC asset payload";
    return false;
  }
  if (entry.method == 0) {
    if (entry.packed_size != entry.unpacked_size) {
      error = "raw CAC asset size mismatch";
      return false;
    }
    bytes = std::move(packed);
    return true;
  }
  return DecompressChunkRef(
      packed, entry.unpacked_size, bytes, error);
}

bool Archive::Extract(
    std::string_view archive_path, const std::filesystem::path& destination,
    std::string& error) const {
  error.clear();
  const auto found = impl_->entries.find(std::string(archive_path));
  if (found == impl_->entries.end()) {
    error = "CAC asset is not present in the archive";
    return false;
  }
  const Impl::Entry& entry = found->second;
  std::error_code filesystem_error;
  if (std::filesystem::is_regular_file(destination, filesystem_error) &&
      !filesystem_error &&
      std::filesystem::file_size(destination, filesystem_error) ==
          entry.unpacked_size &&
      !filesystem_error) {
    return true;
  }

  std::ifstream stream(impl_->path, std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(entry.offset));
  std::vector<std::uint8_t> packed(entry.packed_size);
  if (!stream.read(reinterpret_cast<char*>(packed.data()), packed.size())) {
    error = "failed to read CAC asset payload";
    return false;
  }
  std::vector<std::uint8_t> unpacked;
  if (entry.method == 0) {
    if (entry.packed_size != entry.unpacked_size) {
      error = "raw CAC asset size mismatch";
      return false;
    }
    unpacked = std::move(packed);
  } else if (!DecompressChunkRef(packed, entry.unpacked_size, unpacked,
                                 error)) {
    return false;
  }

  std::filesystem::create_directories(destination.parent_path(),
                                      filesystem_error);
  if (filesystem_error) {
    error = "failed to create CAC cache directory";
    return false;
  }
  std::filesystem::path temporary = destination;
  temporary += ".tmp";
  std::filesystem::remove(temporary, filesystem_error);
  filesystem_error.clear();
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.write(reinterpret_cast<const char*>(unpacked.data()),
                      static_cast<std::streamsize>(unpacked.size()))) {
      error = "failed to write CAC cache file";
      return false;
    }
  }
  std::filesystem::remove(destination, filesystem_error);
  filesystem_error.clear();
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    error = "failed to publish CAC cache file";
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
  return true;
}

}  // namespace skate3::cac_archive
