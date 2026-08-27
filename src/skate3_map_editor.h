#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace skate3::map_editor {

void SetWindowHandle(void* window);
void ConfigureObjectLibrary(
    std::filesystem::path game_data_root,
    std::filesystem::path object_library_root);
void Toggle();
void SetActive(bool active);
bool Active();
void ToggleSpawnMenu();
bool SpawnMenuVisible();
struct SpawnObjectEntry {
  std::size_t asset_index = 0;
  std::string category;
  std::string name;
};
std::vector<SpawnObjectEntry> SpawnObjectEntries();
// Rescans category folders while the menu is open. Existing spawned
// instances remain independent in the current world.
bool RefreshSpawnObjects();
bool QueueSpawnObject(std::size_t asset_index);

enum class DefaultLibraryState {
  NotStarted,
  Running,
  Complete,
  Failed,
};

struct DefaultLibraryStatus {
  DefaultLibraryState state = DefaultLibraryState::NotStarted;
  std::size_t completed = 0;
  std::size_t total = 0;
  std::size_t written = 0;
  std::size_t reused = 0;
  std::size_t unsupported = 0;
  std::string message;
  std::vector<std::string> errors;
};

bool StartDefaultLibraryImport();
DefaultLibraryStatus GetDefaultLibraryStatus();
// Consumed by the native collision update so scene mutation and collision
// registration happen on the same authoritative emulation thread.
bool ApplyPendingSpawn();

// True only while the game window owns foreground keyboard/mouse focus.
// Editor and free-camera polling both use this so GetAsyncKeyState cannot
// consume desktop input while the user is working in another application.
bool HasInputFocus();

// Called from the free-camera owner. Holding RMB captures/hides the cursor;
// release restores it immediately. While held this recentres the cursor and
// returns raw radians to compose into camera look.
bool ConsumeMouseLook(double& yaw_radians, double& pitch_radians);

// Samples the authoritative local board state once per mechanics frame and
// emits transition telemetry, including rapid off-board -> on-board returns.
void ObservePlayerState() noexcept;

// Called once the frame's final free-camera view/projection are known.
void UpdateInteraction(const float view[16], const float projection[16],
                       const float camera_position[3]);

// Map-local authoritative pose (authored origin + session delta and an
// orthonormal basis). Rendering adds the installed map-world origin; native
// collision and grind paths use the same pose and revision.
bool ObjectTransform(std::size_t index, float out_translation[3],
                     std::uint64_t* out_revision = nullptr);
bool ObjectTransform(std::size_t index, float out_translation[3],
                     float out_basis[9],
                     std::uint64_t* out_revision = nullptr);

bool IsSelected(std::size_t index);
std::size_t SelectedObject();
int ActiveGizmoHandle();
std::uint64_t TransformCommitSerial();
void AppendTelemetry(std::ostream& out);

}  // namespace skate3::map_editor
