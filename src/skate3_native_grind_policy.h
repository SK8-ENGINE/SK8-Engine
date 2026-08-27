#pragma once

#include <cstdint>

namespace skate3::native_grind {

enum class RegistrationDecision : std::uint8_t {
  AllowVanilla = 0,
  SuppressVanilla,
  AllowOwned,
};

// The streamer unloads GrindData runtime objects by the 64-bit identity
// stored at offset zero. Reusing the intercepted retail asset identity makes
// the owned runtime disappear when that retail streaming cell unloads.
constexpr std::uint64_t OwnedRuntimeIdentity(
    std::uint64_t retail_identity) noexcept {
  constexpr std::uint64_t kOwnedIdentitySalt =
      0x534B384752494E44ull;  // "SK8GRIND"
  const std::uint64_t owned_identity =
      retail_identity ^ kOwnedIdentitySalt;
  return owned_identity != 0 ? owned_identity
                             : ~kOwnedIdentitySalt;
}

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
