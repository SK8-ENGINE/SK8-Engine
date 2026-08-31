#include "skate3_custom_textures_dialog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>

#include "skate3_custom_textures.h"

#if defined(_WIN32)
#include <windows.h>
#pragma comment(lib, "comdlg32.lib")
#endif

namespace skate3 {
namespace {

constexpr const char* kWizardId = "Add Custom Texture##skate3_custom_textures";
constexpr int kThumbnailMax = 160;

#if defined(_WIN32)
// Native file picker for the wizard's file step.
bool PickImageFile(std::filesystem::path& output) {
  static constexpr wchar_t kFilter[] =
      L"Image files (*.png;*.jpg;*.jpeg;*.webp;*.tga;*.dds;*.bmp)\0"
      L"*.png;*.jpg;*.jpeg;*.webp;*.tga;*.dds;*.bmp\0"
      L"All files (*.*)\0*.*\0\0";
  std::wstring buffer(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = kFilter;
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  ofn.lpstrTitle = L"Choose a custom texture image";
  if (GetOpenFileNameW(&ofn) == 0) {
    return false;
  }
  output = std::filesystem::path(buffer.c_str());
  return true;
}
#endif

std::string Trimmed(const std::string& text) {
  std::size_t first = 0;
  while (first < text.size() &&
         (text[first] == ' ' || text[first] == '\t' || text[first] == '\n')) {
    ++first;
  }
  std::size_t last = text.size();
  while (last > first &&
         (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\n')) {
    --last;
  }
  return text.substr(first, last - first);
}

}  // namespace

struct CustomTexturesDialog::Impl {
  custom_textures::Library library;

  std::string library_error;

  int current_tab = 1;  // 0 = Merchandise, 1 = Custom Textures
  bool part_view_open = false;
  custom_textures::Part current_part = custom_textures::Part::kHead;

  // Add-texture wizard.
  bool wizard_open = false;
  bool wizard_requested = false;
  int wizard_step = 0;
  custom_textures::Part wizard_part = custom_textures::Part::kHead;
  std::string wizard_path;
  std::array<char, 200> name_buffer{};

  std::string wizard_error;
  std::string persist_note;

  struct Thumb {
    std::unique_ptr<ImTextureData> data;
    bool failed = false;
    std::string note;

    ~Thumb() {
      if (data && ImGui::GetCurrentContext()) {
        ImGui::UnregisterUserTexture(data.get());
      }
    }
  };
  std::unordered_map<std::uint64_t, Thumb> thumbs;

  explicit Impl(std::filesystem::path user_data_root)
      : library(std::move(user_data_root)) {}

  // Returns a thumbnail for the preset, generating it lazily (decodes and
  // downsamples the stored image once per preset).
  ImTextureData* ThumbnailFor(const custom_textures::TexturePreset& preset,
                              std::string& error) {
    const auto it = thumbs.find(preset.id);
    if (it != thumbs.end()) {
      if (it->second.failed) {
        error = it->second.note;
        return nullptr;
      }
      return it->second.data.get();
    }

    Thumb thumb;
    custom_textures::ImageTexture image;
    if (!custom_textures::DecodeImageFileToRgba8(library.PresetImagePath(preset),
                                                 image, nullptr, error)) {
      thumb.failed = true;
      thumb.note = error;
      thumbs.emplace(preset.id, std::move(thumb));
      return nullptr;
    }
    const int src_w = static_cast<int>(image.width);
    const int src_h = static_cast<int>(image.height);
    int factor = 1;
    while (src_w / factor > kThumbnailMax || src_h / factor > kThumbnailMax) {
      ++factor;
    }
    const int dst_w = std::max(1, src_w / factor);
    const int dst_h = std::max(1, src_h / factor);
    thumb.data = std::make_unique<ImTextureData>();
    thumb.data->Create(ImTextureFormat_RGBA32, dst_w, dst_h);
    if (thumb.data->GetPixels() == nullptr) {
      thumb.failed = true;
      thumb.note = "Could not create thumbnail texture";
      thumbs.emplace(preset.id, std::move(thumb));
      return nullptr;
    }
    auto* dst = static_cast<std::uint8_t*>(thumb.data->GetPixels());
    for (int y = 0; y < dst_h; ++y) {
      for (int x = 0; x < dst_w; ++x) {
        const std::uint8_t* src =
            image.rgba8.data() +
            (static_cast<std::uint64_t>(y * factor) * src_w + x * factor) * 4;
        std::memcpy(dst + (static_cast<std::uint64_t>(y) * dst_w + x) * 4, src, 4);
      }
    }
    ImGui::RegisterUserTexture(thumb.data.get());
    ImTextureData* result = thumb.data.get();
    thumbs.emplace(preset.id, std::move(thumb));
    return result;
  }

  void DropThumbnail(std::uint64_t preset_id) { thumbs.erase(preset_id); }

  static void DrawPartGrid(Impl& self, bool& picked) {
    const float item_width = 168.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    const int per_row = std::max(1, static_cast<int>(available / item_width));
    int column = 0;
    for (const custom_textures::PartInfo& info : custom_textures::PartCatalog()) {
      const int count =
          static_cast<int>(self.library.PresetsForPart(info.part).size());
      char label[48];
      std::snprintf(label, sizeof(label), "%s (%d)", info.label, count);
      if (column > 0) {
        ImGui::SameLine();
      }
      if (ImGui::Button(label, ImVec2(item_width, 0.0f))) {
        self.current_part = info.part;
        picked = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Part slot: %s", info.id);
      }
      ++column;
      if (column >= per_row) {
        column = 0;
      }
    }
  }

  static void DrawMerchandise(Impl& self) {
    ImGui::TextWrapped(
        "In-game merchandise opens from the Edit Skater section of the retail "
        "game (recompiled game code). Custom textures below are project-owned "
        "and live on the next tab.");
    if (ImGui::Button("Open Custom Textures")) {
      self.current_tab = 1;
    }
    ImGui::Separator();
    ImGui::TextWrapped("Library folder: %s", self.library.directory().string().c_str());
    int applied_count = 0;
    for (const custom_textures::PartInfo& info : custom_textures::PartCatalog()) {
      if (self.library.AppliedForPart(info.part) != 0) {
        ++applied_count;
      }
    }
    ImGui::Text("Presets: %d  |  Parts with an active texture: %d",
                static_cast<int>(self.library.presets().size()), applied_count);
  }

  static void DrawHeaderRow(Impl& self, const char* title) {
    if (self.part_view_open) {
      if (ImGui::Button("< Back")) {
        self.part_view_open = false;
      }
      ImGui::SameLine();
      std::string heading = std::string("Custom Textures / ") + title;
      ImGui::TextUnformatted(heading.c_str());
    } else {
      ImGui::TextUnformatted("Custom Textures");
    }
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextDisabled("F7");
  }

  static void DrawPartDetail(Impl& self, std::string& frame_error) {
    const custom_textures::Part part = self.current_part;
    const char* label = custom_textures::PartLabel(part);
    DrawHeaderRow(self, label);

    const std::uint64_t applied = self.library.AppliedForPart(part);
    if (applied != 0) {
      ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "Active texture: %s",
                         self.library.Find(applied) != nullptr
                             ? self.library.Find(applied)->name.c_str()
                             : "?");
    } else {
      ImGui::TextDisabled("This part uses the game's own texture.");
    }

    if (ImGui::Button("Add Texture...")) {
      self.StartWizard(part);
    }
    ImGui::Separator();

    const std::vector<const custom_textures::TexturePreset*> presets =
        self.library.PresetsForPart(part);
    if (presets.empty()) {
      ImGui::TextWrapped(
          "No custom textures for this part yet. Use \"Add Texture...\" to "
          "import a PNG, JPEG, WebP, TGA, DDS or BMP image.");
      return;
    }

    if (ImGui::BeginChild("##part_presets", ImVec2(0.0f, 0.0f), true)) {
      for (const custom_textures::TexturePreset* preset : presets) {
        ImGui::PushID(static_cast<int>(preset->id));

        std::string thumb_error;
        ImTextureData* thumb = self.ThumbnailFor(*preset, thumb_error);
        const float thumb_size = 64.0f;
        if (thumb != nullptr) {
          ImGui::Image(reinterpret_cast<ImTextureID>(thumb), ImVec2(thumb_size, thumb_size));
        } else {
          ImGui::Image(nullptr, ImVec2(thumb_size, thumb_size));
        }
        ImGui::SameLine();

        const bool is_applied = (applied == preset->id);
        ImGui::TextUnformatted(preset->name.c_str());
        ImGui::TextDisabled("%s", custom_textures::SupportedImageExtensions());
        ImGui::SameLine(0.0f, 24.0f);

        if (is_applied) {
          if (ImGui::Button("Applied")) {
            // Clicking a second time clears the applied selection.
            std::string error;
            if (!self.library.ClearApplied(part, error)) {
              frame_error = error;
            }
          }
        } else if (ImGui::Button("Apply")) {
          std::string error;
          if (self.library.SetApplied(part, preset->id, error)) {
            self.persist_note = "Applying \"" + preset->name + "\" to " + label;
          } else {
            frame_error = error;
          }
        }
        if (ImGui::IsItemHovered() && !is_applied) {
          ImGui::SetTooltip(
              "Use this custom texture for the %s part. The renderer reads the "
              "per-part applied selection.",
              custom_textures::PartId(part));
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
          std::string error;
          const std::uint64_t removed_id = preset->id;
          if (self.library.Remove(removed_id, error)) {
            self.DropThumbnail(removed_id);
            self.persist_note = "Removed \"" + preset->name + "\".";
          } else {
            frame_error = error;
          }
        }
        if (!thumb_error.empty()) {
          ImGui::Separator();
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", thumb_error.c_str());
          ImGui::Separator();
        }
        ImGui::Separator();
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }

  static void DrawCustomTextures(Impl& self, std::string& frame_error) {
    if (!self.part_view_open) {
      if (ImGui::Button("Add Texture...")) {
        self.StartWizard(custom_textures::Part::kHead);
      }
      ImGui::SameLine();
      ImGui::TextWrapped(
          "Pick a skater part to see the custom textures assigned to it.");
      ImGui::Separator();
      bool picked = false;
      DrawPartGrid(self, picked);
      if (picked) {
        self.part_view_open = true;
      }
    } else {
      DrawPartDetail(self, frame_error);
    }
  }

  void StartWizard(custom_textures::Part part) {
    wizard_open = true;
    wizard_requested = false;
    wizard_step = 0;
    wizard_part = part;
    wizard_path.clear();
    name_buffer.fill(0);
    wizard_error.clear();
  }

  static void DrawWizard(Impl& self) {
    if (!self.wizard_open) {
      if (ImGui::IsPopupOpen(kWizardId)) {
        ImGui::CloseCurrentPopup();
      }
      return;
    }
    if (!self.wizard_requested) {
      ImGui::OpenPopup(kWizardId);
      self.wizard_requested = true;
    }
    if (ImGui::BeginPopupModal(kWizardId, &self.wizard_open,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      switch (self.wizard_step) {
        case 0: {
          ImGui::TextUnformatted("Step 1 of 3 - Which part gets this texture?");
          ImGui::Separator();
          bool picked = false;
          DrawPartGrid(self, picked);
          if (picked) {
            self.wizard_part = self.current_part;
            self.wizard_step = 1;
          }
          ImGui::Separator();
          if (ImGui::Button("Cancel")) {
            self.wizard_open = false;
          }
          break;
        }
        case 1: {
          ImGui::TextUnformatted("Step 2 of 3 - Choose the texture image file.");
          ImGui::TextWrapped(
              "PNG, JPEG, WebP, TGA, DDS and BMP are supported. The file is "
              "copied into the library folder.");
          std::array<char, 4096> path_buffer{};
          std::strncpy(path_buffer.data(), self.wizard_path.c_str(),
                       std::min(path_buffer.size() - 1, self.wizard_path.size()));
#if defined(_WIN32)
          ImGui::InputText("File", path_buffer.data(), path_buffer.size(),
                           ImGuiInputTextFlags_ReadOnly);
          ImGui::SameLine();
          if (ImGui::Button("Browse...")) {
            std::filesystem::path picked_path;
            if (PickImageFile(picked_path)) {
              self.wizard_path = picked_path.string();
            }
          }
#else
          ImGui::InputText("File", path_buffer.data(), path_buffer.size());
          ImGui::SameLine();
          if (ImGui::Button("Set Path")) {
            const std::string entered(path_buffer.data());
            if (!entered.empty()) {
              self.wizard_path = entered;
            }
          }
#endif
          ImGui::Separator();
          if (ImGui::Button("< Back")) {
            self.wizard_step = 0;
            self.wizard_error.clear();
          }
          ImGui::SameLine();
          if (ImGui::Button("Next")) {
            if (self.wizard_path.empty()) {
              self.wizard_error = "Choose a texture image file first.";
            } else {
              std::string decode_error;
              custom_textures::ImageTexture probe;
              if (custom_textures::DecodeImageFileToRgba8(
                      std::filesystem::path(self.wizard_path), probe, nullptr,
                      decode_error)) {
                const std::string base =
                    std::filesystem::path(self.wizard_path).stem().string();
                std::strncpy(self.name_buffer.data(), base.c_str(),
                             std::min(self.name_buffer.size() - 1, base.size()));
                self.wizard_step = 2;
                self.wizard_error.clear();
              } else {
                self.wizard_error = "That file cannot be used as a texture: " +
                                    decode_error;
              }
            }
          }
          break;
        }
        case 2: {
          ImGui::TextUnformatted("Step 3 of 3 - Name the texture.");
          ImGui::TextDisabled("Part: %s",
                              custom_textures::PartLabel(self.wizard_part));
          ImGui::InputText("Name", self.name_buffer.data(),
                           static_cast<int>(self.name_buffer.size()));
          ImGui::Separator();
          if (ImGui::Button("< Back")) {
            self.wizard_step = 1;
            self.wizard_error.clear();
          }
          ImGui::SameLine();
          if (ImGui::Button("Save Texture")) {
            std::string name = Trimmed(self.name_buffer.data());
            if (name.empty()) {
              self.wizard_error = "Give the texture a name.";
            } else {
              std::string import_error;
              std::uint64_t new_id = 0;
              if (self.library.ImportFromFile(
                      std::filesystem::path(self.wizard_path), self.wizard_part,
                      name, &new_id, import_error)) {
                self.persist_note = "Added \"" + name + "\" to " +
                                    custom_textures::PartLabel(self.wizard_part) +
                                    ".";
                self.current_tab = 1;
                self.current_part = self.wizard_part;
                self.part_view_open = true;
                self.wizard_open = false;
              } else {
                self.wizard_error = import_error;
              }
            }
          }
          break;
        }
        default:
          self.wizard_open = false;
          break;
      }

      if (!self.wizard_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s",
                           self.wizard_error.c_str());
      }
      ImGui::EndPopup();
    }
    if (!self.wizard_open) {
      self.wizard_requested = false;
      self.wizard_step = 0;
      self.wizard_error.clear();
    }
  }
};

CustomTexturesDialog::CustomTexturesDialog(rex::ui::ImGuiDrawer* drawer,
                                           std::filesystem::path user_data_root)
    : ImGuiDialog(drawer),
      impl_(std::make_unique<Impl>(std::move(user_data_root))) {
  impl_->library.Load(impl_->library_error);
}

CustomTexturesDialog::~CustomTexturesDialog() = default;

void CustomTexturesDialog::Show() {
  visible_ = true;
  SetDrawActive(true);
}

void CustomTexturesDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  SetDrawActive(false);
}

void CustomTexturesDialog::Toggle() {
  if (visible_) {
    Hide();
  } else {
    Show();
  }
}

void CustomTexturesDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  if (!visible_) {
    return;
  }

  std::string frame_error;

  bool open = visible_;
  ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Edit Skater - Custom Textures (F7)", &open,
                    ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) {
      Hide();
    }
    return;
  }

  if (!impl_->library_error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s",
                       impl_->library_error.c_str());
    ImGui::Separator();
  }

  if (ImGui::BeginTabBar("##skate3_edit_skater_tabs")) {
    if (ImGui::BeginTabItem("Merchandise")) {
      impl_->current_tab = 0;
      Impl::DrawMerchandise(*impl_);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Custom Textures")) {
      impl_->current_tab = 1;
      Impl::DrawCustomTextures(*impl_, frame_error);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  if (!impl_->persist_note.empty()) {
    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "%s",
                       impl_->persist_note.c_str());
  }
  if (!frame_error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s",
                       frame_error.c_str());
  }

  ImGui::End();
  impl_->persist_note.clear();

  Impl::DrawWizard(*impl_);

  if (!open) {
    Hide();
  }
}

}  // namespace skate3