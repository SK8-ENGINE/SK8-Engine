#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <imgui.h>

namespace skate3::vanilla_ui {

class RetailSceneRenderer {
public:
  explicit RetailSceneRenderer(std::filesystem::path cache_root);
  ~RetailSceneRenderer();

  RetailSceneRenderer(const RetailSceneRenderer &) = delete;
  RetailSceneRenderer &operator=(const RetailSceneRenderer &) = delete;

  bool EnsureLoaded(std::string &error);
  bool MoveSelection(int direction, std::string &error);
  bool MoveCategory(int direction, std::string &error);
  std::size_t SelectionIndex() const;
  std::size_t CategoryIndex() const;
  std::string SelectedInternalName() const;
  bool OpenGameSettings(std::string &error);
  void BeginGameSettingsClose();
  bool GameSettingsOpen() const;
  void RestartAnimation();
  void BeginCloseAnimation();
  bool CloseAnimationComplete() const;
  void Draw(ImDrawList *draw, ImVec2 canvas_origin, ImVec2 canvas_scale,
            float alpha);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::filesystem::path
CareerMainScenePath(const std::filesystem::path &cache_root);
std::filesystem::path
CareerMainSceneIndexPath(const std::filesystem::path &cache_root);

} // namespace skate3::vanilla_ui
