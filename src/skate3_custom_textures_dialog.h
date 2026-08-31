#pragma once

// Edit Skater - Custom Textures dialog (project-owned UI for the skater
// customisation concept). Because the retail Edit Skater / Merchandise screen
// is recompiled game code, this window hosts a mirror of the concept: a
// "Merchandise" overview that points at the in-game shop and a "Custom
// Textures" section listing every browsable skater part with the custom
// textures the player has imported. The library persists under the user data
// root, so the section is reachable again after a restart (F7 keybind).
//
// Imports go through a three-step wizard: pick the skater part, pick an image
// file (PNG/JPEG/WebP/TGA/DDS/BMP), give the texture a name. Applying a
// texture marks the per-part selection that the renderer integration reads via
// skate3::custom_textures::Library::AppliedTexturePath().

#include <filesystem>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

class CustomTexturesDialog final : public rex::ui::ImGuiDialog {
 public:
  CustomTexturesDialog(rex::ui::ImGuiDrawer* drawer,
                       std::filesystem::path user_data_root);
  ~CustomTexturesDialog() override;

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool visible_ = false;
};

}  // namespace skate3