#pragma once

#include <filesystem>
#include <string>

namespace skate3::vanilla_ui {

// Ensures the first exact retail assets needed by the native menu are present
// in the user-local cache. Inputs are read-only; no retail data is shipped.
bool EnsureRetailAssetBootstrap(const std::filesystem::path &game_root,
                                const std::filesystem::path &cache_root,
                                std::string &error);

std::filesystem::path RetailBackdropPath(
    const std::filesystem::path &cache_root);

} // namespace skate3::vanilla_ui
