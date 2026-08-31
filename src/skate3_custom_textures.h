#pragma once

// Custom Skater Textures - user-owned texture library for the skater's model
// parts.
//
// This module is project-owned (not part of the retail game). It lets players
// import their own image files as named texture presets for the browsable
// skater part slots and keeps that library persistent between sessions under
// the user data root (same family as maps/, objects/ and ui_asset_cache/).
//
// The library stores both the original image bytes (so the renderer can serve
// whatever it needs instead of blindly re-encoding) and a small JSON manifest
// that records the preset name, its part slot and the per-part "applied"
// selection. Image reading supports PNG, JPEG/JPG, BMP, TGA, DDS and WebP:
// PNG/JPEG/WebP/BMP decode through the platform image stack on Windows and the
// pure decoders in this module handle TGA and DDS everywhere.
//
// Rendering integration: nothing in this module touches the native scene
// renderer. Consumers that want a custom texture on the skater model query
// CustomTexturesDirectory() / AppliedTexturePath() and substitute the retail
// diffuse for that part. That seam is intentionally kept out of this file so
// the library, UI and render tests can run without a running game.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::custom_textures {

// Browsable skater part slots. The ids are stable manifest values; labels are
// what players see in the Edit Skater section.
enum class Part : std::uint8_t {
  kHead,
  kFace,
  kEyes,
  kHair,
  kFacialHair,
  kShirt,
  kJacket,
  kGloves,
  kPants,
  kBelt,
  kSocks,
  kShoes,
  kHat,
  kGlasses,
  kEarrings,
  kBoard,
  kAccessories,
  kCount,
};

struct PartInfo {
  Part part;
  const char* id;
  const char* label;
};

// Ordered description of every browsable part slot.
const std::vector<PartInfo>& PartCatalog();

const char* PartId(Part part);
const char* PartLabel(Part part);

enum class ImageFormat : std::uint8_t {
  kPng,
  kJpeg,
  kBmp,
  kTga,
  kDds,
  kWebp,
  kUnknown,
};

const char* ImageFormatLabel(ImageFormat format);

// One decoded image, normalised to straight-alpha RGBA8.
struct ImageTexture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba8;  // width * height * 4
};

// Magic-byte based format detection. TGA has no reliable signature and is
// recognised structurally; callers that still disagree may fall back to the
// file extension via DecodeImageFileToRgba8.
bool DetectImageFormat(const std::vector<std::uint8_t>& bytes,
                       ImageFormat& output, std::string& error);

// Decodes bytes in the given format to RGBA8. PNG/JPEG/WebP require the
// Windows imaging stack; TGA/DDS/BMP are decoded in this module and work on
// every platform.
bool DecodeImageToRgba8(const std::vector<std::uint8_t>& bytes,
                        ImageFormat format, ImageTexture& output,
                        std::string& error);

// Reads a file, detects its format (magic first, extension fallback) and
// decodes it to RGBA8.
bool DecodeImageFileToRgba8(const std::filesystem::path& path,
                            ImageTexture& output, ImageFormat* format_out,
                            std::string& error);

// Files the image picker accepts. Order matches the native dialog filter.
const char* SupportedImageExtensions();

struct TexturePreset {
  std::uint64_t id = 0;
  Part part = Part::kHead;
  std::string name;       // player-facing display name
  std::string filename;   // stored file name inside the library directory
};

// Persistent custom-texture library. Constructed around the user data root;
// Load() reads the manifest, every mutation keeps the manifest on disk so the
// library reappears after a restart.
class Library {
 public:
  explicit Library(std::filesystem::path user_data_root);

  const std::filesystem::path& directory() const { return directory_; }
  const std::filesystem::path& manifest_path() const { return manifest_path_; }

  bool Load(std::string& error);
  bool Save(std::string& error) const;

  const std::vector<TexturePreset>& presets() const { return presets_; }
  std::vector<const TexturePreset*> PresetsForPart(Part part) const;
  const TexturePreset* Find(std::uint64_t id) const;

  // Copies the chosen image into the library directory, validates that it
  // decodes, appends a named preset for the part and persists. `out_id`
  // receives the new preset id.
  bool ImportFromFile(const std::filesystem::path& source, Part part,
                      const std::string& name, std::uint64_t* out_id,
                      std::string& error);

  // Removes the preset and its stored image. The applied selection is cleared
  // when it points at the removed preset.
  bool Remove(std::uint64_t id, std::string& error);

  std::filesystem::path PresetImagePath(const TexturePreset& preset) const;

  // Per-part applied selection (the active custom texture for a part). A
  // cleared selection leaves the game's own texture in place. This is the
  // seam the renderer integration reads from.
  std::uint64_t AppliedForPart(Part part) const;
  std::filesystem::path AppliedTexturePath(Part part) const;
  bool SetApplied(Part part, std::uint64_t preset_id, std::string& error);
  bool ClearApplied(Part part, std::string& error);

 private:
  std::filesystem::path directory_;
  std::filesystem::path manifest_path_;
  std::vector<TexturePreset> presets_;
  std::unordered_map<uint8_t, std::uint64_t> applied_;  // Part -> preset id
  std::uint64_t next_id_ = 1;
};

// Where the library lives below a user data root: <root>/custom_textures/.
std::filesystem::path CustomTexturesDirectory(
    const std::filesystem::path& user_data_root);

}  // namespace skate3::custom_textures