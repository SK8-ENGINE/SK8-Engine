#pragma once

namespace skate3::multiplayer::steam::availability {

enum class Status {
  kUnknown,
  kUnavailable,
  kAvailable,
};

enum class Transition {
  kNone,
  kBecameUnavailable,
  kBecameAvailable,
};

class Tracker {
public:
  [[nodiscard]] Transition Observe(bool integration_ready) {
    const Status next =
        integration_ready ? Status::kAvailable : Status::kUnavailable;
    if (next == status_) {
      return Transition::kNone;
    }
    status_ = next;
    return integration_ready ? Transition::kBecameAvailable
                             : Transition::kBecameUnavailable;
  }

  [[nodiscard]] bool available() const { return status_ == Status::kAvailable; }

private:
  Status status_ = Status::kUnknown;
};

} // namespace skate3::multiplayer::steam::availability
