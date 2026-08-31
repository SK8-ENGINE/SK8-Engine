#include "skate3_custom_textures.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#include <objidl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

namespace skate3::custom_textures {
namespace {

// ---------------------------------------------------------------------------
// Small binary readers (little-endian).
// ---------------------------------------------------------------------------
inline std::uint32_t ReadU16LE(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8);
}
inline std::uint32_t ReadU32LE(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::int32_t ReadI32LE(const std::uint8_t* p) {
  return static_cast<std::int32_t>(ReadU32LE(p));
}
inline std::uint32_t FourCC(char a, char b, char c, char d) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}
inline std::int32_t AbsI32(std::int32_t v) {
  return v < 0 ? -v : v;
}
inline unsigned PopCount(std::uint32_t v) {
  unsigned c = 0;
  while (v) {
    v &= v - 1;
    ++c;
  }
  return c;
}
// Caps decode sizes so malformed files cannot force huge allocations.
inline bool CheckImageDims(std::uint32_t width, std::uint32_t height,
                           std::string& error) {
  constexpr std::uint64_t kMaxPixels = 67'108'864ull;  // 8192x8192
  if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
      static_cast<std::uint64_t>(width) * height > kMaxPixels) {
    error = "Image dimensions are too large";
    return false;
  }
  return true;
}

bool ReadFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>& bytes, std::string& error) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = "Could not read \"" + path.string() + "\": " + ec.message();
    return false;
  }
  if (size > (256ull * 1024ull * 1024ull)) {
    error = "Image file is too large (" + std::to_string(size) + " bytes)";
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "Could not open \"" + path.string() + "\"";
    return false;
  }
  bytes.resize(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  if (!in) {
    error = "Could not read \"" + path.string() + "\"";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// TGA decoder (types 1/2/3 raw and 9/10/11 RLE, 8/15/16/24/32-bit).
// ---------------------------------------------------------------------------
bool IsLikelyTga(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 18 || bytes[1] > 1) {
    return false;
  }
  switch (bytes[2]) {
    case 1:
    case 2:
    case 3:
    case 9:
    case 10:
    case 11:
      break;
    default:
      return false;
  }
  const std::uint32_t w = ReadU16LE(bytes.data() + 12);
  const std::uint32_t h = ReadU16LE(bytes.data() + 14);
  if (w == 0 || h == 0 || w > 16384 || h > 16384) {
    return false;
  }
  switch (bytes[16]) {
    case 8:
    case 15:
    case 16:
    case 24:
    case 32:
      return true;
    default:
      return false;
  }
}

struct PixelSource {
  const std::vector<std::uint8_t>& data;
  const std::vector<std::array<std::uint8_t, 4>>& palette;
  const bool use_palette;
  const std::uint8_t depth;
  std::size_t pos = 0;

  bool ReadPixel(std::uint8_t (&rgba)[4]) {
    if (use_palette) {
      if (pos >= data.size() || data[pos] >= palette.size()) {
        return false;
      }
      const std::array<std::uint8_t, 4>& p = palette[data[pos]];
      std::memcpy(rgba, p.data(), 4);
      ++pos;
      return true;
    }
    switch (depth) {
      case 8: {
        if (pos >= data.size()) {
          return false;
        }
        const std::uint8_t v = data[pos++];
        rgba[0] = rgba[1] = rgba[2] = v;
        rgba[3] = 255;
        return true;
      }
      case 15:
      case 16: {
        if (pos + 2 > data.size()) {
          return false;
        }
        const std::uint32_t v = ReadU16LE(data.data() + pos);
        pos += 2;
        rgba[0] = static_cast<std::uint8_t>(((v >> 10) & 0x1Fu) * 255 / 31);
        rgba[1] = static_cast<std::uint8_t>(((v >> 5) & 0x1Fu) * 255 / 31);
        rgba[2] = static_cast<std::uint8_t>((v & 0x1Fu) * 255 / 31);
        rgba[3] = (depth == 16) ? ((v & 0x8000u) ? 255 : 0) : 255;
        return true;
      }
      case 24: {
        if (pos + 3 > data.size()) {
          return false;
        }
        rgba[0] = data[pos + 2];
        rgba[1] = data[pos + 1];
        rgba[2] = data[pos + 0];
        rgba[3] = 255;
        pos += 3;
        return true;
      }
      case 32: {
        if (pos + 4 > data.size()) {
          return false;
        }
        rgba[0] = data[pos + 2];
        rgba[1] = data[pos + 1];
        rgba[2] = data[pos + 0];
        rgba[3] = data[pos + 3];
        pos += 4;
        return true;
      }
      default:
        return false;
    }
  }
};

bool DecodeTga(const std::vector<std::uint8_t>& bytes, ImageTexture& out,
               std::string& error) {
  if (!IsLikelyTga(bytes)) {
    error = "Unrecognised TGA header";
    return false;
  }
  const std::uint8_t image_type = bytes[2];
  const std::uint32_t width = ReadU16LE(bytes.data() + 12);
  const std::uint32_t height = ReadU16LE(bytes.data() + 14);
  const std::uint8_t depth = bytes[16];
  const std::uint8_t descriptor = bytes[17];
  const bool use_palette = (image_type == 1 || image_type == 9);
  const bool rle = (image_type == 9 || image_type == 10 || image_type == 11);
  const bool top_down = (descriptor & 0x20u) != 0;
  if (!CheckImageDims(width, height, error)) {
    return false;
  }

  std::size_t pos = 18 + bytes[0];
  std::vector<std::array<std::uint8_t, 4>> palette;
  if (use_palette) {
    const std::uint32_t cm_first = ReadU16LE(bytes.data() + 3);
    const std::uint32_t cm_count = ReadU16LE(bytes.data() + 5);
    if (cm_first + cm_count > 256) {
      error = "TGA palette too large";
      return false;
    }
    const std::uint8_t cm_depth = bytes[7];
    if (cm_depth != 15 && cm_depth != 16 && cm_depth != 24 && cm_depth != 32) {
      error = "Unsupported TGA palette depth";
      return false;
    }
    palette.resize(cm_first + cm_count, std::array<std::uint8_t, 4>{0, 0, 0, 255});
    for (std::uint32_t i = 0; i < cm_count; ++i) {
      std::array<std::uint8_t, 4>& p = palette[cm_first + i];
      if (cm_depth == 24) {
        if (pos + 3 > bytes.size()) {
          error = "Truncated TGA palette";
          return false;
        }
        p[0] = bytes[pos + 2];
        p[1] = bytes[pos + 1];
        p[2] = bytes[pos + 0];
        p[3] = 255;
        pos += 3;
      } else if (cm_depth == 15 || cm_depth == 16) {
        if (pos + 2 > bytes.size()) {
          error = "Truncated TGA palette";
          return false;
        }
        const std::uint32_t v = ReadU16LE(bytes.data() + pos);
        pos += 2;
        p[0] = static_cast<std::uint8_t>(((v >> 10) & 0x1Fu) * 255 / 31);
        p[1] = static_cast<std::uint8_t>(((v >> 5) & 0x1Fu) * 255 / 31);
        p[2] = static_cast<std::uint8_t>((v & 0x1Fu) * 255 / 31);
        p[3] = 255;
      } else {  // 32
        if (pos + 4 > bytes.size()) {
          error = "Truncated TGA palette";
          return false;
        }
        p[0] = bytes[pos + 2];
        p[1] = bytes[pos + 1];
        p[2] = bytes[pos + 0];
        p[3] = bytes[pos + 3];
        pos += 4;
      }
    }
  }

  out.width = width;
  out.height = height;
  out.rgba8.assign(static_cast<std::size_t>(width) * height * 4, 0);

  PixelSource source{bytes, palette, use_palette, depth, pos};
  const std::uint64_t pixel_count = static_cast<std::uint64_t>(width) * height;
  std::uint64_t written = 0;

  auto put = [&](const std::uint8_t (&rgba)[4]) -> bool {
    if (written >= pixel_count) {
      return false;
    }
    std::uint32_t row = static_cast<std::uint32_t>(written / width);
    const std::uint32_t col = static_cast<std::uint32_t>(written % width);
    if (!top_down) {
      row = height - 1 - row;
    }
    ++written;
    std::uint8_t* dst =
        out.rgba8.data() + (static_cast<std::uint64_t>(row) * width + col) * 4;
    std::memcpy(dst, rgba, 4);
    return true;
  };

  while (written < pixel_count) {
    if (source.pos >= bytes.size()) {
      error = "Truncated TGA pixel data";
      return false;
    }
    const std::uint8_t packet = bytes[source.pos++];
    const std::uint32_t run = static_cast<std::uint32_t>(packet & 0x7Fu) + 1;
    if (rle && (packet & 0x80u)) {
      std::uint8_t rgba[4];
      if (!source.ReadPixel(rgba)) {
        error = "Truncated TGA RLE pixel";
        return false;
      }
      for (std::uint32_t k = 0; k < run; ++k) {
        if (!put(rgba)) {
          error = "TGA pixel data overrun";
          return false;
        }
      }
    } else {
      for (std::uint32_t k = 0; k < run; ++k) {
        std::uint8_t rgba[4];
        if (!source.ReadPixel(rgba)) {
          error = "Truncated TGA pixel data";
          return false;
        }
        if (!put(rgba)) {
          error = "TGA pixel data overrun";
          return false;
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// BMP decoder.
// ---------------------------------------------------------------------------
bool DecodeBmp(const std::vector<std::uint8_t>& bytes, ImageTexture& out,
               std::string& error) {
  if (bytes.size() < 26 || bytes[0] != 'B' || bytes[1] != 'M') {
    error = "Not a BMP file";
    return false;
  }
  const std::uint32_t pixel_offset = ReadU32LE(bytes.data() + 10);
  const std::uint32_t dib_size = ReadU32LE(bytes.data() + 14);

  std::int32_t width = 0;
  std::int32_t height = 0;
  std::uint16_t bpp = 0;
  std::uint32_t compression = 0;
  std::uint32_t colors_used = 0;
  std::uint32_t palette_bytes = 0;
  const std::uint8_t* dib = bytes.data() + 14;

  if (dib_size == 12) {  // BITMAPCOREHEADER
    if (bytes.size() < 26) {
      error = "Truncated BMP header";
      return false;
    }
    width = static_cast<std::int32_t>(ReadU16LE(dib + 4));
    height = static_cast<std::int32_t>(ReadU16LE(dib + 6));
    bpp = static_cast<std::uint16_t>(ReadU16LE(dib + 10));
    palette_bytes = 3;
  } else if (dib_size == 40 || dib_size == 108 || dib_size == 124) {
    if (bytes.size() < 14 + dib_size) {
      error = "Truncated BMP header";
      return false;
    }
    width = ReadI32LE(dib + 4);
    height = ReadI32LE(dib + 8);
    bpp = static_cast<std::uint16_t>(ReadU16LE(dib + 14));
    compression = ReadU32LE(dib + 16);
    colors_used = ReadU32LE(dib + 32);
    palette_bytes = 4;
  } else {
    error = "Unsupported BMP DIB header size";
    return false;
  }

  if (compression != 0 && compression != 3) {
    error = "Compressed BMPs are not supported";
    return false;
  }
  if (width <= 0 || height == 0 || width > 32768 ||
      AbsI32(height) > 32768) {
    error = "Invalid BMP dimensions";
    return false;
  }
  const bool top_down = height < 0;
  const std::uint32_t h = static_cast<std::uint32_t>(AbsI32(height));
  const std::uint32_t w = static_cast<std::uint32_t>(width);
  if (!CheckImageDims(w, h, error)) {
    return false;
  }

  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
    error = "Unsupported BMP bit depth " + std::to_string(bpp);
    return false;
  }
  if (dib_size + 14u > pixel_offset || pixel_offset >= bytes.size()) {
    error = "BMP pixel data offset is invalid";
    return false;
  }

  std::vector<std::array<std::uint8_t, 4>> palette;
  if (bpp <= 8) {
    const std::uint32_t wanted = (bpp == 8) ? 256u : (1u << bpp);
    const std::uint32_t count =
        (colors_used != 0 && colors_used < wanted) ? colors_used : wanted;
    if (count == 0) {
      error = "BMP has no colour palette";
      return false;
    }
    palette.resize(count, std::array<std::uint8_t, 4>{0, 0, 0, 255});
    const std::uint32_t pb = 14 + dib_size;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t at = pb + i * palette_bytes;
      if (at + 3 > bytes.size()) {
        break;
      }
      std::array<std::uint8_t, 4>& p = palette[i];
      p[0] = bytes[at + 2];
      p[1] = bytes[at + 1];
      p[2] = bytes[at + 0];
      p[3] = (palette_bytes == 4) ? bytes[at + 3] : 255;
    }
  }

  const std::uint64_t row_stride =
      (static_cast<std::uint64_t>(w) * bpp + 31u) / 32u * 4u;
  const std::uint64_t bytes_needed =
      static_cast<std::uint64_t>(pixel_offset) +
      static_cast<std::uint64_t>(h) * row_stride;
  if (bytes_needed > bytes.size()) {
    error = "Truncated BMP pixel data";
    return false;
  }

  out.width = w;
  out.height = h;
  out.rgba8.assign(static_cast<std::size_t>(w) * h * 4, 0);

  bool saw_opaque_alpha = false;
  for (std::uint32_t r = 0; r < h; ++r) {
    const std::uint32_t src_row = top_down ? r : (h - 1 - r);
    const std::uint8_t* src =
        bytes.data() + pixel_offset + static_cast<std::uint64_t>(src_row) * row_stride;
    std::uint8_t* dst = out.rgba8.data() + static_cast<std::uint64_t>(r) * w * 4;
    for (std::uint32_t c = 0; c < w; ++c) {
      std::uint8_t* px = dst + static_cast<std::uint64_t>(c) * 4;
      switch (bpp) {
        case 1:
        case 4: {
          const std::uint32_t bits = (bpp == 1) ? 1u : 4u;
          const std::uint64_t bit_index = static_cast<std::uint64_t>(c) * bits;
          const std::uint32_t idx =
              (src[bit_index / 8] >> (8 - bits - (bit_index % 8))) &
              ((1u << bits) - 1u);
          const std::array<std::uint8_t, 4>& p = palette[std::min(
              idx, static_cast<std::uint32_t>(palette.size() - 1))];
          std::memcpy(px, p.data(), 4);
          break;
        }
        case 8: {
          const std::array<std::uint8_t, 4>& p = palette[src[c]];
          std::memcpy(px, p.data(), 4);
          break;
        }
        case 16: {
          const std::uint32_t v = ReadU16LE(src + c * 2);
          px[0] = static_cast<std::uint8_t>(((v >> 10) & 0x1Fu) * 255 / 31);
          px[1] = static_cast<std::uint8_t>(((v >> 5) & 0x1Fu) * 255 / 31);
          px[2] = static_cast<std::uint8_t>((v & 0x1Fu) * 255 / 31);
          px[3] = 255;
          break;
        }
        case 24: {
          px[0] = src[c * 3 + 2];
          px[1] = src[c * 3 + 1];
          px[2] = src[c * 3 + 0];
          px[3] = 255;
          break;
        }
        case 32: {
          px[0] = src[c * 4 + 2];
          px[1] = src[c * 4 + 1];
          px[2] = src[c * 4 + 0];
          px[3] = src[c * 4 + 3];
          if (px[3] != 0) {
            saw_opaque_alpha = true;
          }
          break;
        }
        default:
          break;
      }
    }
  }

  // Windows frequently writes 32-bit BMPs with an alpha channel left at 0
  // even though the colour data is opaque. Treat a fully-zero alpha as opaque.
  if (bpp == 32 && !saw_opaque_alpha) {
    for (std::size_t i = 0; i < out.rgba8.size(); i += 4) {
      out.rgba8[i + 3] = 255;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// DDS decoder: BC1 (DXT1), BC2 (DXT2/3), BC3 (DXT4/5), BC4, BC5 and raw RGB.
// ---------------------------------------------------------------------------
inline void Rgb565(const std::uint8_t* p, std::uint8_t (&rgb)[3]) {
  const std::uint32_t v = p[0] | (static_cast<std::uint32_t>(p[1]) << 8);
  rgb[0] = static_cast<std::uint8_t>(((v >> 11) & 0x1Fu) * 255 / 31);
  rgb[1] = static_cast<std::uint8_t>(((v >> 5) & 0x3Fu) * 255 / 63);
  rgb[2] = static_cast<std::uint8_t>((v & 0x1Fu) * 255 / 31);
}

// Builds the four-entry BC1 colour palette. The 4th entry becomes transparent
// in 1-bit-alpha mode.
inline void Bc1Palette(const std::uint8_t* p, std::uint8_t (&pal)[4][4]) {
  const std::uint16_t c0 = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
  const std::uint16_t c1 = static_cast<std::uint16_t>(p[2] | (p[3] << 8));
  std::uint8_t a[3];
  std::uint8_t b[3];
  Rgb565(p, a);
  Rgb565(p + 2, b);
  pal[0][0] = a[0];
  pal[0][1] = a[1];
  pal[0][2] = a[2];
  pal[0][3] = 255;
  pal[1][0] = b[0];
  pal[1][1] = b[1];
  pal[1][2] = b[2];
  pal[1][3] = 255;
  if (c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = static_cast<std::uint8_t>((2 * a[i] + b[i]) / 3);
      pal[3][i] = static_cast<std::uint8_t>((a[i] + 2 * b[i]) / 3);
    }
    pal[2][3] = 255;
    pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = static_cast<std::uint8_t>((a[i] + b[i]) / 2);
    }
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = 0;
    pal[3][3] = 0;
  }
}

inline std::uint8_t Bc1Index(const std::uint8_t* bits, std::uint32_t t) {
  const std::uint32_t v = ReadU32LE(bits);
  return static_cast<std::uint8_t>((v >> (30 - t % 16 * 2)) & 3u);
}

inline std::uint8_t Bc2Alpha(const std::uint8_t* bits, std::uint32_t t) {
  const std::uint64_t v = static_cast<std::uint64_t>(ReadU32LE(bits)) |
                          (static_cast<std::uint64_t>(ReadU32LE(bits + 4)) << 32);
  const std::uint32_t nib = static_cast<std::uint32_t>((v >> (60 - (t % 16) * 4)) & 0xFu);
  return static_cast<std::uint8_t>(nib * 17);
}

inline std::uint8_t Bc3Alpha(const std::uint8_t* bits, std::uint32_t t) {
  const std::uint8_t a0 = bits[0];
  const std::uint8_t a1 = bits[1];
  const std::uint64_t v =
      static_cast<std::uint64_t>(bits[2]) |
      (static_cast<std::uint64_t>(bits[3]) << 8) |
      (static_cast<std::uint64_t>(bits[4]) << 16) |
      (static_cast<std::uint64_t>(bits[5]) << 24) |
      (static_cast<std::uint64_t>(bits[6]) << 32) |
      (static_cast<std::uint64_t>(bits[7]) << 40);
  const std::uint32_t idx = static_cast<std::uint32_t>((v >> (45 - (t % 16) * 3)) & 7u);
  if (a0 > a1) {
    return static_cast<std::uint8_t>((a0 * (7 - idx) + a1 * idx) / 7);
  }
  switch (idx) {
    case 6:
      return 0;
    case 7:
      return 255;
    default:
      return static_cast<std::uint8_t>((a0 * (5 - idx) + a1 * idx) / 5);
  }
}

enum class BcFormat {
  kInvalid,
  kBc1,   // DXT1
  kBc2,   // DXT2/DXT3, 4-bit alpha nibbles
  kBc3,   // DXT4/DXT5, interpolated 3-bit alpha
  kBc4,   // single channel (R)
  kBc5,   // two channels (R + G)
};

BcFormat BcFormatForFourCC(std::uint32_t fourcc) {
  switch (fourcc) {
    case FourCC('D', 'X', 'T', '1'):
      return BcFormat::kBc1;
    case FourCC('D', 'X', 'T', '2'):
    case FourCC('D', 'X', 'T', '3'):
      return BcFormat::kBc2;
    case FourCC('D', 'X', 'T', '4'):
    case FourCC('D', 'X', 'T', '5'):
      return BcFormat::kBc3;
    case FourCC('B', 'C', '4', 'U'):
    case FourCC('A', 'T', 'I', '1'):
      return BcFormat::kBc4;
    case FourCC('B', 'C', '5', 'U'):
    case FourCC('A', 'T', 'I', '2'):
      return BcFormat::kBc5;
    default:
      return BcFormat::kInvalid;
  }
}

bool DdsDecodeBc(const std::uint8_t* data, std::size_t data_size,
                 std::size_t header_size, std::uint32_t width,
                 std::uint32_t height, BcFormat format, ImageTexture& out,
                 std::string& error) {
  if (!CheckImageDims(width, height, error)) {
    return false;
  }
  const std::uint32_t bw = (width + 3) / 4;
  const std::uint32_t bh = (height + 3) / 4;
  const std::uint32_t block_bytes =
      (format == BcFormat::kBc1 || format == BcFormat::kBc4) ? 8u : 16u;
  const std::uint64_t total = static_cast<std::uint64_t>(bw) * bh * block_bytes;
  if (header_size + total > data_size) {
    error = "Truncated DDS pixel data";
    return false;
  }

  out.width = width;
  out.height = height;
  out.rgba8.assign(static_cast<std::size_t>(width) * height * 4, 0);

  std::size_t src = header_size;
  for (std::uint32_t by = 0; by < bh; ++by) {
    for (std::uint32_t bx = 0; bx < bw; ++bx) {
      const std::uint8_t* color = data + src;
      if (format != BcFormat::kBc1) {
        color += 8;
      }
      std::uint8_t pal[4][4];
      if (format == BcFormat::kBc1 || format == BcFormat::kBc2 ||
          format == BcFormat::kBc3) {
        Bc1Palette(color, pal);
      }
      for (std::uint32_t ty = 0; ty < 4; ++ty) {
        for (std::uint32_t tx = 0; tx < 4; ++tx) {
          const std::uint32_t px = bx * 4 + tx;
          const std::uint32_t py = by * 4 + ty;
          const std::uint32_t t = ty * 4 + tx;
          if (px >= width || py >= height) {
            continue;
          }
          std::uint8_t* dst =
              out.rgba8.data() + (static_cast<std::uint64_t>(py) * width + px) * 4;
          switch (format) {
            case BcFormat::kBc1: {
              const std::uint8_t idx = Bc1Index(data + src + 4, t);
              std::memcpy(dst, pal[idx], 4);
              break;
            }
            case BcFormat::kBc2: {
              const std::uint8_t idx = Bc1Index(data + src + 12, t);
              std::memcpy(dst, pal[idx], 4);
              dst[3] = Bc2Alpha(data + src, t);
              break;
            }
            case BcFormat::kBc3: {
              const std::uint8_t idx = Bc1Index(data + src + 12, t);
              std::memcpy(dst, pal[idx], 4);
              dst[3] = Bc3Alpha(data + src, t);
              break;
            }
            case BcFormat::kBc4: {
              const std::uint8_t v = Bc3Alpha(data + src, t);
              dst[0] = v;
              dst[1] = v;
              dst[2] = v;
              dst[3] = 255;
              break;
            }
            case BcFormat::kBc5: {
              const std::uint8_t r = Bc3Alpha(data + src, t);
              const std::uint8_t g = Bc3Alpha(data + src + 8, t);
              dst[0] = r;
              dst[1] = g;
              dst[2] = 0;
              dst[3] = 255;
              break;
            }
            default:
              break;
          }
        }
      }
      src += block_bytes;
    }
  }
  return true;
}

// Uncompressed DDS: channel content comes from the pixel format masks.
inline void RemapBits(std::uint32_t value, std::uint32_t mask,
                      std::uint8_t& out_byte) {
  if (mask == 0) {
    out_byte = 0;
    return;
  }
  unsigned shift = 0;
  std::uint32_t m = mask;
  while (!(m & 1u)) {
    m >>= 1;
    ++shift;
  }
  const unsigned width_bits = PopCount(mask);
  const std::uint32_t extracted = (value & mask) >> shift;
  if (width_bits >= 8) {
    out_byte = static_cast<std::uint8_t>(extracted >> (width_bits - 8));
  } else {
    out_byte = static_cast<std::uint8_t>((extracted * 255) /
                                         ((1u << width_bits) - 1u));
  }
}

// Uncompressed DDS. `layout` picks how channels are interpreted: straight
// channel masks from the legacy DDS_PIXELFORMAT (0), or a fixed byte order
// from a DX10 header which carries no masks (1 = R8G8B8A8, 2 = B8G8R8A8).
enum class RawLayout {
  kMasks,
  kRgba8,
  kBgra8,
};

bool DecodeDdsRaw(const std::uint8_t* data, std::size_t data_size,
                  std::size_t header_size, std::uint32_t width,
                  std::uint32_t height, std::uint32_t bit_count,
                  RawLayout layout, ImageTexture& out, std::string& error) {
  if (!CheckImageDims(width, height, error)) {
    return false;
  }
  if (bit_count != 8 && bit_count != 16 && bit_count != 24 && bit_count != 32) {
    error = "Unsupported DDS raw bit depth";
    return false;
  }
  if (layout != RawLayout::kMasks && bit_count != 32) {
    error = "Unsupported DX10 raw format";
    return false;
  }
  const std::uint32_t flags = ReadU32LE(data + 80);
  const bool luminance =
      (flags & 0x00020000u) != 0 && (flags & 0x40u) == 0;

  const std::uint32_t rmask = ReadU32LE(data + 92);
  const std::uint32_t gmask = ReadU32LE(data + 96);
  const std::uint32_t bmask = ReadU32LE(data + 100);
  const std::uint32_t amask = ReadU32LE(data + 104);
  const std::uint64_t row_stride =
      static_cast<std::uint64_t>(width) * (bit_count / 8);
  if (static_cast<std::uint64_t>(header_size) +
          static_cast<std::uint64_t>(height) * row_stride >
      data_size) {
    error = "Truncated DDS pixel data";
    return false;
  }

  out.width = width;
  out.height = height;
  out.rgba8.assign(static_cast<std::size_t>(width) * height * 4, 0);

  for (std::uint32_t r = 0; r < height; ++r) {
    const std::uint8_t* src =
        data + header_size + static_cast<std::uint64_t>(r) * row_stride;
    std::uint8_t* dst =
        out.rgba8.data() + static_cast<std::uint64_t>(r) * width * 4;
    for (std::uint32_t c = 0; c < width; ++c) {
      std::uint8_t* px = dst + static_cast<std::uint64_t>(c) * 4;
      if (layout != RawLayout::kMasks) {
        // Fixed 32-bit byte order (DXGI).
        px[0] = src[c * 4 + (layout == RawLayout::kRgba8 ? 0 : 2)];
        px[1] = src[c * 4 + 1];
        px[2] = src[c * 4 + (layout == RawLayout::kRgba8 ? 2 : 0)];
        px[3] = src[c * 4 + 3];
        if (px[3] == 0) {
          px[3] = 255;  // DXGI alpha expects premultiplied data; keep opaque text legible
        }
        continue;
      }
      std::uint32_t v = 0;
      if (bit_count == 8) {
        v = src[c];
      } else if (bit_count == 16) {
        v = ReadU16LE(src + c * 2);
      } else if (bit_count == 24) {
        v = static_cast<std::uint32_t>(src[c * 3 + 0]) |
            (static_cast<std::uint32_t>(src[c * 3 + 1]) << 8) |
            (static_cast<std::uint32_t>(src[c * 3 + 2]) << 16);
      } else {
        v = ReadU32LE(src + c * 4);
      }
      if (luminance) {
        RemapBits(v, rmask != 0 ? rmask : 0xFFu, px[0]);
        px[1] = px[2] = px[0];
        px[3] = 255;
      } else {
        RemapBits(v, rmask, px[0]);
        RemapBits(v, gmask, px[1]);
        RemapBits(v, bmask, px[2]);
        if (amask != 0) {
          RemapBits(v, amask, px[3]);
        } else {
          px[3] = 255;
        }
      }
    }
  }
  return true;
}

bool DecodeDds(const std::vector<std::uint8_t>& bytes, ImageTexture& out,
               std::string& error) {
  if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
    error = "Not a DDS file";
    return false;
  }
  const std::uint32_t width = ReadU32LE(bytes.data() + 16);
  const std::uint32_t height = ReadU32LE(bytes.data() + 12);
  const std::uint32_t fourcc = ReadU32LE(bytes.data() + 84);
  const std::uint32_t rgb_bit_count = ReadU32LE(bytes.data() + 88);

  if (!CheckImageDims(width, height, error)) {
    return false;
  }

  if (fourcc == FourCC('D', 'X', '1', '0')) {
    // DX10 extended header.
    if (bytes.size() < 148) {
      error = "Truncated DDS DX10 header";
      return false;
    }
    const std::uint32_t dxgi = ReadU32LE(bytes.data() + 128);
    switch (dxgi) {
      case 71:   // BC1_UNORM
      case 72:   // BC1_UNORM_SRGB
        return DdsDecodeBc(bytes.data(), bytes.size(), 148, width, height,
                           BcFormat::kBc1, out, error);
      case 74:   // BC2_UNORM
      case 75:
        return DdsDecodeBc(bytes.data(), bytes.size(), 148, width, height,
                           BcFormat::kBc2, out, error);
      case 77:   // BC3_UNORM
      case 78:
        return DdsDecodeBc(bytes.data(), bytes.size(), 148, width, height,
                           BcFormat::kBc3, out, error);
      case 80:   // BC4_UNORM
      case 81:
        return DdsDecodeBc(bytes.data(), bytes.size(), 148, width, height,
                           BcFormat::kBc4, out, error);
      case 83:   // BC5_UNORM
      case 84:
        return DdsDecodeBc(bytes.data(), bytes.size(), 148, width, height,
                           BcFormat::kBc5, out, error);
      case 28:   // R8G8B8A8_UNORM
        return DecodeDdsRaw(bytes.data(), bytes.size(), 148, width, height, 32,
                            RawLayout::kRgba8, out, error);
      case 87:   // B8G8R8A8_UNORM
      case 91:   // sRGB variant
        return DecodeDdsRaw(bytes.data(), bytes.size(), 148, width, height, 32,
                            RawLayout::kBgra8, out, error);
      case 95:   // BC6H_UF16
      case 96:   // BC6H_SF16
      case 98:   // BC7_UNORM
      case 99:   // BC7_UNORM_SRGB
        error = "BC6H/BC7 DDS textures are not supported yet";
        return false;
      default:
        error = "Unsupported DDS DXGI format " + std::to_string(dxgi);
        return false;
    }
  }

  const BcFormat bc = BcFormatForFourCC(fourcc);
  if (bc != BcFormat::kInvalid) {
    return DdsDecodeBc(bytes.data(), bytes.size(), 128, width, height, bc, out,
                       error);
  }
  if (fourcc != 0) {
    error = "Unsupported DDS compression";
    return false;
  }
  return DecodeDdsRaw(bytes.data(), bytes.size(), 128, width, height,
                      rgb_bit_count, RawLayout::kMasks, out, error);
}

// ---------------------------------------------------------------------------
// WIC-backed decode for PNG/JPEG/WebP (and BMP) on Windows.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
bool DecodeViaWic(const std::vector<std::uint8_t>& bytes, ImageTexture& out,
                  std::string& error) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool initialized = (hr == S_OK || hr == S_FALSE);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    error = "Could not initialise COM for image decoding";
    return false;
  }

  struct ComUninit {
    bool on;
    ~ComUninit() {
      if (on) {
        CoUninitialize();
      }
    }
  } cleanup{initialized};

  IWICImagingFactory* factory = nullptr;
  hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                        CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                        reinterpret_cast<void**>(&factory));
  if (FAILED(hr)) {
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, reinterpret_cast<void**>(&factory));
  }
  if (FAILED(hr) || factory == nullptr) {
    error = "WIC imaging factory is unavailable";
    return false;
  }

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || stream == nullptr) {
    factory->Release();
    error = "WIC stream creation failed";
    return false;
  }
  hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                    static_cast<DWORD>(bytes.size()));
  if (FAILED(hr)) {
    stream->Release();
    factory->Release();
    error = "WIC stream initialisation failed";
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(
      stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr) || decoder == nullptr) {
    stream->Release();
    factory->Release();
    error = "Could not open image with WIC";
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr) || frame == nullptr) {
    decoder->Release();
    stream->Release();
    factory->Release();
    error = "Image has no readable frame";
    return false;
  }

  IWICFormatConverter* converter = nullptr;
  hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr) || converter == nullptr) {
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    error = "WIC format converter is unavailable";
    return false;
  }
  hr = converter->Initialize(frame, &GUID_WICPixelFormat32bppRGBA,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    converter->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    error = "WIC could not convert the image pixels";
    return false;
  }

  UINT w = 0;
  UINT h = 0;
  converter->GetSize(&w, &h);
  if (!CheckImageDims(w, h, error)) {
    converter->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    return false;
  }
  out.width = w;
  out.height = h;
  out.rgba8.assign(static_cast<std::size_t>(w) * h * 4, 0);
  hr = converter->CopyPixels(nullptr, w * 4,
                             static_cast<UINT>(out.rgba8.size()), out.rgba8.data());
  if (FAILED(hr)) {
    converter->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    out = ImageTexture{};
    error = "WIC pixel copy failed";
    return false;
  }

  converter->Release();
  frame->Release();
  decoder->Release();
  stream->Release();
  factory->Release();
  return true;
}
#else
bool DecodeViaWic(const std::vector<std::uint8_t>& /*bytes*/, ImageTexture& /*out*/,
                  std::string& error) {
  error =
      "PNG/JPEG/WebP decoding on this build requires the Windows imaging stack; "
      "TGA/DDS/BMP supported here";
  return false;
}
#endif

// ---------------------------------------------------------------------------
// Format string helpers used by the manifest.
// ---------------------------------------------------------------------------
const char* ExtensionForFormat(ImageFormat format) {
  switch (format) {
    case ImageFormat::kPng:
      return "png";
    case ImageFormat::kJpeg:
      return "jpg";
    case ImageFormat::kBmp:
      return "bmp";
    case ImageFormat::kTga:
      return "tga";
    case ImageFormat::kDds:
      return "dds";
    case ImageFormat::kWebp:
      return "webp";
    default:
      return "";
  }
}

std::string MakeSlug(const std::string& raw) {
  std::string s;
  s.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c < 32u || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
        c == '\\' || c == '|' || c == '?' || c == '*') {
      s += '_';
    } else {
      s += static_cast<char>(c);
    }
  }
  while (!s.empty() && s.front() == '.') {
    s.erase(s.begin());
  }
  if (s.empty()) {
    s = "texture";
  }
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Part catalog.
// ---------------------------------------------------------------------------
const std::vector<PartInfo>& PartCatalog() {
  static const std::vector<PartInfo> kCatalog = {
      {Part::kHead, "head", "Head"},
      {Part::kFace, "face", "Face"},
      {Part::kEyes, "eyes", "Eyes"},
      {Part::kHair, "hair", "Hair"},
      {Part::kFacialHair, "facial_hair", "Facial Hair"},
      {Part::kShirt, "shirt", "Shirt"},
      {Part::kJacket, "jacket", "Jacket"},
      {Part::kGloves, "gloves", "Gloves"},
      {Part::kPants, "pants", "Pants"},
      {Part::kBelt, "belt", "Belt"},
      {Part::kSocks, "socks", "Socks"},
      {Part::kShoes, "shoes", "Shoes"},
      {Part::kHat, "hat", "Hat"},
      {Part::kGlasses, "glasses", "Glasses"},
      {Part::kEarrings, "earrings", "Earrings"},
      {Part::kBoard, "board", "Skateboard"},
      {Part::kAccessories, "accessories", "Accessories"},
  };
  return kCatalog;
}

const char* PartId(Part part) {
  for (const PartInfo& info : PartCatalog()) {
    if (info.part == part) {
      return info.id;
    }
  }
  return "unknown";
}

const char* PartLabel(Part part) {
  for (const PartInfo& info : PartCatalog()) {
    if (info.part == part) {
      return info.label;
    }
  }
  return "Unknown";
}

const char* ImageFormatLabel(ImageFormat format) {
  switch (format) {
    case ImageFormat::kPng:
      return "PNG";
    case ImageFormat::kJpeg:
      return "JPEG";
    case ImageFormat::kBmp:
      return "BMP";
    case ImageFormat::kTga:
      return "TGA";
    case ImageFormat::kDds:
      return "DDS";
    case ImageFormat::kWebp:
      return "WebP";
    default:
      return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// Format detection and dispatch.
// ---------------------------------------------------------------------------
bool DetectImageFormat(const std::vector<std::uint8_t>& bytes,
                       ImageFormat& output, std::string& error) {
  if (bytes.size() >= 8 &&
      std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
    output = ImageFormat::kPng;
    return true;
  }
  if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
      bytes[2] == 0xFF) {
    output = ImageFormat::kJpeg;
    return true;
  }
  if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
    output = ImageFormat::kBmp;
    return true;
  }
  if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
      std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
    output = ImageFormat::kWebp;
    return true;
  }
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
    output = ImageFormat::kDds;
    return true;
  }
  if (IsLikelyTga(bytes)) {
    output = ImageFormat::kTga;
    return true;
  }
  error = "Unrecognised image format (expected PNG, JPEG, WebP, TGA, DDS or BMP)";
  return false;
}

bool DecodeImageToRgba8(const std::vector<std::uint8_t>& bytes,
                        ImageFormat format, ImageTexture& output,
                        std::string& error) {
  switch (format) {
    case ImageFormat::kPng:
    case ImageFormat::kJpeg:
    case ImageFormat::kWebp:
      return DecodeViaWic(bytes, output, error);
    case ImageFormat::kBmp:
      return DecodeBmp(bytes, output, error);
    case ImageFormat::kTga:
      return DecodeTga(bytes, output, error);
    case ImageFormat::kDds:
      return DecodeDds(bytes, output, error);
    default:
      error = "Unrecognised image format";
      return false;
  }
}

bool DecodeImageFileToRgba8(const std::filesystem::path& path,
                            ImageTexture& output, ImageFormat* format_out,
                            std::string& error) {
  std::vector<std::uint8_t> bytes;
  if (!ReadFileBytes(path, bytes, error)) {
    return false;
  }
  ImageFormat format = ImageFormat::kUnknown;
  if (!DetectImageFormat(bytes, format, error)) {
    // Fall back to the file extension before giving up (TGA/DDS have no
    // airtight magic).
    const std::string ext = path.extension().string();
    if (ext == ".tga" || ext == ".TGA") {
      format = ImageFormat::kTga;
    } else if (ext == ".dds" || ext == ".DDS") {
      format = ImageFormat::kDds;
    } else {
      return false;
    }
  }
  if (!DecodeImageToRgba8(bytes, format, output, error)) {
    return false;
  }
  if (format_out != nullptr) {
    *format_out = format;
  }
  return true;
}

const char* SupportedImageExtensions() {
  return "png,jpg,jpeg,webp,tga,dds,bmp";
}

// ---------------------------------------------------------------------------
// Library.
// ---------------------------------------------------------------------------
std::filesystem::path CustomTexturesDirectory(
    const std::filesystem::path& user_data_root) {
  return user_data_root / "custom_textures";
}

Library::Library(std::filesystem::path user_data_root)
    : directory_(CustomTexturesDirectory(user_data_root)),
      manifest_path_(directory_ / "presets.json") {}

bool Library::Load(std::string& error) {
  presets_.clear();
  applied_.clear();
  next_id_ = 1;

  std::error_code ec;
  if (!std::filesystem::exists(manifest_path_, ec) || ec) {
    return true;  // First run: nothing to load.
  }

  std::ifstream in(manifest_path_);
  if (!in) {
    error = "Could not open \"" + manifest_path_.string() + "\"";
    return false;
  }

  nlohmann::json root;
  try {
    in >> root;
  } catch (const std::exception& e) {
    error = std::string("Could not parse preset manifest: ") + e.what();
    return false;
  }

  if (!root.is_object() || root.value("version", 0) != 1) {
    error = "Preset manifest has an unsupported version";
    return false;
  }

  if (root.contains("presets") && root["presets"].is_array()) {
    for (const auto& item : root["presets"]) {
      if (!item.is_object()) {
        continue;
      }
      const std::uint64_t id = item.value("id", 0ull);
      const std::string part_id = item.value("part", "");
      Part part = Part::kHead;
      bool known_part = false;
      for (const PartInfo& info : PartCatalog()) {
        if (part_id == info.id) {
          part = info.part;
          known_part = true;
          break;
        }
      }
      if (!known_part || id == 0) {
        continue;
      }
      TexturePreset preset;
      preset.id = id;
      preset.part = part;
      preset.name = item.value("name", "");
      preset.filename = item.value("file", "");
      if (preset.filename.empty()) {
        continue;
      }
      presets_.push_back(std::move(preset));
      next_id_ = std::max(next_id_, id + 1);
    }
  }

  if (root.contains("applied") && root["applied"].is_object()) {
    for (auto it = root["applied"].begin(); it != root["applied"].end(); ++it) {
      Part part = Part::kHead;
      bool known_part = false;
      for (const PartInfo& info : PartCatalog()) {
        if (it.key() == info.id) {
          part = info.part;
          known_part = true;
          break;
        }
      }
      if (!known_part || !it.value().is_number_unsigned()) {
        continue;
      }
      const std::uint64_t id = it.value().get<std::uint64_t>();
      bool id_known = false;
      for (const TexturePreset& p : presets_) {
        if (p.id == id && p.part == part) {
          id_known = true;
          break;
        }
      }
      if (id_known) {
        applied_[static_cast<std::uint8_t>(part)] = id;
      }
    }
  }
  return true;
}

bool Library::Save(std::string& error) const {
  std::error_code ec;
  if (!std::filesystem::exists(directory_, ec)) {
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
      error = "Could not create \"" + directory_.string() + "\": " + ec.message();
      return false;
    }
  }

  nlohmann::json root;
  root["version"] = 1;
  root["next_id"] = next_id_;
  nlohmann::json presets = nlohmann::json::array();
  for (const TexturePreset& p : presets_) {
    presets.push_back({{"id", p.id},
                       {"part", PartId(p.part)},
                       {"name", p.name},
                       {"file", p.filename}});
  }
  root["presets"] = presets;
  nlohmann::json applied = nlohmann::json::object();
  for (const auto& entry : applied_) {
    applied[PartId(static_cast<Part>(entry.first))] = entry.second;
  }
  root["applied"] = applied;

  const std::filesystem::path tmp = manifest_path_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Could not write \"" + tmp.string() + "\"";
      return false;
    }
    out << root.dump(2);
    out.flush();
    if (!out) {
      error = "Could not write \"" + tmp.string() + "\"";
      return false;
    }
  }
  std::filesystem::rename(tmp, manifest_path_, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    error = "Could not save \"" + manifest_path_.string() + "\": " + ec.message();
    return false;
  }
  return true;
}

std::vector<const TexturePreset*> Library::PresetsForPart(Part part) const {
  std::vector<const TexturePreset*> result;
  result.reserve(presets_.size());
  for (const TexturePreset& p : presets_) {
    if (p.part == part) {
      result.push_back(&p);
    }
  }
  return result;
}

const TexturePreset* Library::Find(std::uint64_t id) const {
  for (const TexturePreset& p : presets_) {
    if (p.id == id) {
      return &p;
    }
  }
  return nullptr;
}

std::filesystem::path Library::PresetImagePath(const TexturePreset& preset) const {
  return directory_ / preset.filename;
}

bool Library::ImportFromFile(const std::filesystem::path& source, Part part,
                             const std::string& name, std::uint64_t* out_id,
                             std::string& error) {
  std::vector<std::uint8_t> bytes;
  if (!ReadFileBytes(source, bytes, error)) {
    return false;
  }
  ImageFormat format = ImageFormat::kUnknown;
  if (!DetectImageFormat(bytes, format, error)) {
    const std::string ext = source.extension().string();
    if (ext == ".tga" || ext == ".TGA") {
      format = ImageFormat::kTga;
    } else if (ext == ".dds" || ext == ".DDS") {
      format = ImageFormat::kDds;
    } else {
      return false;
    }
  }
  ImageTexture probe;
  if (!DecodeImageToRgba8(bytes, format, probe, error)) {
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::exists(directory_, ec)) {
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
      error = "Could not create \"" + directory_.string() + "\": " + ec.message();
      return false;
    }
  }

  TexturePreset preset;
  preset.id = next_id_;
  preset.part = part;
  preset.name = name.empty() ? PartLabel(part) : name;
  const std::string ext = ExtensionForFormat(format);
  preset.filename = std::to_string(preset.id) + "_" +
                    MakeSlug(preset.name) + "." + ext;

  const std::filesystem::path destination = directory_ / preset.filename;
  {
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Could not write \"" + destination.string() + "\"";
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
      error = "Could not write \"" + destination.string() + "\"";
      return false;
    }
  }

  presets_.push_back(preset);
  ++next_id_;
  if (!Save(error)) {
    presets_.pop_back();
    std::filesystem::remove(destination, ec);
    return false;
  }
  if (out_id != nullptr) {
    *out_id = preset.id;
  }
  return true;
}

bool Library::Remove(std::uint64_t id, std::string& error) {
  for (auto it = presets_.begin(); it != presets_.end(); ++it) {
    if (it->id != id) {
      continue;
    }
    const std::filesystem::path file = directory_ / it->filename;
    std::error_code ec;
    std::filesystem::remove(file, ec);
    presets_.erase(it);
    for (auto a = applied_.begin(); a != applied_.end();) {
      if (a->second == id) {
        a = applied_.erase(a);
      } else {
        ++a;
      }
    }
    if (!Save(error)) {
      return false;
    }
    return true;
  }
  error = "Preset not found";
  return false;
}

std::uint64_t Library::AppliedForPart(Part part) const {
  const auto it = applied_.find(static_cast<std::uint8_t>(part));
  return it == applied_.end() ? 0 : it->second;
}

std::filesystem::path Library::AppliedTexturePath(Part part) const {
  const std::uint64_t id = AppliedForPart(part);
  if (id == 0) {
    return {};
  }
  const TexturePreset* preset = Find(id);
  if (preset == nullptr || preset->part != part) {
    return {};
  }
  return PresetImagePath(*preset);
}

bool Library::SetApplied(Part part, std::uint64_t preset_id,
                         std::string& error) {
  const TexturePreset* preset = Find(preset_id);
  if (preset == nullptr || preset->part != part) {
    error = "Cannot apply a texture that is not assigned to this part";
    return false;
  }
  applied_[static_cast<std::uint8_t>(part)] = preset_id;
  if (!Save(error)) {
    applied_.erase(static_cast<std::uint8_t>(part));
    return false;
  }
  return true;
}

bool Library::ClearApplied(Part part, std::string& error) {
  applied_.erase(static_cast<std::uint8_t>(part));
  if (!Save(error)) {
    return false;
  }
  return true;
}

}  // namespace skate3::custom_textures