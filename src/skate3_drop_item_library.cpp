#include "skate3_drop_item_library.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace skate3::drop_item_library {
namespace {

std::string Fold(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return value;
}

bool IsSkateObject(const std::filesystem::path& path) {
  return Fold(path.extension().string()) == ".skateobj";
}

}  // namespace

bool Discover(
    const std::filesystem::path& root,
    std::vector<Category>& categories,
    std::string& error) {
  categories.clear();
  error.clear();
  if (root.empty()) {
    error = "object library path is empty";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(root, filesystem_error);
  if (filesystem_error) {
    error =
        "could not create the object library folder: " +
        filesystem_error.message();
    return false;
  }

  std::filesystem::directory_iterator category_iterator(
      root, filesystem_error);
  const std::filesystem::directory_iterator end;
  while (!filesystem_error && category_iterator != end) {
    const std::filesystem::directory_entry entry =
        *category_iterator;
    std::error_code entry_error;
    if (!entry.is_directory(entry_error)) {
      if (entry_error) {
        error =
            "could not inspect object library entry '" +
            entry.path().string() + "': " + entry_error.message();
        return false;
      }
      category_iterator.increment(filesystem_error);
      continue;
    }
    Category category;
    category.name = entry.path().filename().string();
    std::filesystem::directory_iterator item_iterator(
        entry.path(), entry_error);
    while (!entry_error && item_iterator != end) {
      const std::filesystem::directory_entry item = *item_iterator;
      if (item.is_regular_file(entry_error) &&
          !entry_error && IsSkateObject(item.path())) {
        category.files.push_back({category.name, item.path()});
      }
      if (!entry_error) {
        item_iterator.increment(entry_error);
      }
    }
    if (entry_error) {
      error =
          "could not scan object category '" + category.name +
          "': " + entry_error.message();
      return false;
    }
    std::sort(
        category.files.begin(), category.files.end(),
        [](const File& left, const File& right) {
          return Fold(left.path.filename().string()) <
                 Fold(right.path.filename().string());
        });
    categories.push_back(std::move(category));
    category_iterator.increment(filesystem_error);
  }
  if (filesystem_error) {
    error =
        "could not scan object library: " +
        filesystem_error.message();
    return false;
  }
  std::sort(
      categories.begin(), categories.end(),
      [](const Category& left, const Category& right) {
        return Fold(left.name) < Fold(right.name);
      });
  return true;
}

}  // namespace skate3::drop_item_library
