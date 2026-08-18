#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace skate3 {

enum class ReleaseUpdatePhase {
  kIdle,
  kChecking,
  kDownloading,
  kInstalling,
  kUpToDate,
  kFailed,
  kUnsupported,
};

struct ReleaseUpdateState {
  ReleaseUpdatePhase phase = ReleaseUpdatePhase::kIdle;
  std::string current_version;
  std::string latest_version;
  std::string status;
  float progress = 0.0f;
};

class ReleaseUpdater {
public:
  using RestartCallback = std::function<void()>;

  ReleaseUpdater(std::filesystem::path executable_path,
                 std::string current_version, RestartCallback restart_callback);
  ~ReleaseUpdater();

  ReleaseUpdater(const ReleaseUpdater &) = delete;
  ReleaseUpdater &operator=(const ReleaseUpdater &) = delete;

  ReleaseUpdateState state() const;
  void Start();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skate3
