cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(skate3_multiplayer_tests LANGUAGES CXX)
  include(CTest)
  set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(
    ../third_party/rexglue-sdk/thirdparty/snappy
    ${CMAKE_CURRENT_BINARY_DIR}/snappy
  )
endif()

add_executable(skate3_multiplayer_protocol_tests
  skate3_multiplayer_protocol_tests.cpp
)

target_include_directories(skate3_multiplayer_protocol_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(skate3_multiplayer_protocol_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_protocol_tests
  COMMAND skate3_multiplayer_protocol_tests
)

add_executable(skate3_multiplayer_protocol_v12_tests
  skate3_multiplayer_protocol_v12_tests.cpp
)

target_include_directories(
  skate3_multiplayer_protocol_v12_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_protocol_v12_tests
  COMMAND skate3_multiplayer_protocol_v12_tests
)

add_executable(skate3_multiplayer_protocol_v12_state_tests
  skate3_multiplayer_protocol_v12_state_tests.cpp
)

target_include_directories(
  skate3_multiplayer_protocol_v12_state_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_state_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_state_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_state_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_protocol_v12_state_tests
  COMMAND skate3_multiplayer_protocol_v12_state_tests
)

add_executable(skate3_multiplayer_protocol_v12_pose_tests
  skate3_multiplayer_protocol_v12_pose_tests.cpp
)

target_include_directories(
  skate3_multiplayer_protocol_v12_pose_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_pose_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_pose_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_pose_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_protocol_v12_pose_tests
  COMMAND skate3_multiplayer_protocol_v12_pose_tests
)

add_executable(skate3_multiplayer_protocol_v12_transport_tests
  skate3_multiplayer_protocol_v12_transport_tests.cpp
)

target_include_directories(
  skate3_multiplayer_protocol_v12_transport_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_transport_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_transport_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_transport_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_protocol_v12_transport_tests
  COMMAND skate3_multiplayer_protocol_v12_transport_tests
)

add_executable(skate3_multiplayer_protocol_v12_lossless_tests
  skate3_multiplayer_protocol_v12_lossless_tests.cpp
)
target_include_directories(
  skate3_multiplayer_protocol_v12_lossless_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_lossless_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_lossless_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_lossless_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_protocol_v12_lossless_tests
  COMMAND skate3_multiplayer_protocol_v12_lossless_tests
)

add_executable(skate3_multiplayer_protocol_v12_quaternion_tests
  skate3_multiplayer_protocol_v12_quaternion_tests.cpp
)
target_include_directories(
  skate3_multiplayer_protocol_v12_quaternion_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_quaternion_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_quaternion_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_quaternion_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_protocol_v12_quaternion_tests
  COMMAND skate3_multiplayer_protocol_v12_quaternion_tests
)

add_executable(skate3_multiplayer_protocol_v12_delta_tests
  skate3_multiplayer_protocol_v12_delta_tests.cpp
)
target_include_directories(
  skate3_multiplayer_protocol_v12_delta_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_delta_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_delta_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_delta_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_protocol_v12_delta_tests
  COMMAND skate3_multiplayer_protocol_v12_delta_tests
)

add_executable(skate3_multiplayer_protocol_v12_codec_benchmarks
  skate3_multiplayer_protocol_v12_codec_benchmarks.cpp
)
target_include_directories(
  skate3_multiplayer_protocol_v12_codec_benchmarks PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_link_libraries(
  skate3_multiplayer_protocol_v12_codec_benchmarks PRIVATE snappy)
target_compile_features(
  skate3_multiplayer_protocol_v12_codec_benchmarks PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_codec_benchmarks PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_codec_benchmarks PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_protocol_v12_codec_benchmarks
  COMMAND skate3_multiplayer_protocol_v12_codec_benchmarks
)

add_executable(skate3_multiplayer_protocol_v12_block_delta_tests
  skate3_multiplayer_protocol_v12_block_delta_tests.cpp
)
target_include_directories(
  skate3_multiplayer_protocol_v12_block_delta_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_protocol_v12_block_delta_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_protocol_v12_block_delta_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_protocol_v12_block_delta_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_protocol_v12_block_delta_tests
  COMMAND skate3_multiplayer_protocol_v12_block_delta_tests
)

add_executable(skate3_multiplayer_outbound_scheduler_tests
  skate3_multiplayer_outbound_scheduler_tests.cpp
)

target_include_directories(
  skate3_multiplayer_outbound_scheduler_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_outbound_scheduler_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_outbound_scheduler_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_outbound_scheduler_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_outbound_scheduler_tests
  COMMAND skate3_multiplayer_outbound_scheduler_tests
)

add_executable(skate3_multiplayer_lifecycle_tests
  skate3_multiplayer_lifecycle_tests.cpp
)

target_include_directories(skate3_multiplayer_lifecycle_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(skate3_multiplayer_lifecycle_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_lifecycle_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_lifecycle_tests
  COMMAND skate3_multiplayer_lifecycle_tests
)

add_executable(skate3_multiplayer_worker_tests
  skate3_multiplayer_worker_tests.cpp
)

target_include_directories(skate3_multiplayer_worker_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(skate3_multiplayer_worker_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_worker_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_worker_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_worker_tests
  COMMAND skate3_multiplayer_worker_tests
)

add_executable(skate3_multiplayer_render_cache_tests
  skate3_multiplayer_render_cache_tests.cpp
)

target_include_directories(
  skate3_multiplayer_render_cache_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_render_cache_tests PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(
    skate3_multiplayer_render_cache_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_render_cache_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

add_test(
  NAME skate3_multiplayer_render_cache_tests
  COMMAND skate3_multiplayer_render_cache_tests
)

add_executable(skate3_multiplayer_routing_tests
  skate3_multiplayer_routing_tests.cpp
)
target_include_directories(
  skate3_multiplayer_routing_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_routing_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_routing_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_routing_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_routing_tests
  COMMAND skate3_multiplayer_routing_tests
)

add_executable(skate3_multiplayer_scale_tests
  skate3_multiplayer_scale_tests.cpp
)
target_include_directories(
  skate3_multiplayer_scale_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src
)
target_compile_features(
  skate3_multiplayer_scale_tests PRIVATE cxx_std_23)
if(MSVC)
  target_compile_options(
    skate3_multiplayer_scale_tests PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(
    skate3_multiplayer_scale_tests PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
add_test(
  NAME skate3_multiplayer_scale_tests
  COMMAND skate3_multiplayer_scale_tests
)
