#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace skate3::vanilla_ui {

class RetailSceneRenderer;
class RetailMenuAudio;

// Small transport type so the prototype does not depend on the settings
// overlay's input model. The application supplies the raw UI-pad state.
struct GamepadState {
  bool connected = false;
  uint16_t buttons = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
};

class PrototypeDialog final : public rex::ui::ImGuiDialog {
public:
  using PollGamepad = std::function<GamepadState()>;
  using VisibilityChanged = std::function<void(bool)>;

  PrototypeDialog(rex::ui::ImGuiDrawer *drawer, PollGamepad poll_gamepad,
                  VisibilityChanged visibility_changed,
                  std::filesystem::path asset_cache_root);
  ~PrototypeDialog();

  void Show();
  void Hide();
  void Toggle();
  void NavigateBack();
  bool visible() const { return visible_; }

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  struct InputIntent {
    int vertical = 0;
    int horizontal = 0;
    bool select = false;
    bool back = false;
  };

  InputIntent GatherInput(ImGuiIO &io);
  void FinalizeHide();

  PollGamepad poll_gamepad_;
  VisibilityChanged visibility_changed_;
  std::filesystem::path asset_cache_root_;
  std::unique_ptr<RetailSceneRenderer> scene_;
  std::unique_ptr<RetailMenuAudio> audio_;
  bool visible_ = false;
  bool closing_ = false;
  bool pad_active_ = false;
  uint16_t previous_pad_buttons_ = 0;
  int held_vertical_ = 0;
  float repeat_timer_ = 0.0f;
};

} // namespace skate3::vanilla_ui
