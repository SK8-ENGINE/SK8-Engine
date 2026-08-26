#pragma once

#include <cstdint>

namespace skate3::native_grind {

enum class RegistrationDecision : std::uint8_t {
  AllowVanilla = 0,
  SuppressVanilla,
  AllowOwned,
};

constexpr RegistrationDecision DecideRegistration(
    bool installing_owned,
    bool original_world_replacement_requested) noexcept {
  if (installing_owned) {
    return RegistrationDecision::AllowOwned;
  }
  return original_world_replacement_requested
             ? RegistrationDecision::SuppressVanilla
             : RegistrationDecision::AllowVanilla;
}

}  // namespace skate3::native_grind
