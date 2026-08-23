#include "skate/world/owned_map_package.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: skate_owned_map_validate <package.skate>\n";
    return 2;
  }
  try {
    const skate::world::MapDefinition map =
        skate::world::LoadOwnedMapPackage(std::filesystem::path(argv[1]));
    skate::world::Vec3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    skate::world::Vec3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (const skate::world::RenderVertex& vertex :
         map.render_mesh.vertices) {
      minimum.x = std::min(minimum.x, vertex.position.x);
      minimum.y = std::min(minimum.y, vertex.position.y);
      minimum.z = std::min(minimum.z, vertex.position.z);
      maximum.x = std::max(maximum.x, vertex.position.x);
      maximum.y = std::max(maximum.y, vertex.position.y);
      maximum.z = std::max(maximum.z, vertex.position.z);
    }
    std::uint64_t texture_bytes = 0;
    for (const skate::world::ImageTexture& texture : map.textures) {
      texture_bytes += texture.rgba8.size();
    }
    std::cout
        << "SKATE_PACKAGE_OK"
        << " name=" << map.name
        << " materials=" << map.materials.size()
        << " textures=" << map.textures.size()
        << " texture_rgba8_bytes=" << texture_bytes
        << " vertices=" << map.render_mesh.vertices.size()
        << " indices=" << map.render_mesh.indices.size()
        << " triangles=" << map.render_mesh.indices.size() / 3
        << " collision=" << map.collision_triangles.size()
        << " rails=" << map.grind_rails.size()
        << " doors=" << map.hinged_doors.size()
        << " lights=" << map.moving_light_orbs.size()
        << " npc_routes=" << map.npc_routes.size()
        << " bounds_min=" << minimum.x << "," << minimum.y << ","
        << minimum.z
        << " bounds_max=" << maximum.x << "," << maximum.y << ","
        << maximum.z
        << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SKATE_PACKAGE_FAIL " << error.what() << '\n';
    return 1;
  }
}
