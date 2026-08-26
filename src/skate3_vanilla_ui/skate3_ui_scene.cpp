#include "skate3_ui_scene.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <imgui_internal.h>
#include <nlohmann/json.hpp>

namespace skate3::vanilla_ui {
namespace {

using Json = nlohmann::json;

struct RetailTexture {
  std::unique_ptr<ImTextureData> data;
  bool attempted = false;

  ~RetailTexture() {
    if (data && ImGui::GetCurrentContext()) {
      ImGui::UnregisterUserTexture(data.get());
    }
  }

  bool EnsureLoaded(const std::filesystem::path &path, int width, int height,
                    std::string &error) {
    if (data) {
      return true;
    }
    if (attempted) {
      return false;
    }
    attempted = true;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      error = "Unable to open exact UI texture: " + path.string();
      return false;
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    const auto expected = static_cast<std::streamoff>(width) * height * 4;
    if (size != expected) {
      error = "Exact UI texture has an unexpected size: " + path.string();
      return false;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(expected));
    if (!stream.read(reinterpret_cast<char *>(pixels.data()), expected)) {
      error = "Unable to read exact UI texture: " + path.string();
      return false;
    }
    // The extracted APT geometry explicitly uses texture-clamped materials.
    // ImGui's managed user textures are uploaded through the shared repeating
    // sampler, so surround the exact retail pixels with a duplicated one-texel
    // border. UVs are remapped into this border below, reproducing clamp
    // sampling without changing the shared renderer used by the other menus.
    const int padded_width = width + 2;
    const int padded_height = height + 2;
    data = std::make_unique<ImTextureData>();
    data->Create(ImTextureFormat_RGBA32, padded_width, padded_height);
    auto *padded = static_cast<unsigned char *>(data->GetPixels());
    for (int y = 0; y < padded_height; ++y) {
      const int source_y = std::clamp(y - 1, 0, height - 1);
      for (int x = 0; x < padded_width; ++x) {
        const int source_x = std::clamp(x - 1, 0, width - 1);
        std::memcpy(
            padded + (static_cast<std::size_t>(y) * padded_width + x) * 4,
            pixels.data() +
                (static_cast<std::size_t>(source_y) * width + source_x) * 4,
            4);
      }
    }
    ImGui::RegisterUserTexture(data.get());
    return true;
  }
};

struct Vertex {
  ImVec2 position;
  ImVec2 uv;
};

struct Triangle {
  std::array<Vertex, 3> vertices;
};

struct Primitive {
  int draw_order = 0;
  std::string item_key;
  ImU32 color = IM_COL32_WHITE;
  float opacity = 1.0f;
  std::string animation_track;
  std::array<float, 6> matrix{};
  std::string texture_path;
  int texture_width = 0;
  int texture_height = 0;
  std::vector<Triangle> triangles;
};

struct Glyph {
  int texture_index = 0;
  float atlas_left = 0;
  float atlas_top = 0;
  float atlas_right = 0;
  float atlas_bottom = 0;
  float width = 0;
  float height = 0;
  float x_offset = 0;
  float y_offset = 0;
  float x_advance = 0;
};

struct Font {
  std::string texture_path;
  int texture_width = 0;
  int texture_height = 0;
  float ascent = 1.0f;
  std::unordered_map<std::uint32_t, int> characters;
  std::unordered_map<int, Glyph> glyphs;
};

struct TextRun {
  int draw_order = 0;
  std::string item_key;
  std::string animation_track;
  std::array<float, 6> matrix{};
  std::shared_ptr<Font> font;
  float font_height = 0;
  ImU32 color = IM_COL32_WHITE;
  float opacity = 1.0f;
  std::string value;
};

using DrawItem = std::variant<Primitive, TextRun>;

struct AlphaTrack {
  float baseline = 1.0f;
  std::vector<std::uint8_t> values;
};

struct SelectedGlowAnimation {
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  std::chrono::milliseconds frame_duration{16};
  std::size_t frame_count = 0;
  std::map<std::string, AlphaTrack, std::less<>> tracks;
};

struct MotionTrack {
  std::vector<std::array<float, 6>> matrices;
  std::vector<float> opacities;
};

struct MotionClip {
  std::chrono::milliseconds frame_duration{16};
  std::size_t frame_count = 0;
  std::unordered_map<std::string, MotionTrack> tracks;
};

struct MotionPlayback {
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  const MotionClip *clip = nullptr;
  std::string name;
};

struct SceneAnimations {
  SelectedGlowAnimation glow;
  std::map<std::string, MotionClip, std::less<>> motion;
};

ImU32 WithAlpha(ImU32 color, float alpha) {
  const auto source = static_cast<float>((color >> 24) & 0xFF);
  const auto result =
      static_cast<unsigned>(std::clamp(source * alpha, 0.0f, 255.0f));
  return (color & 0x00FFFFFFu) | (result << 24);
}

std::uint32_t ParseArgbValue(std::string_view value) {
  if (value.size() != 9 || value.front() != '#') {
    return 0xFFFFFFFFu;
  }
  return static_cast<std::uint32_t>(
      std::stoul(std::string(value.substr(1)), nullptr, 16));
}

ImU32 ParseArgbColor(std::string_view value) {
  const auto number = ParseArgbValue(value);
  const auto a = static_cast<unsigned>((number >> 24) & 0xFF);
  const auto r = static_cast<unsigned>((number >> 16) & 0xFF);
  const auto g = static_cast<unsigned>((number >> 8) & 0xFF);
  const auto b = static_cast<unsigned>(number & 0xFF);
  (void)a;
  return IM_COL32(r, g, b, 255);
}

float ParseArgbOpacity(std::string_view value) {
  return static_cast<float>((ParseArgbValue(value) >> 24) & 0xFF) / 255.0f;
}

ImVec2 Transform(const std::array<float, 6> &matrix, float x, float y) {
  return ImVec2(matrix[0] * x + matrix[2] * y + matrix[4],
                matrix[1] * x + matrix[3] * y + matrix[5]);
}

std::array<float, 6> Compose(const std::array<float, 6> &parent,
                             const std::array<float, 6> &local) {
  return {
      parent[0] * local[0] + parent[2] * local[1],
      parent[1] * local[0] + parent[3] * local[1],
      parent[0] * local[2] + parent[2] * local[3],
      parent[1] * local[2] + parent[3] * local[3],
      parent[0] * local[4] + parent[2] * local[5] + parent[4],
      parent[1] * local[4] + parent[3] * local[5] + parent[5],
  };
}

std::optional<std::array<float, 6>>
Inverse(const std::array<float, 6> &matrix) {
  const float determinant = matrix[0] * matrix[3] - matrix[1] * matrix[2];
  if (std::abs(determinant) < 1.0e-8f) {
    return std::nullopt;
  }
  const float inverse_determinant = 1.0f / determinant;
  const float a = matrix[3] * inverse_determinant;
  const float b = -matrix[1] * inverse_determinant;
  const float c = -matrix[2] * inverse_determinant;
  const float d = matrix[0] * inverse_determinant;
  return std::array<float, 6>{
      a,
      b,
      c,
      d,
      -(a * matrix[4] + c * matrix[5]),
      -(b * matrix[4] + d * matrix[5]),
  };
}

ImVec2 ClampedUv(ImVec2 uv, float width, float height) {
  return ImVec2((1.0f + uv.x * width) / (width + 2.0f),
                (1.0f + uv.y * height) / (height + 2.0f));
}

std::vector<std::uint32_t> DecodeUtf8(std::string_view value) {
  std::vector<std::uint32_t> result;
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index++]);
    if (first < 0x80) {
      result.push_back(first);
      continue;
    }
    std::uint32_t codepoint = 0;
    unsigned continuation = 0;
    if ((first & 0xE0) == 0xC0) {
      codepoint = first & 0x1F;
      continuation = 1;
    } else if ((first & 0xF0) == 0xE0) {
      codepoint = first & 0x0F;
      continuation = 2;
    } else if ((first & 0xF8) == 0xF0) {
      codepoint = first & 0x07;
      continuation = 3;
    } else {
      result.push_back(0xFFFD);
      continue;
    }
    if (index + continuation > value.size()) {
      result.push_back(0xFFFD);
      break;
    }
    bool valid = true;
    for (unsigned part = 0; part < continuation; ++part) {
      const auto byte = static_cast<unsigned char>(value[index++]);
      if ((byte & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (byte & 0x3F);
    }
    result.push_back(valid ? codepoint : 0xFFFD);
  }
  return result;
}

int DrawOrder(const DrawItem &item) {
  return std::visit([](const auto &value) { return value.draw_order; }, item);
}

} // namespace

struct RetailSceneRenderer::Impl {
  explicit Impl(std::filesystem::path root) : cache_root(std::move(root)) {}

  struct Variant {
    int category = 0;
    int option = 0;
    std::string internal_name;
    std::filesystem::path path;
  };

  std::filesystem::path cache_root;
  bool attempted = false;
  bool loaded = false;
  std::string load_error;
  std::vector<DrawItem> items;
  SceneAnimations animations;
  MotionPlayback motion_playback;
  std::vector<Variant> variants;
  std::size_t current_variant = 0;
  bool game_settings_open = false;
  bool game_settings_closing = false;
  std::map<std::string, RetailTexture> textures;
  std::unordered_map<std::string, std::shared_ptr<Font>> fonts;

  std::shared_ptr<Font> ParseFont(const Json &asset, std::string &error) {
    const auto metadata = asset.at("metadata").get<std::string>();
    if (const auto found = fonts.find(metadata); found != fonts.end()) {
      return found->second;
    }
    const auto &definition = asset.at("definition");
    auto font = std::make_shared<Font>();
    font->texture_path = asset.at("texture").get<std::string>();
    const auto &font_textures = definition.at("textures");
    if (font_textures.empty()) {
      error = "Bitmap font has no texture declaration: " + metadata;
      return nullptr;
    }
    font->texture_width = font_textures[0].at("width").get<int>();
    font->texture_height = font_textures[0].at("height").get<int>();
    font->ascent = definition.at("metrics").at("Ascent").get<float>();
    if (font->ascent <= 0 || font->texture_width <= 0 ||
        font->texture_height <= 0) {
      error = "Bitmap font has invalid metrics: " + metadata;
      return nullptr;
    }
    for (const auto &mapping : definition.at("characters")) {
      font->characters.emplace(mapping.at("codepoint").get<std::uint32_t>(),
                               mapping.at("glyph_index").get<int>());
    }
    for (const auto &entry : definition.at("glyphs")) {
      Glyph glyph;
      const auto index = entry.at("glyph_index").get<int>();
      glyph.texture_index = entry.at("texture_index").get<int>();
      const auto &atlas_bounds = entry.at("atlas_bounds");
      glyph.atlas_left = atlas_bounds[0].get<float>();
      glyph.atlas_top = atlas_bounds[1].get<float>();
      glyph.atlas_right = atlas_bounds[2].get<float>();
      glyph.atlas_bottom = atlas_bounds[3].get<float>();
      glyph.width = entry.at("width").get<float>();
      glyph.height = entry.at("height").get<float>();
      glyph.x_offset = entry.at("x_offset").get<float>();
      glyph.y_offset = entry.at("y_offset").get<float>();
      glyph.x_advance = entry.at("x_advance").get<float>();
      font->glyphs.emplace(index, glyph);
    }
    fonts.emplace(metadata, font);
    return font;
  }

  bool LoadScene(const std::filesystem::path &path,
                 std::vector<DrawItem> &parsed,
                 SceneAnimations &parsed_animations, std::string &error) {
    try {
      std::ifstream stream(path);
      if (!stream) {
        error = "Validated Career > Main scene manifest is missing: " +
                path.string();
        return false;
      }
      Json root;
      stream >> root;
      if (root.at("format") != "skate3-career-main-scene" ||
          root.at("version").get<int>() != 3 ||
          !root.at("validation").at("renderable").get<bool>() ||
          !root.at("validation").at("unresolved").empty()) {
        error = "Career > Main scene manifest failed provenance validation: " +
                path.string();
        return false;
      }

      for (const auto &source : root.at("scene").at("primitives")) {
        Primitive primitive;
        primitive.draw_order = source.at("draw_order").get<int>();
        primitive.item_key = source.at("item_key").get<std::string>();
        primitive.animation_track =
            source.value("animation_track", std::string{});
        for (std::size_t index = 0; index < primitive.matrix.size(); ++index) {
          primitive.matrix[index] = source.at("matrix")[index].get<float>();
        }
        const auto &color = source.at("color");
        primitive.color = IM_COL32(
            static_cast<unsigned>(
                std::clamp(color[0].get<float>(), 0.0f, 1.0f) * 255.0f),
            static_cast<unsigned>(
                std::clamp(color[1].get<float>(), 0.0f, 1.0f) * 255.0f),
            static_cast<unsigned>(
                std::clamp(color[2].get<float>(), 0.0f, 1.0f) * 255.0f),
            255);
        primitive.opacity = std::clamp(color[3].get<float>(), 0.0f, 1.0f);
        if (!source.at("texture").is_null()) {
          primitive.texture_path =
              source.at("texture").at("rgba").get<std::string>();
          primitive.texture_width = source.at("texture").at("width").get<int>();
          primitive.texture_height =
              source.at("texture").at("height").get<int>();
        }
        for (const auto &source_triangle : source.at("triangles")) {
          Triangle triangle;
          for (std::size_t index = 0; index < 3; ++index) {
            const auto &source_vertex = source_triangle[index];
            triangle.vertices[index].position =
                ImVec2(source_vertex.at("position")[0].get<float>(),
                       source_vertex.at("position")[1].get<float>());
            if (source_vertex.contains("uv")) {
              triangle.vertices[index].uv =
                  ImVec2(source_vertex.at("uv")[0].get<float>(),
                         source_vertex.at("uv")[1].get<float>());
            }
          }
          primitive.triangles.push_back(triangle);
        }
        parsed.emplace_back(std::move(primitive));
      }

      for (const auto &source : root.at("scene").at("text")) {
        TextRun text;
        text.draw_order = source.at("draw_order").get<int>();
        text.item_key = source.at("item_key").get<std::string>();
        text.animation_track = source.value("animation_track", std::string{});
        for (std::size_t index = 0; index < text.matrix.size(); ++index) {
          text.matrix[index] = source.at("matrix")[index].get<float>();
        }
        text.font = ParseFont(source.at("font_asset"), error);
        if (!text.font) {
          return false;
        }
        text.font_height = source.at("font_height").get<float>();
        const auto color_argb = source.at("color_argb").get<std::string>();
        text.color = ParseArgbColor(color_argb);
        text.opacity = ParseArgbOpacity(color_argb) *
                       std::clamp(source.at("alpha").get<float>(), 0.0f, 1.0f);
        text.value = source.at("value").get<std::string>();
        parsed.emplace_back(std::move(text));
      }

      const auto &source_animation = root.at("animations").at("selected_glow");
      const auto milliseconds_per_frame =
          source_animation.at("milliseconds_per_frame").get<int>();
      const auto frame_count =
          source_animation.at("frame_count").get<std::size_t>();
      if (milliseconds_per_frame <= 0 || frame_count == 0 ||
          !source_animation.at("loop").get<bool>()) {
        error = "Career > Main selected glow has invalid playback metadata";
        return false;
      }
      parsed_animations = {};
      parsed_animations.glow.frame_duration =
          std::chrono::milliseconds(milliseconds_per_frame);
      parsed_animations.glow.frame_count = frame_count;
      for (const auto &[name, source_track] :
           source_animation.at("tracks").items()) {
        const auto baseline =
            source_track.at("baseline_alpha_u8").get<unsigned>();
        const auto &source_values = source_track.at("alpha_u8");
        if (baseline == 0 || baseline > 255 ||
            source_values.size() != frame_count) {
          error =
              "Career > Main selected glow has an invalid alpha track: " + name;
          return false;
        }
        AlphaTrack track;
        track.baseline = static_cast<float>(baseline);
        track.values.reserve(frame_count);
        for (const auto &source_value : source_values) {
          const auto value = source_value.get<unsigned>();
          if (value > 255) {
            error =
                "Career > Main selected glow contains an invalid alpha value";
            return false;
          }
          track.values.push_back(static_cast<std::uint8_t>(value));
        }
        parsed_animations.glow.tracks.emplace(name, std::move(track));
      }
      for (const auto &item : parsed) {
        const auto &track = std::visit(
            [](const auto &value) -> const std::string & {
              return value.animation_track;
            },
            item);
        if (!track.empty() && !parsed_animations.glow.tracks.contains(track)) {
          error = "Career > Main draw item references a missing alpha track: " +
                  track;
          return false;
        }
      }

      const auto &source_motion = root.at("animations").at("motion");
      const auto motion_frame_duration =
          source_motion.at("milliseconds_per_frame").get<int>();
      if (motion_frame_duration <= 0) {
        error = "Career > Main motion timeline has an invalid frame rate";
        return false;
      }
      for (const auto &[clip_name, source_clip] :
           source_motion.at("clips").items()) {
        MotionClip clip;
        clip.frame_duration = std::chrono::milliseconds(
            source_clip.at("milliseconds_per_frame").get<int>());
        clip.frame_count = source_clip.at("frame_count").get<std::size_t>();
        if (clip.frame_duration.count() != motion_frame_duration ||
            clip.frame_count == 0 || source_clip.at("loop").get<bool>()) {
          error = "Career > Main motion clip has invalid playback metadata: " +
                  clip_name;
          return false;
        }
        for (const auto &source_track : source_clip.at("tracks")) {
          MotionTrack track;
          if (source_track.contains("matrix")) {
            const auto &source_matrices = source_track.at("matrix");
            if (source_matrices.size() != clip.frame_count) {
              error = "Career > Main motion matrix track has an invalid "
                      "frame count";
              return false;
            }
            track.matrices.reserve(clip.frame_count);
            for (const auto &source_matrix : source_matrices) {
              if (source_matrix.size() != 6) {
                error = "Career > Main motion track contains a malformed "
                        "matrix";
                return false;
              }
              std::array<float, 6> matrix{};
              for (std::size_t index = 0; index < matrix.size(); ++index) {
                matrix[index] = source_matrix[index].get<float>();
              }
              track.matrices.push_back(matrix);
            }
          }
          if (source_track.contains("opacity")) {
            const auto &source_opacities = source_track.at("opacity");
            if (source_opacities.size() != clip.frame_count) {
              error = "Career > Main motion opacity track has an invalid "
                      "frame count";
              return false;
            }
            track.opacities.reserve(clip.frame_count);
            for (const auto &source_opacity : source_opacities) {
              track.opacities.push_back(
                  std::clamp(source_opacity.get<float>(), 0.0f, 1.0f));
            }
          }
          if (track.matrices.empty() && track.opacities.empty()) {
            error = "Career > Main motion track contains no properties";
            return false;
          }
          const auto item_key = source_track.at("item").get<std::string>();
          if (!clip.tracks.emplace(item_key, std::move(track)).second) {
            error = "Career > Main motion clip contains a duplicate item: " +
                    item_key;
            return false;
          }
        }
        parsed_animations.motion.emplace(clip_name, std::move(clip));
      }
      if (!parsed_animations.motion.contains("open") ||
          !parsed_animations.motion.contains("close")) {
        error = "Career > Main source motion clips are incomplete";
        return false;
      }
      std::stable_sort(parsed.begin(), parsed.end(),
                       [](const DrawItem &left, const DrawItem &right) {
                         return DrawOrder(left) < DrawOrder(right);
                       });
      return true;
    } catch (const std::exception &exception) {
      error = std::string("Unable to parse Career > Main scene: ") +
              exception.what();
      return false;
    }
  }

  bool Load(std::string &error) {
    if (loaded) {
      return true;
    }
    if (attempted) {
      error = load_error;
      return false;
    }
    attempted = true;
    try {
      const auto index_path = CareerMainSceneIndexPath(cache_root);
      std::ifstream stream(index_path);
      if (!stream) {
        load_error =
            "Career > Main state index is missing: " + index_path.string();
        error = load_error;
        return false;
      }
      Json index;
      stream >> index;
      if (index.at("format") != "skate3-career-main-scene-index" ||
          index.at("version").get<int>() != 4 || index.at("variants").empty()) {
        load_error = "Career > Main state index failed validation";
        error = load_error;
        return false;
      }
      const auto variant_root = index_path.parent_path();
      for (const auto &source : index.at("variants")) {
        const std::filesystem::path relative =
            source.at("path").get<std::string>();
        if (relative.is_absolute() || relative.has_parent_path()) {
          load_error = "Career > Main state index contains an unsafe path";
          error = load_error;
          return false;
        }
        variants.push_back({source.at("category").get<int>(),
                            source.at("option").get<int>(),
                            source.at("internal_name").get<std::string>(),
                            variant_root / relative});
      }
      std::vector<DrawItem> parsed;
      SceneAnimations parsed_animations;
      if (!LoadScene(variants.front().path, parsed, parsed_animations,
                     load_error)) {
        error = load_error;
        return false;
      }
      items = std::move(parsed);
      animations = std::move(parsed_animations);
      animations.glow.started = std::chrono::steady_clock::now();
      current_variant = 0;
      loaded = true;
      PlayMotion("open");
      return true;
    } catch (const std::exception &exception) {
      load_error = std::string("Unable to parse Career > Main state index: ") +
                   exception.what();
      error = load_error;
      return false;
    }
  }

  bool MoveSelection(int direction, std::string &error) {
    if (game_settings_open || direction == 0 || variants.empty()) {
      return true;
    }
    const int next_option =
        variants[current_variant].option + (direction < 0 ? -1 : 1);
    const auto found = std::find_if(
        variants.begin(), variants.end(), [&](const Variant &variant) {
          return variant.category == variants[current_variant].category &&
                 variant.option == next_option;
        });
    if (found == variants.end()) {
      return true;
    }
    const auto next =
        static_cast<std::size_t>(std::distance(variants.begin(), found));
    if (next == current_variant) {
      return true;
    }
    std::vector<DrawItem> parsed;
    const int previous_option = variants[current_variant].option;
    SceneAnimations parsed_animations;
    if (!LoadScene(variants[next].path, parsed, parsed_animations, error)) {
      return false;
    }
    items = std::move(parsed);
    animations = std::move(parsed_animations);
    animations.glow.started = std::chrono::steady_clock::now();
    current_variant = next;
    PlayMotion("from_" + std::to_string(previous_option));
    return true;
  }

  bool MoveCategory(int direction, std::string &error) {
    if (game_settings_open || direction == 0 || variants.empty()) {
      return true;
    }
    const int previous_category = variants[current_variant].category;
    const int last_category = std::max_element(
                                  variants.begin(), variants.end(),
                                  [](const Variant &left, const Variant &right) {
                                    return left.category < right.category;
                                  })
                                  ->category;
    const int next_category =
        std::clamp(previous_category + (direction < 0 ? -1 : 1), 0,
                   last_category);
    if (next_category == previous_category) {
      return true;
    }
    const auto found = std::find_if(
        variants.begin(), variants.end(), [&](const Variant &variant) {
          return variant.category == next_category && variant.option == 0;
        });
    if (found == variants.end()) {
      error = "Crossbar category has no initial selection state";
      return false;
    }
    const auto next =
        static_cast<std::size_t>(std::distance(variants.begin(), found));
    std::vector<DrawItem> parsed;
    SceneAnimations parsed_animations;
    if (!LoadScene(variants[next].path, parsed, parsed_animations, error)) {
      return false;
    }
    items = std::move(parsed);
    animations = std::move(parsed_animations);
    animations.glow.started = std::chrono::steady_clock::now();
    current_variant = next;
    PlayMotion("from_category_" + std::to_string(previous_category));
    return true;
  }

  bool OpenGameSettings(std::string &error) {
    if (game_settings_open) {
      return true;
    }
    if (variants.empty() ||
        variants[current_variant].internal_name != "GameSettings") {
      error = "Game Settings can only open from its retail crossbar entry";
      return false;
    }
    const auto path =
        CareerMainSceneIndexPath(cache_root).parent_path() /
        "game_settings.json";
    std::vector<DrawItem> parsed;
    SceneAnimations parsed_animations;
    if (!LoadScene(path, parsed, parsed_animations, error)) {
      return false;
    }
    items = std::move(parsed);
    animations = std::move(parsed_animations);
    animations.glow.started = std::chrono::steady_clock::now();
    game_settings_open = true;
    game_settings_closing = false;
    PlayMotion("open");
    return true;
  }

  void BeginGameSettingsClose() {
    if (!game_settings_open || game_settings_closing) {
      return;
    }
    game_settings_closing = true;
    PlayMotion("close");
  }

  bool RestoreCrossbar(std::string &error) {
    std::vector<DrawItem> parsed;
    SceneAnimations parsed_animations;
    if (!LoadScene(variants[current_variant].path, parsed, parsed_animations,
                   error)) {
      return false;
    }
    items = std::move(parsed);
    animations = std::move(parsed_animations);
    animations.glow.started = std::chrono::steady_clock::now();
    game_settings_open = false;
    game_settings_closing = false;
    PlayMotion("open");
    return true;
  }

  void RestartAnimation() {
    if (!loaded) {
      return;
    }
    if (game_settings_open) {
      std::string error;
      if (!RestoreCrossbar(error)) {
        load_error = std::move(error);
        return;
      }
    }
    animations.glow.started = std::chrono::steady_clock::now();
    PlayMotion("open");
  }

  void PlayMotion(std::string_view name) {
    const auto found = animations.motion.find(name);
    if (found == animations.motion.end()) {
      motion_playback = {};
      return;
    }
    motion_playback.clip = &found->second;
    motion_playback.name = name;
    motion_playback.started = std::chrono::steady_clock::now();
  }

  void BeginCloseAnimation() {
    if (loaded) {
      PlayMotion("close");
    }
  }

  bool MotionComplete() const {
    if (!motion_playback.clip) {
      return true;
    }
    const auto duration = motion_playback.clip->frame_duration *
                          motion_playback.clip->frame_count;
    return std::chrono::steady_clock::now() - motion_playback.started >=
           duration;
  }

  bool CloseAnimationComplete() const {
    return !loaded || (motion_playback.name == "close" && MotionComplete());
  }

  float AnimatedAlpha(std::string_view track_name, std::size_t frame) const {
    if (track_name.empty()) {
      return 1.0f;
    }
    const auto found = animations.glow.tracks.find(track_name);
    if (found == animations.glow.tracks.end() || found->second.values.empty()) {
      return 1.0f;
    }
    const auto &track = found->second;
    return static_cast<float>(track.values[frame % track.values.size()]) /
           track.baseline;
  }

  ImVec2 Screen(ImVec2 value, ImVec2 origin, ImVec2 scale) const {
    return ImVec2(origin.x + value.x * scale.x, origin.y + value.y * scale.y);
  }

  std::size_t MotionFrame() const {
    if (!motion_playback.clip || MotionComplete()) {
      return 0;
    }
    const auto elapsed =
        std::chrono::steady_clock::now() - motion_playback.started;
    const auto elapsed_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return std::min(
        static_cast<std::size_t>(elapsed_milliseconds /
                                 motion_playback.clip->frame_duration.count()),
        motion_playback.clip->frame_count - 1);
  }

  const MotionTrack *CurrentMotionTrack(std::string_view item_key) const {
    if (!motion_playback.clip || MotionComplete()) {
      return nullptr;
    }
    const auto found = motion_playback.clip->tracks.find(std::string(item_key));
    return found == motion_playback.clip->tracks.end() ? nullptr
                                                       : &found->second;
  }

  RetailTexture *Texture(const std::string &path, int width, int height) {
    auto &texture = textures[path];
    std::string error;
    if (!texture.EnsureLoaded(cache_root / path, width, height, error)) {
      if (load_error.empty()) {
        load_error = std::move(error);
      }
      return nullptr;
    }
    return &texture;
  }

  void DrawPrimitive(ImDrawList *draw, const Primitive &primitive,
                     ImVec2 origin, ImVec2 scale, float alpha,
                     const MotionTrack *motion, std::size_t motion_frame) {
    const float opacity = motion && !motion->opacities.empty()
                              ? motion->opacities[motion_frame]
                              : primitive.opacity;
    if (alpha * opacity <= 0.0f) {
      return;
    }
    const auto color = WithAlpha(primitive.color, alpha * opacity);
    std::optional<std::array<float, 6>> stage_delta;
    if (motion && !motion->matrices.empty()) {
      const auto inverse = Inverse(primitive.matrix);
      if (inverse) {
        stage_delta = Compose(motion->matrices[motion_frame], *inverse);
      }
    }
    RetailTexture *texture = nullptr;
    if (!primitive.texture_path.empty()) {
      texture = Texture(primitive.texture_path, primitive.texture_width,
                        primitive.texture_height);
      if (!texture) {
        return;
      }
      draw->PushTexture(texture->data->GetTexRef());
    }
    for (const auto &triangle : primitive.triangles) {
      if (texture) {
        draw->PrimReserve(3, 3);
        for (const auto &vertex : triangle.vertices) {
          const auto position = stage_delta
                                    ? Transform(*stage_delta, vertex.position.x,
                                                vertex.position.y)
                                    : vertex.position;
          draw->PrimVtx(Screen(position, origin, scale),
                        ClampedUv(vertex.uv,
                                  static_cast<float>(primitive.texture_width),
                                  static_cast<float>(primitive.texture_height)),
                        color);
        }
      } else {
        const auto p0 = stage_delta ? Transform(*stage_delta,
                                                triangle.vertices[0].position.x,
                                                triangle.vertices[0].position.y)
                                    : triangle.vertices[0].position;
        const auto p1 = stage_delta ? Transform(*stage_delta,
                                                triangle.vertices[1].position.x,
                                                triangle.vertices[1].position.y)
                                    : triangle.vertices[1].position;
        const auto p2 = stage_delta ? Transform(*stage_delta,
                                                triangle.vertices[2].position.x,
                                                triangle.vertices[2].position.y)
                                    : triangle.vertices[2].position;
        draw->AddTriangleFilled(Screen(p0, origin, scale),
                                Screen(p1, origin, scale),
                                Screen(p2, origin, scale), color);
      }
    }
    if (texture) {
      draw->PopTexture();
    }
  }

  void DrawText(ImDrawList *draw, const TextRun &text, ImVec2 origin,
                ImVec2 scale, float alpha, const MotionTrack *motion,
                std::size_t motion_frame) {
    const float opacity = motion && !motion->opacities.empty()
                              ? motion->opacities[motion_frame]
                              : text.opacity;
    if (alpha * opacity <= 0.0f) {
      return;
    }
    auto *texture = Texture(text.font->texture_path, text.font->texture_width,
                            text.font->texture_height);
    if (!texture) {
      return;
    }
    const auto color = WithAlpha(text.color, alpha * opacity);
    const auto &matrix = motion && !motion->matrices.empty()
                             ? motion->matrices[motion_frame]
                             : text.matrix;
    // APT's font_height is the Flash ascent height, not the bmpFont's nominal
    // point Size. The shipped atlas bearings are measured in the same source
    // pixels as Ascent, placing the text baseline exactly font_height units
    // below the field origin.
    const float font_scale = text.font_height / text.font->ascent;
    const float baseline = text.font_height;
    float pen_x = 0.0f;
    for (const auto codepoint : DecodeUtf8(text.value)) {
      if (codepoint == '\r') {
        continue;
      }
      if (codepoint == '\n') {
        pen_x = 0.0f;
        continue;
      }
      const auto mapping = text.font->characters.find(codepoint);
      if (mapping == text.font->characters.end()) {
        continue;
      }
      const auto glyph_entry = text.font->glyphs.find(mapping->second);
      if (glyph_entry == text.font->glyphs.end()) {
        continue;
      }
      const auto &glyph = glyph_entry->second;
      const float left = pen_x + glyph.x_offset * font_scale;
      const float top = baseline - glyph.y_offset * font_scale;
      const float right = left + glyph.width * font_scale;
      const float bottom = top + glyph.height * font_scale;
      const auto p0 = Screen(Transform(matrix, left, top), origin, scale);
      const auto p1 = Screen(Transform(matrix, right, top), origin, scale);
      const auto p2 = Screen(Transform(matrix, right, bottom), origin, scale);
      const auto p3 = Screen(Transform(matrix, left, bottom), origin, scale);
      const ImVec2 uv0 =
          ClampedUv(ImVec2(glyph.atlas_left / text.font->texture_width,
                           glyph.atlas_top / text.font->texture_height),
                    static_cast<float>(text.font->texture_width),
                    static_cast<float>(text.font->texture_height));
      const ImVec2 uv1 =
          ClampedUv(ImVec2(glyph.atlas_right / text.font->texture_width,
                           glyph.atlas_top / text.font->texture_height),
                    static_cast<float>(text.font->texture_width),
                    static_cast<float>(text.font->texture_height));
      const ImVec2 uv2 =
          ClampedUv(ImVec2(glyph.atlas_right / text.font->texture_width,
                           glyph.atlas_bottom / text.font->texture_height),
                    static_cast<float>(text.font->texture_width),
                    static_cast<float>(text.font->texture_height));
      const ImVec2 uv3 =
          ClampedUv(ImVec2(glyph.atlas_left / text.font->texture_width,
                           glyph.atlas_bottom / text.font->texture_height),
                    static_cast<float>(text.font->texture_width),
                    static_cast<float>(text.font->texture_height));
      draw->AddImageQuad(texture->data->GetTexRef(), p0, p1, p2, p3, uv0, uv1,
                         uv2, uv3, color);
      pen_x += glyph.x_advance * font_scale;
    }
  }

  void Draw(ImDrawList *draw, ImVec2 origin, ImVec2 scale, float alpha) {
    if (game_settings_open && game_settings_closing &&
        motion_playback.name == "close" && MotionComplete()) {
      std::string error;
      if (!RestoreCrossbar(error)) {
        load_error = std::move(error);
        return;
      }
    }
    std::size_t frame = 0;
    if (animations.glow.frame_count != 0 &&
        animations.glow.frame_duration.count() > 0) {
      const auto elapsed =
          std::chrono::steady_clock::now() - animations.glow.started;
      const auto elapsed_milliseconds =
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
              .count();
      frame = static_cast<std::size_t>(elapsed_milliseconds /
                                       animations.glow.frame_duration.count()) %
              animations.glow.frame_count;
    }
    const auto motion_frame = MotionFrame();
    for (const auto &item : items) {
      std::visit(
          [&](const auto &value) {
            using Value = std::decay_t<decltype(value)>;
            const auto item_alpha =
                alpha * AnimatedAlpha(value.animation_track, frame);
            const auto *motion = CurrentMotionTrack(value.item_key);
            if constexpr (std::is_same_v<Value, Primitive>) {
              DrawPrimitive(draw, value, origin, scale, item_alpha, motion,
                            motion_frame);
            } else {
              DrawText(draw, value, origin, scale, item_alpha, motion,
                       motion_frame);
            }
          },
          item);
    }
  }
};

std::filesystem::path
CareerMainScenePath(const std::filesystem::path &cache_root) {
  return cache_root / "metadata" / "scenes" / "career_main" /
         "category_0_option_0.json";
}

std::filesystem::path
CareerMainSceneIndexPath(const std::filesystem::path &cache_root) {
  return cache_root / "metadata" / "scenes" / "career_main" / "index.json";
}

RetailSceneRenderer::RetailSceneRenderer(std::filesystem::path cache_root)
    : impl_(std::make_unique<Impl>(std::move(cache_root))) {}

RetailSceneRenderer::~RetailSceneRenderer() = default;

bool RetailSceneRenderer::EnsureLoaded(std::string &error) {
  return impl_->Load(error);
}

bool RetailSceneRenderer::MoveSelection(int direction, std::string &error) {
  return impl_->MoveSelection(direction, error);
}

std::size_t RetailSceneRenderer::SelectionIndex() const {
  return impl_->variants.empty()
             ? 0
             : static_cast<std::size_t>(
                   impl_->variants[impl_->current_variant].option);
}

bool RetailSceneRenderer::MoveCategory(int direction, std::string &error) {
  return impl_->MoveCategory(direction, error);
}

std::size_t RetailSceneRenderer::CategoryIndex() const {
  return impl_->variants.empty()
             ? 0
             : static_cast<std::size_t>(
                   impl_->variants[impl_->current_variant].category);
}

std::string RetailSceneRenderer::SelectedInternalName() const {
  return impl_->game_settings_open || impl_->variants.empty()
             ? std::string{}
             : impl_->variants[impl_->current_variant].internal_name;
}

bool RetailSceneRenderer::OpenGameSettings(std::string &error) {
  return impl_->OpenGameSettings(error);
}

void RetailSceneRenderer::BeginGameSettingsClose() {
  impl_->BeginGameSettingsClose();
}

bool RetailSceneRenderer::GameSettingsOpen() const {
  return impl_->game_settings_open;
}

void RetailSceneRenderer::RestartAnimation() { impl_->RestartAnimation(); }

void RetailSceneRenderer::BeginCloseAnimation() {
  impl_->BeginCloseAnimation();
}

bool RetailSceneRenderer::CloseAnimationComplete() const {
  return impl_->CloseAnimationComplete();
}

void RetailSceneRenderer::Draw(ImDrawList *draw, ImVec2 canvas_origin,
                               ImVec2 canvas_scale, float alpha) {
  impl_->Draw(draw, canvas_origin, canvas_scale, alpha);
}

} // namespace skate3::vanilla_ui
