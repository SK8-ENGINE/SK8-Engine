#pragma once

#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace skate3::multiplayer::worker {

// Thread-safe latest-wins handoff for immutable local captures and prepared
// remote presentations. Superseded local captures may be dropped, while
// one-shot lifecycle retirements accumulate until the renderer consumes them.
template <typename Input, typename Player, typename Retirement>
class LatestFrameMailbox {
 public:
  [[nodiscard]] bool PublishInput(
      std::shared_ptr<const Input> input) {
    std::scoped_lock lock(input_mutex_);
    const bool replaced = pending_input_ != nullptr;
    pending_input_ = std::move(input);
    return replaced;
  }

  [[nodiscard]] bool HasPendingInput() const {
    std::scoped_lock lock(input_mutex_);
    return pending_input_ != nullptr;
  }

  [[nodiscard]] std::shared_ptr<const Input> TakeInput() {
    std::scoped_lock lock(input_mutex_);
    return std::exchange(pending_input_, nullptr);
  }

  [[nodiscard]] std::uint64_t PublishPresentation(
      std::shared_ptr<const std::vector<Player>> players,
      std::vector<Retirement> retirements) {
    std::scoped_lock lock(output_mutex_);
    latest_players_ = std::move(players);
    pending_retirements_.insert(
        pending_retirements_.end(),
        std::make_move_iterator(retirements.begin()),
        std::make_move_iterator(retirements.end()));
    return ++output_sequence_;
  }

  [[nodiscard]] bool ConsumePresentation(
      std::uint64_t& sequence,
      std::shared_ptr<const std::vector<Player>>& players,
      std::vector<Retirement>& retirements) {
    std::scoped_lock lock(output_mutex_);
    sequence = output_sequence_;
    players = latest_players_;
    retirements.clear();
    retirements.swap(pending_retirements_);
    return players != nullptr && !players->empty();
  }

  void Clear() {
    {
      std::scoped_lock lock(input_mutex_);
      pending_input_.reset();
    }
    {
      std::scoped_lock lock(output_mutex_);
      latest_players_.reset();
      pending_retirements_.clear();
      output_sequence_ = 0;
    }
  }

 private:
  mutable std::mutex input_mutex_;
  std::shared_ptr<const Input> pending_input_;

  std::mutex output_mutex_;
  std::shared_ptr<const std::vector<Player>> latest_players_;
  std::vector<Retirement> pending_retirements_;
  std::uint64_t output_sequence_ = 0;
};

}  // namespace skate3::multiplayer::worker
