#include "skate/world/owned_map_package.h"
#include "skate/world/rw_collision_mesh.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr float kCellSize = 256.0f;
constexpr std::size_t kMaximumTrianglesPerChunk = 400000;
constexpr std::size_t kMaximumChunks = 1024;
constexpr std::uint32_t kDefaultWorkerCount = 8;

using Cell = std::pair<std::int32_t, std::int32_t>;

struct ChunkJob {
  std::string name;
  const std::vector<skate::world::CollisionTriangle>* triangles = nullptr;
  std::size_t first = 0;
  std::size_t count = 0;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "RW_COLLISION_ARCHIVE_BUILD_FAIL " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void WriteLeU32(std::ostream& stream, std::uint32_t value) {
  const char bytes[4] = {
      static_cast<char>(value & 0xFFu),
      static_cast<char>((value >> 8u) & 0xFFu),
      static_cast<char>((value >> 16u) & 0xFFu),
      static_cast<char>((value >> 24u) & 0xFFu),
  };
  stream.write(bytes, sizeof(bytes));
}

std::filesystem::path DefaultOutputPath(
    const std::filesystem::path& package) {
  std::filesystem::path output = package;
  output.replace_extension(".spawn-collision.rwcmset");
  return output;
}

std::uint32_t ParseWorkerCount(const char* value) {
  if (value == nullptr) {
    const std::uint32_t available =
        std::max(1u, std::thread::hardware_concurrency());
    return std::min(available, kDefaultWorkerCount);
  }
  try {
    const unsigned long parsed = std::stoul(value);
    if (parsed == 0 || parsed > 64) {
      Fail("worker count must be between 1 and 64");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (const std::exception&) {
    Fail("worker count is invalid");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr
        << "usage: skate_rw_collision_archive_build <map.skate> "
           "[output.rwcmset] [workers]\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path package =
      std::filesystem::absolute(argv[1]);
  const std::filesystem::path output =
      argc >= 3 ? std::filesystem::absolute(argv[2])
                : DefaultOutputPath(package);
  const std::uint32_t worker_count =
      ParseWorkerCount(argc >= 4 ? argv[3] : nullptr);

  std::cout << "RW_COLLISION_ARCHIVE_PROGRESS stage=load-package "
            << "path=\"" << package.string() << "\"\n";
  skate::world::MapDefinition map;
  try {
    map = skate::world::LoadOwnedMapPackage(package);
  } catch (const std::exception& error) {
    Fail(std::string("package load failed: ") + error.what());
  }

  std::cout << "RW_COLLISION_ARCHIVE_PROGRESS stage=partition "
            << "triangles=" << map.collision_triangles.size() << '\n';
  std::map<Cell, std::vector<skate::world::CollisionTriangle>> cells;
  for (const skate::world::CollisionTriangle& triangle :
       map.collision_triangles) {
    const float center_x =
        (triangle.a.x + triangle.b.x + triangle.c.x) / 3.0f;
    const float center_z =
        (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0f;
    const Cell cell{
        static_cast<std::int32_t>(std::floor(center_x / kCellSize)),
        static_cast<std::int32_t>(std::floor(center_z / kCellSize)),
    };
    cells[cell].push_back(triangle);
  }

  std::vector<ChunkJob> jobs;
  for (const auto& [cell, triangles] : cells) {
    for (std::size_t first = 0; first < triangles.size();
         first += kMaximumTrianglesPerChunk) {
      const std::size_t count = std::min(
          kMaximumTrianglesPerChunk, triangles.size() - first);
      jobs.push_back({
          "owned_collision_" + std::to_string(cell.first) + "_" +
              std::to_string(cell.second) + "_" +
              std::to_string(first / kMaximumTrianglesPerChunk),
          &triangles,
          first,
          count,
      });
    }
  }
  if (jobs.empty() || jobs.size() > kMaximumChunks) {
    Fail("spatial partition produced " + std::to_string(jobs.size()) +
         " chunks; expected 1-" + std::to_string(kMaximumChunks));
  }

  skate::world::RwCollisionBuildOptions options;
  options.default_surface_id =
      skate::world::EncodeRwSurfaceId(3, 1, 0);
  for (const skate::world::SurfaceMaterial& material : map.materials) {
    options.material_surface_ids.emplace(
        material.id,
        skate::world::EncodeRwSurfaceId(
            material.skate_audio_surface,
            material.skate_physics_surface,
            material.skate_surface_pattern));
  }

  std::vector<skate::world::RwCollisionBuildResult> results(jobs.size());
  std::atomic<std::size_t> next_job{0};
  std::atomic<std::size_t> completed{0};
  std::atomic<bool> failed{false};
  std::mutex output_mutex;
  std::string first_error;
  std::cout << "RW_COLLISION_ARCHIVE_PROGRESS stage=compile "
            << "chunks=" << jobs.size()
            << " workers=" << std::min<std::size_t>(
                   worker_count, jobs.size())
            << '\n';

  const auto worker = [&] {
    while (!failed.load(std::memory_order_acquire)) {
      const std::size_t index =
          next_job.fetch_add(1, std::memory_order_relaxed);
      if (index >= jobs.size()) {
        return;
      }
      const ChunkJob& job = jobs[index];
      skate::world::MapDefinition chunk;
      chunk.name = job.name;
      chunk.collision_triangles.insert(
          chunk.collision_triangles.end(),
          job.triangles->begin() +
              static_cast<std::ptrdiff_t>(job.first),
          job.triangles->begin() +
              static_cast<std::ptrdiff_t>(job.first + job.count));
      results[index] =
          skate::world::BuildRwCollisionMesh(chunk, options);
      if (!results[index].ok || results[index].mesh.bytes.empty()) {
        std::scoped_lock lock(output_mutex);
        if (first_error.empty()) {
          first_error =
              job.name + ": " +
              (results[index].error.empty()
                   ? "empty native collision mesh"
                   : results[index].error);
        }
        failed.store(true, std::memory_order_release);
        return;
      }
      const std::size_t done =
          completed.fetch_add(1, std::memory_order_relaxed) + 1;
      if (done == jobs.size() || done % 8 == 0) {
        std::scoped_lock lock(output_mutex);
        std::cout << "RW_COLLISION_ARCHIVE_PROGRESS stage=compile "
                  << "completed=" << done
                  << " total=" << jobs.size()
                  << " percent=" << (done * 100 / jobs.size())
                  << '\n';
      }
    }
  };

  std::vector<std::thread> workers;
  const std::size_t actual_worker_count =
      std::min<std::size_t>(worker_count, jobs.size());
  workers.reserve(actual_worker_count);
  for (std::size_t index = 0; index < actual_worker_count; ++index) {
    workers.emplace_back(worker);
  }
  for (std::thread& thread : workers) {
    thread.join();
  }
  if (failed.load(std::memory_order_acquire)) {
    Fail(first_error.empty() ? "collision compilation failed" : first_error);
  }

  std::error_code directory_error;
  std::filesystem::create_directories(
      output.parent_path(), directory_error);
  if (directory_error) {
    Fail("could not create output directory: " +
         directory_error.message());
  }
  std::filesystem::path temporary = output;
  temporary += ".tmp";
  std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
  if (!stream) {
    Fail("could not create temporary archive: " + temporary.string());
  }
  constexpr char kMagic[8] = {
      'R', 'W', 'C', 'M', 'S', 'E', 'T', '1'};
  stream.write(kMagic, sizeof(kMagic));
  WriteLeU32(stream, static_cast<std::uint32_t>(jobs.size()));
  std::uint64_t payload_bytes = 0;
  std::uint64_t triangle_count = 0;
  for (std::size_t index = 0; index < jobs.size(); ++index) {
    const std::string& name = jobs[index].name;
    const std::vector<std::uint8_t>& bytes =
        results[index].mesh.bytes;
    WriteLeU32(stream, static_cast<std::uint32_t>(name.size()));
    stream.write(name.data(), static_cast<std::streamsize>(name.size()));
    WriteLeU32(stream, static_cast<std::uint32_t>(bytes.size()));
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    payload_bytes += bytes.size();
    triangle_count += results[index].mesh.triangle_count;
  }
  stream.close();
  if (!stream) {
    std::filesystem::remove(temporary);
    Fail("could not write complete collision archive");
  }
  std::error_code copy_error;
  std::filesystem::copy_file(
      temporary, output,
      std::filesystem::copy_options::overwrite_existing,
      copy_error);
  std::filesystem::remove(temporary);
  if (copy_error) {
    Fail("could not publish collision archive: " +
         copy_error.message());
  }

  std::cout << "RW_COLLISION_ARCHIVE_BUILD_OK"
            << " output=\"" << output.string() << "\""
            << " chunks=" << jobs.size()
            << " triangles=" << triangle_count
            << " payload_bytes=" << payload_bytes << '\n';
  return EXIT_SUCCESS;
}
