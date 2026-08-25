#pragma once

#include <filesystem>
#include <memory>

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::vanilla_ui {

enum class MenuSound {
  kNavigateUp,
  kNavigateDown,
  kConfirm,
  kBack,
  kFade,
  kTransitionIn,
  kTransitionOut,
  kPopup,
};

class RetailMenuAudio {
public:
  explicit RetailMenuAudio(std::filesystem::path cache_root);
  ~RetailMenuAudio();

  RetailMenuAudio(const RetailMenuAudio &) = delete;
  RetailMenuAudio &operator=(const RetailMenuAudio &) = delete;

  void Play(MenuSound sound);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::filesystem::path
RetailMenuAudioManifestPath(const std::filesystem::path &cache_root);

// Observes the retail front-end sound queue so native playback follows the
// same live user volume setting instead of bypassing the game's gain staging.
void InstallAudioHooks(rex::runtime::FunctionDispatcher *dispatcher);

} // namespace skate3::vanilla_ui
