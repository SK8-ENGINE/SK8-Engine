#include "skate3_ui_asset_cache.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace skate3::vanilla_ui {
namespace {

constexpr std::string_view kBackdropArchivePath =
    "data/fe/source/screens/main/fe_root.rx2";
constexpr std::string_view kBackdropTextureName =
    "fe_root_textures\\1.texture";
constexpr std::uint32_t kRw4TocType = 0x00EB000B;
constexpr std::uint32_t kDxt5Format = 20;
constexpr std::uint32_t kBackdropWidth = 512;
constexpr std::uint32_t kBackdropHeight = 256;

std::uint32_t Align(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return value;
}

class Reader {
public:
  explicit Reader(const std::vector<std::uint8_t> &data) : data_(data) {}

  bool Has(std::uint64_t offset, std::uint64_t size) const {
    return offset <= data_.size() && size <= data_.size() - offset;
  }
  std::uint8_t U8(std::size_t offset) const { return data_[offset]; }
  std::uint16_t U16(std::size_t offset) const {
    return std::uint16_t(data_[offset]) << 8 |
           std::uint16_t(data_[offset + 1]);
  }
  std::uint32_t U32(std::size_t offset) const {
    return std::uint32_t(data_[offset]) << 24 |
           std::uint32_t(data_[offset + 1]) << 16 |
           std::uint32_t(data_[offset + 2]) << 8 |
           std::uint32_t(data_[offset + 3]);
  }
  std::uint64_t U64(std::size_t offset) const {
    return std::uint64_t(U32(offset)) << 32 | U32(offset + 4);
  }
  std::string CString(std::size_t offset) const {
    if (offset >= data_.size()) {
      return {};
    }
    auto end = offset;
    while (end < data_.size() && data_[end]) {
      ++end;
    }
    return std::string(reinterpret_cast<const char *>(data_.data() + offset),
                       end - offset);
  }

private:
  const std::vector<std::uint8_t> &data_;
};

bool ReadFile(const std::filesystem::path &path,
              std::vector<std::uint8_t> &data, std::string &error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "Unable to open " + path.string();
    return false;
  }
  stream.seekg(0, std::ios::end);
  const auto size = stream.tellg();
  if (size < 0 ||
      std::uint64_t(size) > std::numeric_limits<std::size_t>::max()) {
    error = "Invalid file size for " + path.string();
    return false;
  }
  stream.seekg(0, std::ios::beg);
  data.resize(static_cast<std::size_t>(size));
  if (size && !stream.read(reinterpret_cast<char *>(data.data()), size)) {
    error = "Unable to read " + path.string();
    return false;
  }
  return true;
}

bool CopyBackReference(std::vector<std::uint8_t> &output,
                       std::size_t distance, std::size_t count) {
  if (!distance || distance > output.size()) {
    return false;
  }
  while (count--) {
    output.push_back(output[output.size() - distance]);
  }
  return true;
}

bool DecompressRefPack(const std::uint8_t *data, std::size_t size,
                       std::size_t expected,
                       std::vector<std::uint8_t> &output) {
  if (size < 2) {
    return false;
  }
  std::size_t position = 0;
  if (data[1] == 0xFB && (data[0] == 0x10 || data[0] == 0x90)) {
    position = (data[0] & 0x80) ? 6 : 5;
    if (position > size) {
      return false;
    }
  }
  output.clear();
  output.reserve(expected);
  auto literal = [&](std::size_t count) {
    if (count > size - position) {
      return false;
    }
    output.insert(output.end(), data + position, data + position + count);
    position += count;
    return true;
  };
  while (position < size) {
    const auto control = data[position++];
    if (control < 0x80) {
      if (position >= size) {
        return false;
      }
      const auto b1 = data[position++];
      if (!literal(control & 3) ||
          !CopyBackReference(output, ((control & 0x60) << 3) + b1 + 1,
                             ((control >> 2) & 7) + 3)) {
        return false;
      }
    } else if (control < 0xC0) {
      if (size - position < 2) {
        return false;
      }
      const auto b1 = data[position++];
      const auto b2 = data[position++];
      if (!literal(b1 >> 6) ||
          !CopyBackReference(output, ((b1 & 0x3F) << 8) + b2 + 1,
                             (control & 0x3F) + 4)) {
        return false;
      }
    } else if (control < 0xE0) {
      if (size - position < 3) {
        return false;
      }
      const auto b1 = data[position++];
      const auto b2 = data[position++];
      const auto b3 = data[position++];
      if (!literal(control & 3) ||
          !CopyBackReference(
              output, ((control & 0x10) << 12) + (b1 << 8) + b2 + 1,
              ((control & 0x0C) << 6) + b3 + 5)) {
        return false;
      }
    } else if (control < 0xFC) {
      if (!literal(((control & 0x1F) << 2) + 4)) {
        return false;
      }
    } else {
      if (!literal(control & 3)) {
        return false;
      }
      break;
    }
    if (output.size() > expected) {
      return false;
    }
  }
  return output.size() == expected;
}

bool ExtractBigEntry(const std::filesystem::path &archive_path,
                     std::string_view wanted_path,
                     std::vector<std::uint8_t> &output, std::string &error) {
  std::vector<std::uint8_t> archive;
  if (!ReadFile(archive_path, archive, error)) {
    return false;
  }
  Reader reader(archive);
  if (!reader.Has(0, 48) || reader.U16(0) != 0x4542 ||
      reader.U16(2) != 3) {
    error = "Unsupported front-end BIG archive";
    return false;
  }
  const auto count = reader.U32(4);
  const auto flags = reader.U16(8);
  const auto shift = reader.U8(10);
  const auto index_size = reader.U32(12);
  const auto names_size = reader.U32(16);
  const auto name_record_size = reader.U8(20);
  const auto directory_record_size = reader.U8(21);
  const auto entry_size = (flags & 1) ? 20u : 16u;
  if (count > 1000000 || shift > 31 || name_record_size < 3 ||
      directory_record_size < 2 ||
      !reader.Has(48, std::uint64_t(count) * entry_size) ||
      !reader.Has(index_size, names_size)) {
    error = "Malformed front-end BIG archive";
    return false;
  }
  const auto compression_start =
      48 + Align(count * entry_size, 16);
  const auto names_start = index_size;
  const auto directories_start =
      names_start + Align(count * name_record_size, 16);
  if (!reader.Has(compression_start, count) ||
      directories_start > std::uint64_t(index_size) + names_size) {
    error = "Malformed front-end BIG index";
    return false;
  }
  std::vector<std::string> directories;
  for (std::uint64_t cursor = directories_start;
       cursor + directory_record_size <=
       std::uint64_t(index_size) + names_size;
       cursor += directory_record_size) {
    auto end = cursor;
    while (end < cursor + directory_record_size && archive[end]) {
      ++end;
    }
    directories.emplace_back(
        reinterpret_cast<const char *>(archive.data() + cursor), end - cursor);
  }

  const auto wanted = Lower(std::string(wanted_path));
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto entry = 48 + index * entry_size;
    const auto file_offset = std::uint64_t(reader.U32(entry)) << shift;
    const auto stored_field = reader.U32(entry + 4);
    const auto unpacked_field = reader.U32(entry + 8);
    const auto unpacked = unpacked_field ? unpacked_field : stored_field;
    const auto stored = stored_field ? stored_field : unpacked;
    const auto name_record = names_start + index * name_record_size;
    if (!reader.Has(name_record, name_record_size) ||
        !reader.Has(file_offset, stored)) {
      error = "Malformed front-end BIG entry";
      return false;
    }
    const auto directory_index = reader.U16(name_record);
    auto end = name_record + 2;
    while (end < name_record + name_record_size && archive[end]) {
      ++end;
    }
    std::string filename(
        reinterpret_cast<const char *>(archive.data() + name_record + 2),
        end - name_record - 2);
    std::string path = filename;
    if (directory_index < directories.size() &&
        !directories[directory_index].empty() &&
        directories[directory_index] != ".") {
      path = directories[directory_index] + "/" + filename;
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (Lower(path) != wanted) {
      continue;
    }
    const auto compression = reader.U8(compression_start + index);
    if (!compression) {
      output.assign(archive.begin() + file_offset,
                    archive.begin() + file_offset + stored);
      return output.size() == unpacked;
    }
    if (compression == 1 &&
        DecompressRefPack(archive.data() + file_offset, stored, unpacked,
                          output)) {
      return true;
    }
    error = "Unsupported or invalid compression for " + path;
    return false;
  }
  error = "Required retail UI arena was not found in fetexture.big";
  return false;
}

std::uint32_t ReverseBits(std::uint32_t value, unsigned size) {
  std::uint32_t result = 0;
  for (unsigned bit = 0; bit < size; ++bit) {
    result |= ((value >> bit) & 1) << (size - bit - 1);
  }
  return result;
}

std::size_t TiledOffset(std::uint32_t x, std::uint32_t y,
                        std::uint32_t width, unsigned log_bpb) {
  const auto aligned_width = Align(width, 32);
  const auto macro =
      ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpb + 7);
  const auto micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb;
  const auto offset =
      macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  const auto address =
      ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) +
      ((offset & 0x1C0) << 2) +
      (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  return address >> log_bpb;
}

std::array<std::uint8_t, 3> Expand565(std::uint16_t value) {
  const auto r = (value >> 11) & 0x1F;
  const auto g = (value >> 5) & 0x3F;
  const auto b = value & 0x1F;
  return {std::uint8_t((r << 3) | (r >> 2)),
          std::uint8_t((g << 2) | (g >> 4)),
          std::uint8_t((b << 3) | (b >> 2))};
}

bool DecodeDxt5(const std::vector<std::uint8_t> &tiled, std::uint32_t width,
                std::uint32_t height, bool is_tiled,
                std::vector<std::uint8_t> &rgba) {
  const auto blocks_wide = std::max(1u, (width + 3) / 4);
  const auto blocks_high = std::max(1u, (height + 3) / 4);
  const auto tiled_width = Align(width, 128) / 4;
  rgba.assign(std::size_t(width) * height * 4, 0);
  for (std::uint32_t by = 0; by < blocks_high; ++by) {
    for (std::uint32_t bx = 0; bx < blocks_wide; ++bx) {
      const auto source =
          is_tiled ? TiledOffset(bx, by, tiled_width, 4) * 16
                   : (std::size_t(by) * blocks_wide + bx) * 16;
      if (source + 16 > tiled.size()) {
        return false;
      }
      std::array<std::uint8_t, 16> block{};
      for (unsigned i = 0; i < 16; i += 2) {
        block[i] = tiled[source + i + 1];
        block[i + 1] = tiled[source + i];
      }
      const auto alpha0 = block[0];
      const auto alpha1 = block[1];
      std::array<std::uint8_t, 8> alphas{alpha0, alpha1};
      if (alpha0 > alpha1) {
        for (unsigned i = 1; i < 7; ++i) {
          alphas[i + 1] =
              std::uint8_t(((7 - i) * alpha0 + i * alpha1) / 7);
        }
      } else {
        for (unsigned i = 1; i < 5; ++i) {
          alphas[i + 1] =
              std::uint8_t(((5 - i) * alpha0 + i * alpha1) / 5);
        }
        alphas[6] = 0;
        alphas[7] = 255;
      }
      std::uint64_t alpha_bits = 0;
      for (unsigned i = 0; i < 6; ++i) {
        alpha_bits |= std::uint64_t(block[2 + i]) << (i * 8);
      }
      const auto c0 = std::uint16_t(block[8]) |
                      std::uint16_t(block[9]) << 8;
      const auto c1 = std::uint16_t(block[10]) |
                      std::uint16_t(block[11]) << 8;
      const auto color_bits = std::uint32_t(block[12]) |
                              std::uint32_t(block[13]) << 8 |
                              std::uint32_t(block[14]) << 16 |
                              std::uint32_t(block[15]) << 24;
      const auto color0 = Expand565(c0);
      const auto color1 = Expand565(c1);
      std::array<std::array<std::uint8_t, 3>, 4> colors{
          color0, color1,
          std::array<std::uint8_t, 3>{
              std::uint8_t((2 * color0[0] + color1[0]) / 3),
              std::uint8_t((2 * color0[1] + color1[1]) / 3),
              std::uint8_t((2 * color0[2] + color1[2]) / 3)},
          std::array<std::uint8_t, 3>{
              std::uint8_t((color0[0] + 2 * color1[0]) / 3),
              std::uint8_t((color0[1] + 2 * color1[1]) / 3),
              std::uint8_t((color0[2] + 2 * color1[2]) / 3)}};
      for (unsigned py = 0; py < 4; ++py) {
        for (unsigned px = 0; px < 4; ++px) {
          const auto x = bx * 4 + px;
          const auto y = by * 4 + py;
          if (x >= width || y >= height) {
            continue;
          }
          const auto pixel = py * 4 + px;
          const auto &color = colors[(color_bits >> (pixel * 2)) & 3];
          const auto alpha = alphas[(alpha_bits >> (pixel * 3)) & 7];
          const auto target = (std::size_t(y) * width + x) * 4;
          rgba[target] = color[0];
          rgba[target + 1] = color[1];
          rgba[target + 2] = color[2];
          rgba[target + 3] = alpha;
        }
      }
    }
  }
  return true;
}

bool ExtractBackdrop(const std::vector<std::uint8_t> &arena,
                     std::vector<std::uint8_t> &rgba, std::string &error) {
  Reader reader(arena);
  if (!reader.Has(0, 72) ||
      std::string_view(reinterpret_cast<const char *>(arena.data()), 4) !=
          std::string_view("\x89RW4", 4) ||
      std::string_view(reinterpret_cast<const char *>(arena.data() + 4), 4) !=
          std::string_view("xb2\0", 4)) {
    error = "Invalid Xbox 360 RW4 retail UI arena";
    return false;
  }
  const auto count = reader.U32(32);
  const auto dictionary_offset = reader.U32(48);
  const auto resource_header_size = reader.U32(68);
  if (count > 1000000 || !reader.Has(dictionary_offset, std::uint64_t(count) * 24)) {
    error = "Malformed Xbox 360 RW4 dictionary";
    return false;
  }
  struct Entry {
    std::uint32_t pointer;
    std::uint32_t size;
    std::uint32_t type;
  };
  std::vector<Entry> dictionary;
  dictionary.reserve(count);
  std::uint32_t toc_offset = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto offset = dictionary_offset + i * 24;
    Entry entry{reader.U32(offset), reader.U32(offset + 8),
                reader.U32(offset + 20)};
    if (entry.type == kRw4TocType) {
      toc_offset = entry.pointer;
    }
    dictionary.push_back(entry);
  }
  if (!toc_offset || !reader.Has(toc_offset, 20)) {
    error = "RW4 retail UI arena has no table of contents";
    return false;
  }
  const auto item_count = reader.U32(toc_offset);
  const auto item_array = reader.U32(toc_offset + 4);
  if (item_count > 1000000 ||
      !reader.Has(std::uint64_t(toc_offset) + item_array,
                  std::uint64_t(item_count) * 24)) {
    error = "Malformed RW4 retail UI table of contents";
    return false;
  }
  for (std::uint32_t i = 0; i < item_count; ++i) {
    const auto item = toc_offset + item_array + i * 24;
    const auto name = Lower(reader.CString(toc_offset + reader.U32(item)));
    if (name != kBackdropTextureName) {
      continue;
    }
    const auto dictionary_index = reader.U32(item + 20) - 1;
    if (dictionary_index + 1 >= dictionary.size()) {
      break;
    }
    const auto &payload = dictionary[dictionary_index];
    const auto &info = dictionary[dictionary_index + 1];
    const auto data_offset =
        std::uint64_t(payload.pointer) + resource_header_size;
    if (!reader.Has(data_offset, payload.size) ||
        !reader.Has(std::uint64_t(info.pointer) + 28, 24)) {
      break;
    }
    const auto fetch0 = ReverseBits(reader.U32(info.pointer + 28), 32);
    const auto fetch1 = ReverseBits(reader.U32(info.pointer + 32), 32);
    const auto dimensions = reader.U32(info.pointer + 36);
    const auto width = (dimensions & 0x1FFF) + 1;
    const auto height = ((dimensions >> 13) & 0x1FFF) + 1;
    const auto format = ReverseBits((fetch1 >> 26) & 0x3F, 6);
    const auto tiled = ReverseBits(fetch0 & 1, 1) != 0;
    if (width != kBackdropWidth || height != kBackdropHeight ||
        format != kDxt5Format) {
      error = "Unexpected FE_root retail backdrop format: " +
              std::to_string(width) + "x" + std::to_string(height) +
              " format=" + std::to_string(format) +
              " tiled=" + std::to_string(tiled);
      return false;
    }
    std::vector<std::uint8_t> texture(
        arena.begin() + data_offset,
        arena.begin() + data_offset + payload.size);
    if (!DecodeDxt5(texture, width, height, tiled, rgba)) {
      error = "Unable to decode FE_root retail backdrop";
      return false;
    }
    return true;
  }
  error = "FE_root retail backdrop texture was not found";
  return false;
}

bool WriteFile(const std::filesystem::path &path,
               const std::vector<std::uint8_t> &data, std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "Unable to create retail UI cache directory: " + ec.message();
    return false;
  }
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream ||
        !stream.write(reinterpret_cast<const char *>(data.data()),
                      std::streamsize(data.size()))) {
      error = "Unable to write retail UI cache file";
      return false;
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
  }
  if (ec) {
    error = "Unable to finalize retail UI cache file: " + ec.message();
    return false;
  }
  return true;
}

} // namespace

std::filesystem::path
RetailBackdropPath(const std::filesystem::path &cache_root) {
  return cache_root / "assets" / "data" / "fe" / "source" / "screens" /
         "main" / "FE_root" / "0000_1.Texture.rgba";
}

bool EnsureRetailAssetBootstrap(const std::filesystem::path &game_root,
                                const std::filesystem::path &cache_root,
                                std::string &error) {
  const auto backdrop = RetailBackdropPath(cache_root);
  std::error_code ec;
  if (std::filesystem::file_size(backdrop, ec) ==
          std::uintmax_t(kBackdropWidth) * kBackdropHeight * 4 &&
      !ec) {
    return true;
  }
  std::vector<std::uint8_t> arena;
  if (!ExtractBigEntry(game_root / "data" / "big" / "fetexture.big",
                       kBackdropArchivePath, arena, error)) {
    return false;
  }
  std::vector<std::uint8_t> rgba;
  if (!ExtractBackdrop(arena, rgba, error)) {
    return false;
  }
  return WriteFile(backdrop, rgba, error);
}

} // namespace skate3::vanilla_ui
