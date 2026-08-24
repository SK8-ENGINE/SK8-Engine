#include "skate3_cac_catalogue.h"

#include "skate3_cac_archive.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/logging.h>

namespace skate3::cac_catalogue {
namespace {

class Catalogue {
 public:
  ~Catalogue() {
    Stop();
  }

  void Start(
      const std::filesystem::path& game_data_root,
      const std::filesystem::path& cache_root) {
    Stop();
    {
      std::lock_guard lock(mutex_);
      stopping_ = false;
      preparing_ = true;
      root_.clear();
      cache_base_.clear();
      archive_.reset();
    }
    const std::filesystem::path archive_path =
        game_data_root / "data" / "content" /
        "createacharacter.big";
    thread_ = std::thread(
        [this, archive_path, cache_root] {
          Prepare(archive_path, cache_root);
        });
  }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    std::lock_guard lock(mutex_);
    preparing_ = false;
    root_.clear();
    cache_base_.clear();
    archive_.reset();
  }

  std::filesystem::path Root(bool wait) {
    std::unique_lock lock(mutex_);
    if (wait) {
      ready_.wait(lock, [this] {
        return !preparing_ || stopping_;
      });
    }
    return root_;
  }

  bool Ensure(const std::filesystem::path& destination) {
    std::shared_ptr<const cac_archive::Archive> archive;
    std::filesystem::path cache_base;
    {
      std::lock_guard lock(mutex_);
      archive = archive_;
      cache_base = cache_base_;
    }
    if (archive == nullptr || cache_base.empty()) {
      return false;
    }
    const std::filesystem::path normalized_base =
        std::filesystem::absolute(cache_base).lexically_normal();
    const std::filesystem::path normalized_destination =
        std::filesystem::absolute(destination).lexically_normal();
    const std::filesystem::path relative =
        normalized_destination.lexically_relative(normalized_base);
    if (relative.empty() || relative.is_absolute()) {
      return false;
    }
    for (const auto& component : relative) {
      if (component == "..") {
        return false;
      }
    }
    std::string error;
    if (!archive->Extract(
            relative.generic_string(), normalized_destination,
            error)) {
      REXLOG_WARN(
          "multiplayer-assets: failed to cache local CAC asset "
          "path={} error={}",
          relative.generic_string(), error);
      return false;
    }
    return true;
  }

 private:
  bool Stopping() {
    std::lock_guard lock(mutex_);
    return stopping_;
  }

  void Prepare(
      const std::filesystem::path& archive_path,
      const std::filesystem::path& cache_root) {
    const auto started = std::chrono::steady_clock::now();
    auto archive = std::make_shared<cac_archive::Archive>();
    std::string error;
    if (!archive->Open(archive_path, error)) {
      FinishFailure(archive_path, error);
      return;
    }
    const std::filesystem::path cache_base =
        cache_root / "multiplayer_cac" / archive->CacheKey();
    constexpr std::string_view kModelPrefix =
        "data/content/createacharacter/model/cas_db/";
    const std::vector<std::string> model_paths =
        archive->PathsWithPrefix(kModelPrefix);
    if (model_paths.empty()) {
      FinishFailure(
          archive_path,
          "archive contains no Create-a-Skater models");
      return;
    }

    std::size_t prepared = 0;
    std::size_t reused = 0;
    for (const std::string& path : model_paths) {
      if (Stopping()) {
        FinishStopped();
        return;
      }
      const std::filesystem::path destination =
          cache_base / std::filesystem::path(path);
      std::error_code filesystem_error;
      const bool existed =
          std::filesystem::is_regular_file(
              destination, filesystem_error);
      if (!archive->Extract(path, destination, error)) {
        FinishFailure(archive_path, error);
        return;
      }
      ++prepared;
      reused += existed ? 1 : 0;
    }

    const std::filesystem::path root =
        cache_base / "data" / "content" /
        "createacharacter" / "model" / "cas_db";
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        preparing_ = false;
        ready_.notify_all();
        return;
      }
      cache_base_ = cache_base;
      root_ = root;
      archive_ = std::move(archive);
      preparing_ = false;
    }
    ready_.notify_all();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    REXLOG_INFO(
        "multiplayer-assets: automatic CAC catalogue ready "
        "models={} reused={} prepare={:.3f}ms root={}",
        prepared, reused, elapsed_ms, root.string());
  }

  void FinishFailure(
      const std::filesystem::path& archive_path,
      std::string_view error) {
    {
      std::lock_guard lock(mutex_);
      preparing_ = false;
    }
    ready_.notify_all();
    REXLOG_WARN(
        "multiplayer-assets: automatic CAC catalogue unavailable "
        "archive={} error={}",
        archive_path.string(), error);
  }

  void FinishStopped() {
    {
      std::lock_guard lock(mutex_);
      preparing_ = false;
    }
    ready_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::thread thread_;
  std::filesystem::path root_;
  std::filesystem::path cache_base_;
  std::shared_ptr<const cac_archive::Archive> archive_;
  bool stopping_ = false;
  bool preparing_ = false;
};

Catalogue& GetCatalogue() {
  static Catalogue catalogue;
  return catalogue;
}

}  // namespace

void Start(
    const std::filesystem::path& game_data_root,
    const std::filesystem::path& cache_root) {
  GetCatalogue().Start(game_data_root, cache_root);
}

void Stop() {
  GetCatalogue().Stop();
}

std::filesystem::path Root(bool wait) {
  return GetCatalogue().Root(wait);
}

bool Ensure(const std::filesystem::path& destination) {
  return GetCatalogue().Ensure(destination);
}

}  // namespace skate3::cac_catalogue
