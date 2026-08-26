#include "skate3_vanilla_ui_dialog.h"
#include "skate3_ui_audio.h"
#include "skate3_ui_scene.h"

#include <algorithm>
#include <string>
#include <utility>

#include <imgui.h>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>

namespace skate3::vanilla_ui {
namespace {

constexpr uint16_t kPadDpadUp = 0x0001;
constexpr uint16_t kPadDpadDown = 0x0002;
constexpr uint16_t kPadDpadLeft = 0x0004;
constexpr uint16_t kPadDpadRight = 0x0008;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr float kRepeatDelay = 0.34f;
constexpr float kRepeatRate = 0.10f;

} // namespace

PrototypeDialog::PrototypeDialog(rex::ui::ImGuiDrawer *drawer,
                                 PollGamepad poll_gamepad,
                                 VisibilityChanged visibility_changed,
                                 std::filesystem::path asset_cache_root)
    : ImGuiDialog(drawer), poll_gamepad_(std::move(poll_gamepad)),
      visibility_changed_(std::move(visibility_changed)),
      asset_cache_root_(std::move(asset_cache_root)),
      scene_(std::make_unique<RetailSceneRenderer>(asset_cache_root_)),
      audio_(std::make_unique<RetailMenuAudio>(asset_cache_root_)) {
  SetDrawActive(false);
}

PrototypeDialog::~PrototypeDialog() = default;

void PrototypeDialog::Show() {
  if (visible_) {
    return;
  }
  visible_ = true;
  closing_ = false;
  previous_pad_buttons_ = 0xFFFF;
  held_vertical_ = 0;
  repeat_timer_ = 0.0f;
  scene_->RestartAnimation();
  audio_->Play(MenuSound::kFade);
  SetDrawActive(true);
  if (visibility_changed_) {
    visibility_changed_(true);
  }
}

void PrototypeDialog::Hide() {
  if (!visible_ || closing_) {
    return;
  }
  closing_ = true;
  held_vertical_ = 0;
  repeat_timer_ = 0.0f;
  scene_->BeginCloseAnimation();
  audio_->Play(MenuSound::kFade);
}

void PrototypeDialog::FinalizeHide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  closing_ = false;
  SetDrawActive(false);
  if (visibility_changed_) {
    visibility_changed_(false);
  }
}

void PrototypeDialog::Toggle() { visible_ ? Hide() : Show(); }

void PrototypeDialog::NavigateBack() { Hide(); }

PrototypeDialog::InputIntent PrototypeDialog::GatherInput(ImGuiIO &io) {
  InputIntent input;
  GamepadState pad;
  if (poll_gamepad_) {
    pad = poll_gamepad_();
  }
  const uint16_t pressed = pad.buttons & ~previous_pad_buttons_;
  previous_pad_buttons_ = pad.buttons;

  int pad_vertical = 0;
  if (pad.connected) {
    if (pad.buttons & kPadDpadUp) {
      pad_vertical = -1;
    } else if (pad.buttons & kPadDpadDown) {
      pad_vertical = 1;
    } else if (pad.thumb_ly > 12000) {
      pad_vertical = -1;
    } else if (pad.thumb_ly < -12000) {
      pad_vertical = 1;
    }
  }
  if (pad_vertical == 0) {
    held_vertical_ = 0;
    repeat_timer_ = 0.0f;
  } else if (pad_vertical != held_vertical_) {
    held_vertical_ = pad_vertical;
    repeat_timer_ = 0.0f;
    input.vertical = pad_vertical;
  } else {
    repeat_timer_ += io.DeltaTime;
    if (repeat_timer_ >= kRepeatDelay) {
      repeat_timer_ -= kRepeatRate;
      input.vertical = pad_vertical;
    }
  }

  if (pressed & kPadDpadLeft) {
    input.horizontal = -1;
  } else if (pressed & kPadDpadRight) {
    input.horizontal = 1;
  }
  input.select = (pressed & kPadA) != 0;
  input.back = (pressed & kPadB) != 0;
  if (pressed || pad_vertical) {
    pad_active_ = true;
  }

  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
    input.vertical = -1;
    pad_active_ = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
    input.vertical = 1;
    pad_active_ = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
    input.horizontal = -1;
    pad_active_ = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
    input.horizontal = 1;
    pad_active_ = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    input.select = true;
    pad_active_ = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    input.back = true;
    pad_active_ = false;
  }
  return input;
}

void PrototypeDialog::OnDraw(ImGuiIO &io) {
  if (!visible_) {
    return;
  }
  if (closing_ && scene_->CloseAnimationComplete()) {
    FinalizeHide();
    return;
  }
  const InputIntent input = closing_ ? InputIntent{} : GatherInput(io);
  if (input.back) {
    audio_->Play(MenuSound::kBack);
    if (scene_->GameSettingsOpen()) {
      scene_->BeginGameSettingsClose();
    } else {
      Hide();
    }
  }

  ImVec2 frame_pos(0.0f, 0.0f);
  ImVec2 frame_size = io.DisplaySize;
  if (auto *presenter = imgui_drawer()->presenter()) {
    const auto rect = presenter->GetLastGuestOutputPaintRect();
    if (rect.width > 0 && rect.height > 0) {
      frame_pos =
          ImVec2(static_cast<float>(rect.x), static_cast<float>(rect.y));
      frame_size = ImVec2(static_cast<float>(rect.width),
                          static_cast<float>(rect.height));
    }
  }

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::SetNextWindowBgAlpha(0.0f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!ImGui::Begin("##skate3_vanilla_ui_career_main", nullptr, kFlags)) {
    ImGui::End();
    return;
  }

  // APT's 1280x720 stage maps to the complete guest paint rectangle. Using
  // one aspect-preserving scalar left fractional letterbox slivers whenever
  // the host rectangle had odd dimensions (2559x1439 is the reported case).
  const ImVec2 canvas_scale(frame_size.x / 1280.0f, frame_size.y / 720.0f);
  const ImVec2 canvas_origin = frame_pos;
  std::string error;
  bool scene_ready = scene_->EnsureLoaded(error);
  if (scene_ready && input.horizontal != 0) {
    scene_ready = scene_->MoveCategory(input.horizontal, error);
  }
  if (scene_ready && input.vertical != 0) {
    const auto previous_selection = scene_->SelectionIndex();
    scene_ready = scene_->MoveSelection(input.vertical, error);
    if (scene_ready && scene_->SelectionIndex() != previous_selection) {
      audio_->Play(input.vertical < 0 ? MenuSound::kNavigateUp
                                     : MenuSound::kNavigateDown);
    }
  }
  if (scene_ready && input.select) {
    if (scene_->SelectedInternalName() == "GameSettings") {
      scene_ready = scene_->OpenGameSettings(error);
    }
    if (scene_ready) {
      audio_->Play(MenuSound::kConfirm);
    }
  }
  if (scene_ready) {
    auto *draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(
        frame_pos,
        ImVec2(frame_pos.x + frame_size.x, frame_pos.y + frame_size.y), true);
    scene_->Draw(draw, canvas_origin, canvas_scale, 1.0f);
    draw->PopClipRect();
  } else {
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(frame_pos.x + 24.0f, frame_pos.y + 24.0f),
        IM_COL32(255, 96, 96, 255), error.c_str());
  }

  ImGui::End();
}

} // namespace skate3::vanilla_ui
