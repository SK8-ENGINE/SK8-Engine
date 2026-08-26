#include "skate3_drop_item_import.h"
#include "skate3_drop_item_library.h"

#include <skate/world/skate_object_package.h>
#include <skate/world/rw_collision_mesh.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool Touch(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  return output.put('\0').good();
}

bool TestDiscovery(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::create_directories(root / "Rails", error);
  std::filesystem::create_directories(root / "Plaza" / "Nested", error);
  if (error ||
      !Touch(root / "Rails" / "rail_a.skateobj") ||
      !Touch(root / "Rails" / "rail_b.SKATEOBJ") ||
      !Touch(root / "Plaza" / "not-an-object.txt") ||
      !Touch(root / "Plaza" / "Nested" / "ignored.skateobj")) {
    return false;
  }
  std::vector<skate3::drop_item_library::Category> categories;
  std::string discovery_error;
  if (!skate3::drop_item_library::Discover(
          root, categories, discovery_error)) {
    std::cerr << discovery_error << '\n';
    return false;
  }
  const auto rails = std::find_if(
      categories.begin(), categories.end(),
      [](const auto& category) {
        return category.name == "Rails";
      });
  const auto custom = std::find_if(
      categories.begin(), categories.end(),
      [](const auto& category) {
        return category.name == "Custom";
      });
  const auto plaza = std::find_if(
      categories.begin(), categories.end(),
      [](const auto& category) {
        return category.name == "Plaza";
      });
  return rails != categories.end() && rails->files.size() == 2 &&
         custom != categories.end() && custom->files.empty() &&
         plaza != categories.end() && plaza->files.empty() &&
         categories.back().name == "Custom";
}

bool TestRecipeParser() {
  constexpr std::string_view recipe = R"xml(
<?xml version="1.0"?>
<compositeasset n="fixture" type="Dynamic">
  <mat id="0x0000000000000020" type="dynamicObject">
    <sp id="0x0000000000000030" chn="diffuse" />
  </mat>
  <comp n="DMO_Rails">
    <mod id="0x1" n="aa_Test_Rail">
      <lod idx="0" arenaid="0x0000000000000010">
        <matinst>
          <matvar id="0x0000000000000020" n="fixture" />
        </matinst>
      </lod>
    </mod>
  </comp>
</compositeasset>
)xml";
  skate3::drop_item_import::Recipe parsed;
  std::string error;
  if (!skate3::drop_item_import::ParseRecipe(
          recipe, true, parsed, error) ||
      parsed.items.size() != 1 ||
      parsed.items.front().category != "DMO_Rails" ||
      !parsed.items.front().dynamic ||
      parsed.items.front().model_id != 0x10 ||
      parsed.items.front().material_ids !=
          std::vector<std::uint64_t>{0x20} ||
      parsed.materials.at(0x20).textures.front().texture_id !=
          0x30) {
    std::cerr << "valid recipe rejected: " << error << '\n';
    return false;
  }
  std::string unsafe(recipe);
  const std::size_t category = unsafe.find("DMO_Rails");
  unsafe.replace(category, std::string("DMO_Rails").size(), "../escape");
  if (skate3::drop_item_import::ParseRecipe(
          unsafe, true, parsed, error) ||
      error.find("unsafe") == std::string::npos) {
    std::cerr << "unsafe recipe category was accepted\n";
    return false;
  }
  return true;
}

bool TestRetailMaterialClassification() {
  using skate::world::RetailShaderFamily;
  return skate3::drop_item_import::ShaderFamilyForMaterialType(
             "environmentParkDiffuse") ==
             RetailShaderFamily::DynamicObject &&
         skate3::drop_item_import::ShaderFamilyForMaterialType(
             "dynamicObject") ==
             RetailShaderFamily::DynamicObject &&
         skate3::drop_item_import::ShaderFamilyForMaterialType(
             "environmentParkAlphaTest") ==
             RetailShaderFamily::DynamicObjectAlphaTest &&
         skate3::drop_item_import::ShaderFamilyForMaterialType(
             "dynamicObjectAlphaTest") ==
             RetailShaderFamily::DynamicObjectAlphaTest &&
         skate3::drop_item_import::ShaderFamilyForMaterialType(
             "environmentParkDecal") ==
             RetailShaderFamily::DynamicObjectDecal &&
         skate3::drop_item_import::RetailTextureSemantic(
             "detailnormal") == "detail" &&
         skate3::drop_item_import::RetailTextureSemantic(
             "transparency") == "transparent";
}

bool TestMissingGameData(const std::filesystem::path& root) {
  const auto result = skate3::drop_item_import::ImportDefaults(
      root / "missing-game", root / "objects");
  return result.written == 0 && !result.errors.empty() &&
         result.errors.front().find("parkassets.big") !=
             std::string::npos;
}

bool TestDiscoveryFailure(const std::filesystem::path& root) {
  if (!Touch(root)) {
    return false;
  }
  std::vector<skate3::drop_item_library::Category> categories;
  std::string error;
  return !skate3::drop_item_library::Discover(
             root, categories, error) &&
         !error.empty() && categories.empty();
}

bool TestPackageRoundTrip(const std::filesystem::path& root) {
  skate::world::SkateObjectAsset asset;
  asset.format_version = 2;
  asset.name = "Round trip";
  skate::world::SurfaceMaterial material;
  material.id = 1;
  material.name = "Concrete";
  asset.materials.push_back(material);
  skate::world::MapObject object;
  object.id = 1;
  object.name = "Root";
  object.physics.type =
      skate::world::ObjectPhysicsType::Static;
  for (const skate::world::Vec3 point :
       {skate::world::Vec3{0.0f, 0.0f, 0.0f},
        skate::world::Vec3{1.0f, 0.0f, 0.0f},
        skate::world::Vec3{0.0f, 0.0f, 1.0f}}) {
    skate::world::RenderVertex vertex;
    vertex.position = point;
    vertex.normal = {0.0f, 1.0f, 0.0f};
    vertex.material = 1;
    object.render_mesh.vertices.push_back(vertex);
  }
  object.render_mesh.indices = {0, 1, 2};
  skate::world::CollisionTriangle collision;
  collision.a = {0.0f, 0.0f, 0.0f};
  collision.b = {1.0f, 0.0f, 0.0f};
  collision.c = {0.0f, 0.0f, 1.0f};
  collision.material = 1;
  collision.surface = 1;
  object.collision_triangles.push_back(collision);
  skate::world::GrindRail rail;
  rail.id = 1;
  rail.name = "Retail cubic";
  rail.retail_spline_id = 0x1122334455667788ull;
  rail.retail_type_signature = 0x8877665544332211ull;
  rail.retail_flags = 0x12345678u;
  rail.retail_trailing_word = 0x13572468u;
  rail.native_segments.resize(1);
  rail.native_segments.front().words[8] =
      std::bit_cast<std::uint32_t>(1.0f);
  rail.native_segments.front().words[12] =
      std::bit_cast<std::uint32_t>(2.0f);
  rail.native_segments.front().words[20] =
      std::bit_cast<std::uint32_t>(2.0f);
  rail.native_segments.front().words[24] =
      std::bit_cast<std::uint32_t>(3.0f);
  asset.grind_rails.push_back(std::move(rail));
  object.grind_rail_indices = {0};
  asset.objects.push_back(std::move(object));
  const std::filesystem::path output =
      root / "objects" / "Custom" / "round-trip.skateobj";
  try {
    skate::world::SaveSkateObjectPackage(output, asset);
    const auto loaded =
        skate::world::LoadSkateObjectPackage(output);
    return loaded.name == asset.name &&
           loaded.objects.size() == 1 &&
           loaded.objects.front().render_mesh.indices.size() == 3 &&
           loaded.objects.front().collision_triangles.size() == 1 &&
           loaded.grind_rails.size() == 1 &&
           loaded.grind_rails.front().retail_spline_id ==
               0x1122334455667788ull &&
           loaded.grind_rails.front().native_segments.size() == 1 &&
           loaded.grind_rails.front().native_segments.front().words[12] ==
               std::bit_cast<std::uint32_t>(2.0f);
  } catch (const std::exception& exception) {
    std::cerr << "package round trip failed: "
              << exception.what() << '\n';
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "skate3-drop-item-library-tests";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  if (!TestDiscovery(root / "discovery") ||
      !TestRecipeParser() ||
      !TestRetailMaterialClassification() ||
      !TestMissingGameData(root) ||
      !TestDiscoveryFailure(root / "not-a-directory") ||
      !TestPackageRoundTrip(root)) {
    return 1;
  }
  if (argc >= 3) {
    const std::filesystem::path game_data_root = argv[1];
    const std::filesystem::path output_root = argv[2];
    const auto result =
        skate3::drop_item_import::ImportDefaults(
            game_data_root, output_root,
            [](const skate3::drop_item_import::Progress& progress) {
              if (progress.total != 0 &&
                  progress.completed == progress.total) {
                std::cout << "IMPORT " << progress.completed << "/"
                          << progress.total << " written="
                          << progress.written << " reused="
                          << progress.reused << " unsupported="
                          << progress.unsupported << " "
                          << progress.message << '\n';
              }
            });
    if (!result.errors.empty() || result.completed != result.total ||
        result.unsupported != 9 ||
        result.written + result.reused != 652) {
      for (const std::string& import_error : result.errors) {
        std::cerr << import_error << '\n';
      }
      std::cerr << "installed park-item conversion mismatch: completed="
                << result.completed << "/" << result.total
                << " written=" << result.written
                << " reused=" << result.reused
                << " unsupported=" << result.unsupported << '\n';
      return 1;
    }
    const auto repeated =
        skate3::drop_item_import::ImportDefaults(
            game_data_root, output_root);
    if (!repeated.errors.empty() ||
        repeated.completed != repeated.total ||
        repeated.written != 0 || repeated.reused != 652 ||
        repeated.unsupported != 9) {
      std::cerr << "installed park-item import is not idempotent: written="
                << repeated.written << " reused=" << repeated.reused
                << " unsupported=" << repeated.unsupported << '\n';
      return 1;
    }
    std::vector<skate3::drop_item_library::Category> categories;
    std::string discovery_error;
    if (!skate3::drop_item_library::Discover(
            output_root, categories, discovery_error)) {
      std::cerr << discovery_error << '\n';
      return 1;
    }
    std::size_t checked_assets = 0;
    for (const auto& category : categories) {
      if (category.name == "Custom") {
        continue;
      }
      for (const auto& entry : category.files) {
        const std::filesystem::path& file = entry.path;
        const auto asset =
            skate::world::LoadSkateObjectPackage(file);
        for (const auto& object : asset.objects) {
          if (object.physics.type !=
              skate::world::ObjectPhysicsType::Disabled) {
            std::cerr << file
                      << " retained Box3D physics metadata\n";
            return 1;
          }
          if (!object.grind_rail_indices.empty() &&
              asset.grind_rails.empty()) {
            std::cerr << file
                      << " lost referenced grind splines\n";
            return 1;
          }
        }
        for (const auto& material : asset.materials) {
          if (material.retail.enabled &&
              material.retail.shader_family !=
                  skate3::drop_item_import::
                      ShaderFamilyForMaterialType(
                          material.retail.shader_name)) {
            std::cerr << file
                      << " has stale retail shader routing\n";
            return 1;
          }
          if (!material.retail.enabled) {
            const std::uint16_t encoded =
                skate::world::EncodeRwSurfaceId(
                    material.skate_audio_surface,
                    material.skate_physics_surface,
                    material.skate_surface_pattern);
            char expected[7] = {};
            std::snprintf(
                expected, sizeof(expected), "0x%04x", encoded);
            if (material.name.find(expected) ==
                std::string::npos) {
              std::cerr << file
                        << " did not retain its native collision "
                           "audio/physics/pattern ID\n";
              return 1;
            }
          }
        }
        ++checked_assets;
      }
    }
    if (checked_assets != 652) {
      std::cerr << "expected to validate 652 generated assets, got "
                << checked_assets << '\n';
      return 1;
    }
  }
  std::filesystem::remove_all(root, error);
  std::cout << "skate3_drop_item_library_tests passed\n";
  return 0;
}
