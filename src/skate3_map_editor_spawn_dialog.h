#pragma once

#include <rex/ui/imgui_dialog.h>

#include <array>

namespace skate3 {

class MapEditorSpawnDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MapEditorSpawnDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  int selected_ = 0;
  std::array<char, 128> search_{};
};

}  // namespace skate3
