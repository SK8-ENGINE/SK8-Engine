#pragma once

#include <filesystem>
#include <string>

namespace skate3::release_update {

std::string Sha256OfFile(const std::filesystem::path& path);

} // namespace skate3::release_update
