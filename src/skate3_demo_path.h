#pragma once

#include <cstdint>
#include <functional>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::input {
class InputSystem;
}

namespace skate3::demo_path {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);
// Provider for the merged UI pad state consumed by the user-facing frontend
// movie skip. Set during app setup, before the guest boots (the provider is
// read from the guest thread without further synchronization).
void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider);
// The provider's current input system, or null before app setup. For other
// host features that poll the merged UI pad state.
rex::input::InputSystem* GetUiInputSystem();
bool ShouldForceIntroMovieComplete();
// True from process boot until the game reaches gameplay in direct-boot mode.
// The renderer uses this to replace frontend scenes with its native loading
// frame while profile/save services initialize.
bool DirectBootLoadingVisualActive();
void ObserveFrontEndState(uint32_t manager, uint32_t state_id,
                          uint32_t mode, uint32_t caller_lr);
uint32_t AutomationStage();
uint32_t LastRequestedFrontEndState();
bool SeenLanguageUpdate();

}  // namespace skate3::demo_path
