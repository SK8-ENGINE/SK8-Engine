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

class CheckGate {
public:
  [[nodiscard]] bool Request() {
    const bool newly_requested = !requested_;
    requested_ = true;
    return newly_requested;
  }

  [[nodiscard]] bool Consume() {
    const bool requested = requested_;
    requested_ = false;
    return requested;
  }

private:
  // Check once when monitoring starts. Later checks are explicitly requested
  // by the user rather than retried forever in the background.
  bool requested_ = true;
};

} // namespace skate3::multiplayer::steam::availability
