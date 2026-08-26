#pragma once

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

class MapEditorSpawnDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MapEditorSpawnDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  int selected_ = 0;
};

}  // namespace skate3
