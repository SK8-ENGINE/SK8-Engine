#include "skate3_map_editor_spawn_dialog.h"

#include "skate3_map_editor.h"

#include <imgui.h>

#include <algorithm>
#include <vector>

namespace skate3 {

void MapEditorSpawnDialog::OnDraw(ImGuiIO& io) {
  if (!map_editor::SpawnMenuVisible()) {
    return;
  }
  std::vector<std::string> names =
      map_editor::SpawnObjectNames();
  selected_ = std::clamp(
      selected_, 0, std::max(0, static_cast<int>(names.size()) - 1));

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.32f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin("Spawn object  [E to close]", nullptr, flags)) {
    ImGui::End();
    return;
  }
  ImGui::TextDisabled(
      "Spawns on the aimed surface (or 8 m ahead).");
  if (ImGui::Button("Refresh objects")) {
    map_editor::RefreshSpawnObjects();
    names = map_editor::SpawnObjectNames();
    selected_ = std::clamp(
        selected_, 0,
        std::max(0, static_cast<int>(names.size()) - 1));
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu object(s)", names.size());
  ImGui::Separator();
  if (names.empty()) {
    ImGui::TextUnformatted(
        "No valid .skateobj files found in objects/");
  } else {
    for (int index = 0; index < static_cast<int>(names.size()); ++index) {
      if (ImGui::Selectable(
              names[index].c_str(), selected_ == index)) {
        selected_ = index;
      }
    }
    ImGui::Separator();
    const bool activate =
        ImGui::Button("Spawn selected", ImVec2(-1.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (activate) {
      map_editor::QueueSpawnObject(
          static_cast<std::size_t>(selected_));
    }
  }
  ImGui::End();
}

}  // namespace skate3
