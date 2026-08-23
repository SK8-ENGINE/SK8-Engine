cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(skate_owned_world LANGUAGES C CXX)
  include(CTest)
endif()

if(NOT TARGET ZLIB::ZLIBSTATIC)
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/zlib"
    "${CMAKE_CURRENT_BINARY_DIR}/zlib"
    EXCLUDE_FROM_ALL
  )
endif()

add_library(skate_owned_world STATIC
  src/grind_spline.cpp
  src/owned_map_package.cpp
  src/render_world.cpp
  src/rw_collision_mesh.cpp
  src/starter_flatground.cpp
  src/water_simulation.cpp
  src/world_map.cpp
)

target_link_libraries(skate_owned_world PRIVATE ZLIB::ZLIBSTATIC)

target_include_directories(skate_owned_world
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(skate_owned_world PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(skate_owned_world PRIVATE /W4 /permissive-)
else()
  target_compile_options(skate_owned_world PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(SKATE_OWNED_WORLD_BUILD_TESTS "Build owned world tests" ON)

if(SKATE_OWNED_WORLD_BUILD_TESTS)
  add_executable(skate_owned_world_tests
    tests/world_map_tests.cpp
  )

  target_link_libraries(skate_owned_world_tests PRIVATE skate_owned_world)
  target_compile_features(skate_owned_world_tests PRIVATE cxx_std_20)

  add_executable(skate_owned_map_validate
    tests/owned_map_validate.cpp
  )

  target_link_libraries(skate_owned_map_validate PRIVATE skate_owned_world)
  target_compile_features(skate_owned_map_validate PRIVATE cxx_std_20)

  add_executable(skate_rw_collision_archive_validate
    tests/rw_collision_archive_validate.cpp
  )

  target_link_libraries(
    skate_rw_collision_archive_validate PRIVATE skate_owned_world
  )
  target_compile_features(
    skate_rw_collision_archive_validate PRIVATE cxx_std_20
  )

  add_test(NAME skate_owned_world_tests COMMAND skate_owned_world_tests)
endif()
