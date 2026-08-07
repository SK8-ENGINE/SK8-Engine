#pragma once

struct PPCContext;

#include <cstdint>

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::input_lab {

// Hooks the active state-graph ground predicate. Its fields are authoritative
// per actor; STATUS retains the most recently evaluated actor for legacy
// readiness checks, while observation telemetry tracks each actor separately.
void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);

// Starts the local controller-automation command server. On Windows it listens
// on \\.\pipe\Skate3InputLab; commands affect guest controller 0 regardless of
// which host window has focus.
void Install();

// Called from the reconstructed LocalPlayer factory after its subclass
// constructor has installed the final vtable.
void ObserveLocalPlayerCreated(uint8_t* base, uint32_t object);

// Called at the processed-input coordinator entry. Index 0 is the primary
// gameplay input lane; the address is retained for read-only ownership
// correlation with state-graph objects.
void ObserveProcessedInputCoordinator(uint8_t* base, uint32_t coordinator,
                                      uint32_t player_index);

// Returns the actor most recently observed by the authoritative player-state
// predicate, and resolves a PlayAnimation state context back to one of those
// independently observed actors. These are used to avoid dispatching a
// player-only custom asset on the first unrelated NPC animation controller.
uint32_t CurrentObservedPlayerEntity();
uint32_t ResolveAnimationContextActor(uint8_t* base,
                                      uint32_t state_context);
bool AnimationContextReferences(uint8_t* base, uint32_t state_context,
                                uint32_t value);
bool GuestObjectReferences(uint8_t* base, uint32_t root, uint32_t target);

// Called by the existing PlayAnimation behavior hook after the retail function
// selects its final 24-byte animation intent on the guest stack.
void ObservePlayAnimation(PPCContext& ctx, uint8_t* base,
                          uint32_t selected_intent,
                          uint32_t behavior_context,
                          uint32_t state_context,
                          uint32_t animation_controller);

}  // namespace skate3::input_lab
