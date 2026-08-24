#pragma once

#include <filesystem>

namespace skate3::cac_catalogue {

// Parses the user's installed createacharacter.big and prepares the small
// model portion of a persistent receiver-side cache on a background thread.
void Start(
    const std::filesystem::path& game_data_root,
    const std::filesystem::path& cache_root);

void Stop();

// Waiting is reserved for the multiplayer appearance worker. Render-thread
// topology probes use the non-waiting form and retain the proxy until ready.
std::filesystem::path Root(bool wait);

// Lazily extracts an archive-owned texture (or repairs a missing cached
// model) when the compact appearance recipe references it.
bool Ensure(const std::filesystem::path& destination);

}  // namespace skate3::cac_catalogue
