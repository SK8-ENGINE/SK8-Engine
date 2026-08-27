#include "skate3_native_grind_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using skate3::native_grind::DecideRegistration;
  using skate3::native_grind::OwnedRuntimeIdentity;
  using skate3::native_grind::RegistrationDecision;

  Require(DecideRegistration(false, false) ==
              RegistrationDecision::AllowVanilla,
          "vanilla grind registration remains available outside replacement");
  Require(DecideRegistration(false, true) ==
              RegistrationDecision::SuppressVanilla,
          "vanilla grind registration is suppressed for owned-world replacement");
  Require(DecideRegistration(true, true) ==
              RegistrationDecision::AllowOwned,
          "owned grind registration bypasses vanilla suppression");
  Require(DecideRegistration(true, false) ==
              RegistrationDecision::AllowOwned,
          "owned registration never depends on retail replacement state");

  constexpr std::uint64_t retail_identity =
      0xFDA502C0276F1490ull;
  constexpr std::uint64_t owned_identity =
      OwnedRuntimeIdentity(retail_identity);
  Require(owned_identity != 0 &&
              owned_identity != retail_identity,
          "owned runtime identity must not match a retail unload identity");
  Require(OwnedRuntimeIdentity(retail_identity) ==
              owned_identity,
          "owned runtime identity must be deterministic");
  Require(OwnedRuntimeIdentity(0x534B384752494E44ull) != 0,
          "owned runtime identity fallback must remain valid");

  std::cout << "native grind registration policy tests passed\n";
  return 0;
}
