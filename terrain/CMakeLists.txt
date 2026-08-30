cmake_minimum_required(VERSION 3.16)
project(minecraft_terrain_tests)

# Standalone build for the terrain-parity test harness inside MyVoxelGame.
# The library SOURCES are the game's embedded copy (../src/my_terrain_library)
# so the game and the parity net always test the same code. Configure once:
#   cd terrain/build && cmake ..
# then the tests/parity scripts drive `make async_chunk_test` themselves.
#
# This replaces the old standalone project at
# ~/Desktop/26.1-snapshot-1/minecraft_terrain (no longer worked on).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# CRITICAL for Java bit parity (same flags as the game's terrain_library
# target): no FP contraction, Java signed-overflow wrap semantics.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-ffp-contract=off -fwrapv)
endif()
if(MSVC)
    add_compile_options(/fp:precise)
endif()

get_filename_component(GAME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(TERRAIN_LIB_DIR "${GAME_ROOT}/src/my_terrain_library")

find_package(ZLIB REQUIRED)

include_directories(
    ${TERRAIN_LIB_DIR}/include
    ${GAME_ROOT}/ext/md5     # <openssl/md5.h> shim -> CommonCrypto on macOS (same as the game build)
    ${ZLIB_INCLUDE_DIRS}
)

file(GLOB_RECURSE TERRAIN_LIB_SOURCES CONFIGURE_DEPENDS
    "${TERRAIN_LIB_DIR}/src/*.cpp"
)

add_library(minecraft_terrain STATIC ${TERRAIN_LIB_SOURCES})
target_link_libraries(minecraft_terrain ${ZLIB_LIBRARIES})

# The main parity binary (full async pipeline; driven by tests/parity scripts).
add_executable(async_chunk_test tests/parity/cpp/CppChunkGeneratorTest.cpp)
target_link_libraries(async_chunk_test minecraft_terrain)

# Biome region finder (drives golden region selection).
add_executable(biome_search tests/parity/cpp/BiomeSearchTool.cpp)
target_link_libraries(biome_search minecraft_terrain)

# Repro/diagnostic for World Properties tweaks edge cases.
add_executable(tweaks_repro tests/tweaks_repro.cpp)
target_link_libraries(tweaks_repro minecraft_terrain)

add_executable(template_sweep tests/template_sweep.cpp)
target_link_libraries(template_sweep minecraft_terrain)
