#pragma once

#include <cstdint>
#include <iosfwd>

struct PPCContext;

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::scoring {

// Installs a transparent observer at the ScoringTrick state-graph action.
// Retail behavior always runs unchanged after its context has been captured.
void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);

// Called by the generated-function prologue patch. Direct generated calls do
// not pass through FunctionDispatcher, so this is the authoritative boundary.
void ObserveExecuteTrick(PPCContext& ctx, uint8_t* base);

// Observation windows are coordinated with the autonomous input harness.
void ResetAndArm();
void SetFocus(bool focused);
void AppendObservationFields(std::ostream& response);

}  // namespace skate3::scoring
