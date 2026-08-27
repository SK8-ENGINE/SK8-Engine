#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace skate3::drop_item_library {

struct File {
  std::string category;
  std::filesystem::path path;
};

struct Category {
  std::string name;
  std::vector<File> files;
};

// Creates the object-library root and discovers its immediate category
// folders. A .skateobj's parent folder is its sole category.
bool Discover(
    const std::filesystem::path& root,
    std::vector<Category>& categories,
    std::string& error);

}  // namespace skate3::drop_item_library
