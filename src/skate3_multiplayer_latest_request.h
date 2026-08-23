#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace skate3::multiplayer::worker {

enum class LatestResultStatus {
  kUnknown,
  kPending,
  kReady,
  kFailed,
};

// Generation-safe result handoff for background jobs addressed by a stable
// slot (for example, a player role). Starting a newer key immediately makes
// every older in-flight publication stale.
template <
    typename Slot, typename Key, typename Result,
    typename SlotHash = std::hash<Slot>>
class LatestResultTable {
 public:
  [[nodiscard]] bool Begin(
      const Slot& slot, const Key& key,
      std::shared_ptr<const Result>* displaced = nullptr) {
    std::lock_guard lock(mutex_);
    const auto current = entries_.find(slot);
    if (current != entries_.end() &&
        current->second.key == key) {
      return false;
    }
    if (displaced != nullptr) {
      displaced->reset();
      if (current != entries_.end()) {
        *displaced = std::move(current->second.result);
      }
    }
    entries_.insert_or_assign(slot, Entry{
        key, LatestResultStatus::kPending, nullptr});
    return true;
  }

  [[nodiscard]] bool IsCurrent(
      const Slot& slot, const Key& key) const {
    std::lock_guard lock(mutex_);
    const auto current = entries_.find(slot);
    return current != entries_.end() &&
           current->second.key == key;
  }

  [[nodiscard]] bool Publish(
      const Slot& slot, const Key& key,
      std::shared_ptr<const Result> result) {
    std::lock_guard lock(mutex_);
    const auto current = entries_.find(slot);
    if (current == entries_.end() ||
        !(current->second.key == key)) {
      return false;
    }
    current->second.status =
        result != nullptr ? LatestResultStatus::kReady
                          : LatestResultStatus::kFailed;
    current->second.result = std::move(result);
    return true;
  }

  [[nodiscard]] LatestResultStatus Poll(
      const Slot& slot, const Key& key,
      std::shared_ptr<const Result>& result) const {
    result.reset();
    std::lock_guard lock(mutex_);
    const auto current = entries_.find(slot);
    if (current == entries_.end() ||
        !(current->second.key == key)) {
      return LatestResultStatus::kUnknown;
    }
    result = current->second.result;
    return current->second.status;
  }

  template <typename Predicate>
  [[nodiscard]] bool ForgetIf(
      const Slot& slot, Predicate&& predicate) {
    std::lock_guard lock(mutex_);
    const auto current = entries_.find(slot);
    if (current == entries_.end() ||
        !std::forward<Predicate>(predicate)(current->second.key)) {
      return false;
    }
    entries_.erase(current);
    return true;
  }

  template <typename Predicate>
  [[nodiscard]] std::shared_ptr<const Result> TakeIf(
      const Slot& slot, Predicate&& predicate) {
    std::lock_guard lock(mutex_);
    const auto current = entries_.find(slot);
    if (current == entries_.end() ||
        !std::forward<Predicate>(predicate)(current->second.key)) {
      return nullptr;
    }
    std::shared_ptr<const Result> result =
        std::move(current->second.result);
    entries_.erase(current);
    return result;
  }

 private:
  struct Entry {
    Key key;
    LatestResultStatus status = LatestResultStatus::kPending;
    std::shared_ptr<const Result> result;
  };

  mutable std::mutex mutex_;
  std::unordered_map<Slot, Entry, SlotHash> entries_;
};

}  // namespace skate3::multiplayer::worker
