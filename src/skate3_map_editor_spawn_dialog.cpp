#include "skate3_map_editor_spawn_dialog.h"

#include "skate3_map_editor.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace skate3 {
namespace {

bool ContainsInsensitive(
    std::string_view value, std::string_view query) {
  if (query.empty()) {
    return true;
  }
  return std::search(
             value.begin(), value.end(), query.begin(), query.end(),
             [](unsigned char left, unsigned char right) {
               return std::tolower(left) == std::tolower(right);
             }) != value.end();
}

bool MatchesSearch(
    const map_editor::SpawnObjectEntry& entry,
    std::string_view query) {
  return ContainsInsensitive(entry.name, query) ||
         ContainsInsensitive(entry.category, query);
}

}  // namespace

void MapEditorSpawnDialog::OnDraw(ImGuiIO& io) {
  if (!map_editor::SpawnMenuVisible()) {
    return;
  }
  std::vector<map_editor::SpawnObjectEntry> entries =
      map_editor::SpawnObjectEntries();
  selected_ = std::clamp(
      selected_, 0,
      std::max(0, static_cast<int>(entries.size()) - 1));

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.32f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(
      ImVec2(520.0f, 620.0f), ImGuiCond_Always);
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
    entries = map_editor::SpawnObjectEntries();
    selected_ = std::clamp(
        selected_, 0,
        std::max(0, static_cast<int>(entries.size()) - 1));
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu object(s)", entries.size());
  ImGui::Separator();

  const map_editor::DefaultLibraryStatus setup =
      map_editor::GetDefaultLibraryStatus();
  if (setup.state == map_editor::DefaultLibraryState::Running) {
    const float fraction =
        setup.total == 0
            ? 0.0f
            : static_cast<float>(setup.completed) /
                  static_cast<float>(setup.total);
    ImGui::ProgressBar(
        fraction, ImVec2(-1.0f, 0.0f),
        setup.total == 0
            ? "Reading Skate 3 catalogue"
            : (std::to_string(setup.completed) + " / " +
               std::to_string(setup.total))
                  .c_str());
    ImGui::TextWrapped("%s", setup.message.c_str());
  } else {
    const char* setup_label =
        setup.state == map_editor::DefaultLibraryState::NotStarted
            ? "Build defaults from installed Skate 3"
            : "Build / repair defaults";
    if (ImGui::Button(setup_label, ImVec2(-1.0f, 0.0f))) {
      map_editor::StartDefaultLibraryImport();
    }
    if (!setup.message.empty()) {
      ImGui::TextWrapped("%s", setup.message.c_str());
    }
    if (!setup.errors.empty()) {
      ImGui::TextColored(
          ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
          "%zu conversion error(s)", setup.errors.size());
      const std::size_t shown =
          std::min<std::size_t>(setup.errors.size(), 3);
      for (std::size_t index = 0; index < shown; ++index) {
        ImGui::BulletText("%s", setup.errors[index].c_str());
      }
    }
  }
  ImGui::TextDisabled(
      "Generated assets stay in local user data. Put personal "
      ".skateobj files in objects/Custom.");
  ImGui::Separator();

  if (entries.empty()) {
    ImGui::TextWrapped(
        "No valid .skateobj files are available yet. Build the default "
        "library above or add files to the Custom folder.");
  } else {
    ImGui::SetNextItemWidth(-62.0f);
    ImGui::InputTextWithHint(
        "##spawn-object-search", "Search items or categories...",
        search_.data(), search_.size(),
        ImGuiInputTextFlags_EscapeClearsAll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
      search_.fill('\0');
    }

    const std::string_view query(search_.data());
    int first_match = -1;
    std::size_t match_count = 0;
    bool selected_visible = false;
    for (int index = 0;
         index < static_cast<int>(entries.size()); ++index) {
      if (!MatchesSearch(entries[index], query)) {
        continue;
      }
      if (first_match < 0) {
        first_match = index;
      }
      ++match_count;
      selected_visible = selected_visible || selected_ == index;
    }
    if (!selected_visible && first_match >= 0) {
      selected_ = first_match;
    }
    ImGui::TextDisabled(
        query.empty() ? "%zu object(s)" : "%zu of %zu object(s)",
        query.empty() ? entries.size() : match_count,
        entries.size());

    ImGui::BeginChild(
        "##spawn-object-list", ImVec2(0.0f, -46.0f),
        ImGuiChildFlags_Borders);
    std::string category;
    bool category_open = false;
    for (int index = 0;
         index < static_cast<int>(entries.size()); ++index) {
      const map_editor::SpawnObjectEntry& entry = entries[index];
      if (!MatchesSearch(entry, query)) {
        continue;
      }
      if (entry.category != category) {
        category = entry.category;
        if (query.empty()) {
          // Category folders begin closed. ImGui retains any explicit user
          // choice for the rest of the editor session.
          category_open =
              ImGui::CollapsingHeader(category.c_str());
        } else {
          // Search results should be immediately visible rather than hidden
          // behind the category's normal collapsed state.
          ImGui::TextDisabled("%s", category.c_str());
          category_open = true;
        }
      }
      if (!category_open) {
        continue;
      }
      ImGui::PushID(index);
      if (ImGui::Selectable(
              entry.name.c_str(), selected_ == index)) {
        selected_ = index;
      }
      ImGui::PopID();
    }
    if (match_count == 0) {
      ImGui::TextDisabled("No items match \"%s\".", search_.data());
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginDisabled(match_count == 0);
    const bool activate =
        ImGui::Button("Spawn selected", ImVec2(-1.0f, 0.0f)) ||
        (!ImGui::IsAnyItemActive() &&
         ImGui::IsKeyPressed(ImGuiKey_Enter, false));
    if (activate) {
      map_editor::QueueSpawnObject(
          entries[static_cast<std::size_t>(selected_)].asset_index);
    }
    ImGui::EndDisabled();
  }
  ImGui::End();
}

}  // namespace skate3
