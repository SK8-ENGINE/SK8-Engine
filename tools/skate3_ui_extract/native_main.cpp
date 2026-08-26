#include "skate3_vanilla_ui/skate3_ui_asset_cache.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage: skate3_ui_asset_bootstrap <game-root> <cache-root>\n";
    return 2;
  }
  std::string error;
  const std::filesystem::path game_root = argv[1];
  const std::filesystem::path cache_root = argv[2];
  if (!skate3::vanilla_ui::EnsureRetailAssetBootstrap(game_root, cache_root,
                                                       error)) {
    std::cerr << "UI asset bootstrap failed: " << error << '\n';
    return 1;
  }
  std::cout << "Exact retail UI bootstrap asset is ready:\n"
            << skate3::vanilla_ui::RetailBackdropPath(cache_root).string()
            << '\n';
  return 0;
}
