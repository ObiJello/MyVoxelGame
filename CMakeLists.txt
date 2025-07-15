cmake_minimum_required(VERSION 3.19)
project(MyVoxelGame LANGUAGES CXX VERSION 0.1.0)

# ── On macOS, build both Apple Silicon (arm64) and Intel (x86_64) slices,
#     and force a minimum deployment target ─────────────────────────────────
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.7.6" CACHE STRING "Minimum macOS version" FORCE)
endif()
# ────────────────────────────────────────────────────────────────────────────

# 1) Force C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optionally build 32-bit binaries on Windows. This is useful when targeting
# older 32-bit environments or running on a 32-bit compiler toolchain.
if(WIN32)
    option(BUILD_WIN32_32BIT "Build 32-bit Windows binaries" OFF)
    if(BUILD_WIN32_32BIT)
        add_compile_options(-m32)
        add_link_options(-m32)
    endif()
endif()

# 2) Binaries go into bin/
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# 3) Vendor subprojects
add_subdirectory(ext/glfw)
add_subdirectory(ext/glad)
add_subdirectory(ext/zlib)    # static-only zlibstatic
set(ALSOFT_CPUEXT_NEON  OFF CACHE BOOL "Disable ARM NEON" FORCE)
set(ALSOFT_REQUIRE_NEON OFF CACHE BOOL "Disable ARM NEON requirement" FORCE)
add_subdirectory(ext/OpenAL)  # static-only OpenAL
add_subdirectory(ext/stb_image)

# ── Add nlohmann/json library for block model JSON parsing ─────────────────
include(FetchContent)
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
# ────────────────────────────────────────────────────────────────────────────

# 4) ImGui
add_library(imgui
        ext/imgui/imgui.cpp
        ext/imgui/imgui_draw.cpp
        ext/imgui/imgui_tables.cpp
        ext/imgui/imgui_widgets.cpp
        ext/imgui/backends/imgui_impl_glfw.cpp
        ext/imgui/backends/imgui_impl_opengl3.cpp
        src/render/debug/DebugSystem.cpp
        src/render/debug/DebugSystem.hpp
)
target_include_directories(imgui PRIVATE
        ext/imgui
        ext/imgui/backends
        ${PROJECT_SOURCE_DIR}/ext/glm  # ADDED: GLM include for DebugSystem
        ${PROJECT_SOURCE_DIR}/src      # ADDED: Source root for Log.hpp and other headers
        ${PROJECT_SOURCE_DIR}/src/core # ADDED: Core directory for Log.hpp
        ${PROJECT_SOURCE_DIR}/src/render # ADDED: Render directory
        ${PROJECT_SOURCE_DIR}/src/game   # ADDED: Game directory
)

# Link JSON library so DebugSystem can include <nlohmann/json.hpp>
target_link_libraries(imgui PRIVATE nlohmann_json::nlohmann_json)

# 5) ImGui linking per-platform
if(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PRIVATE glfw glad OpenGL::GL)
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(OpenGL_FRAMEWORK OpenGL)
    target_link_libraries(imgui PRIVATE
            glfw
            glad
            ${COCOA_FRAMEWORK}
            ${IOKIT_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
            ${OpenGL_FRAMEWORK}
    )
else()
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PRIVATE glfw glad X11::X11 OpenGL::GL)
endif()

# ── Define world parsing sources for region file dumper AND main executable ──────────────
set(WORLD_SOURCES
        src/engine/world/RegionFile.cpp
        src/engine/world/RegionFileCache.cpp
        src/engine/world/NBTParser.cpp
        src/engine/world/RegionDumper.cpp
)
# ────────────────────────────────────────────────────────────────────────────

# 6) The game executable (on macOS, it's a bundle)
if(APPLE)
    add_executable(MyVoxelGame MACOSX_BUNDLE
            src/main.cpp
            src/platform/PlatformMain.cpp
            src/platform/Time.cpp
            src/platform/Input.cpp
            src/core/Log.cpp
            src/core/JobSystem.cpp
            src/render/gfx/Shader.cpp
            src/render/mesh/BlockHighlight.cpp
            src/render/debug/Crosshair.cpp
            src/render/atlas/AtlasBuilder.cpp
            src/engine/block/BlockModel.cpp
            src/engine/world/SectionDataUnpacker.cpp
            src/engine/world/MinecraftChunkLoader.cpp
            src/engine/Physics/RayCast.cpp
            src/engine/Physics/Physics.cpp
            src/engine/block/BlockRegistry.cpp
            ${WORLD_SOURCES}
            src/game/Inventory.cpp
            src/game/PlayerController.cpp
            src/engine/world/World.cpp
            src/engine/world/ChunkProvider.cpp
            src/engine/world/Chunk.cpp
            src/render/mesh/ChunkRenderer.cpp
            src/render/mesh/MeshManager.cpp
            src/render/mesh/ChunkMeshData.cpp
            src/render/mesh/FluidMeshBuilder.cpp
            src/render/mesh/Mesher.cpp
            src/render/mesh/FluidMeshBuilder.cpp
            src/render/mesh/ChunkMeshData.cpp
            src/platform/GameDirectory.cpp
    )
    set_target_properties(MyVoxelGame PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist"
    )
else()
    add_executable(MyVoxelGame
            src/main.cpp
            src/platform/PlatformMain.cpp
            src/platform/Time.cpp
            src/platform/Input.cpp
            src/core/Log.cpp
            src/core/JobSystem.cpp
            src/render/gfx/Shader.cpp
            src/engine/block/BlockModel.cpp
            ${WORLD_SOURCES}
            src/engine/Physics/RayCast.cpp
            src/engine/Physics/RayCast.hpp
            src/engine/Physics/Physics.cpp
            src/engine/Physics/Physics.hpp
            src/game/Inventory.cpp
            src/game/Inventory.hpp
            src/game/PlayerController.cpp
            src/game/PlayerController.hpp
            src/render/mesh/BlockHighlight.hpp
            src/render/mesh/BlockHighlight.cpp
            src/render/mesh/ChunkRenderer.cpp
            src/render/mesh/ChunkRenderer.hpp
            src/render/mesh/MeshManager.cpp
            src/render/mesh/MeshManager.hpp
            src/render/mesh/ChunkMeshData.cpp
            src/render/mesh/ChunkMeshData.hpp
            src/render/mesh/FluidMeshBuilder.cpp
            src/render/mesh/FluidMeshBuilder.hpp
            src/render/mesh/Mesher.cpp
            src/render/mesh/Mesher.hpp
            src/render/mesh/SectionMesh.hpp
            src/render/mesh/FluidMeshBuilder.cpp
            src/render/mesh/FluidMeshBuilder.hpp
            src/render/mesh/ChunkMeshData.cpp
            src/render/mesh/ChunkMeshData.hpp
            src/render/debug/Crosshair.cpp
            src/render/debug/Crosshair.hpp
            src/render/atlas/AtlasBuilder.cpp
            src/render/atlas/AtlasBuilder.hpp
            src/engine/block/BlockRegistry.cpp
            src/engine/block/BlockRegistry.hpp
            src/engine/world/SectionDataUnpacker.cpp
            src/engine/world/SectionDataUnpacker.hpp
            src/engine/world/MinecraftChunkLoader.cpp
            src/engine/world/MinecraftChunkLoader.hpp
            src/engine/world/IBlockAccess.hpp
            src/engine/world/World.cpp
            src/engine/world/World.hpp
            src/engine/world/ChunkProvider.cpp
            src/engine/world/ChunkProvider.hpp
            src/engine/world/Chunk.cpp
            src/engine/world/Chunk.hpp
            src/engine/world/WorldGlobals.hpp
            src/platform/GameDirectory.cpp
            src/platform/GameDirectory.hpp
            src/game/WorldCoordinates.hpp
            src/game/WorldCoordinates.cpp
    )
endif()

# 7) PCH
target_precompile_headers(MyVoxelGame PRIVATE src/pch.hpp)

# 8) Includes
target_include_directories(MyVoxelGame PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/platform
        ${PROJECT_SOURCE_DIR}/src/core
        ${PROJECT_SOURCE_DIR}/src/render
        ${PROJECT_SOURCE_DIR}/src/game
        ${PROJECT_SOURCE_DIR}/src/engine
        ${PROJECT_SOURCE_DIR}/src/world
        ${PROJECT_SOURCE_DIR}/ext/glfw/include
        ${PROJECT_SOURCE_DIR}/ext/glad/include
        ${PROJECT_SOURCE_DIR}/ext/glm
        ${PROJECT_SOURCE_DIR}/ext/stb_image
        ${PROJECT_SOURCE_DIR}/ext/zlib
        ${PROJECT_SOURCE_DIR}/ext/OpenAL/include
        ${PROJECT_SOURCE_DIR}/ext/imgui
        ${PROJECT_SOURCE_DIR}/ext/imgui/backends
)

# 9) Link libraries
target_link_libraries(MyVoxelGame PRIVATE
        glfw
        glad
        stb_image
        ZLIB::ZLIBSTATIC     # static zlib
        OpenAL::OpenAL       # static OpenAL
        imgui
        nlohmann_json::nlohmann_json  # ADDED: JSON library for block models
)

# ── NEW: Create standalone Region Dumper tool ─────────────────────────────
add_executable(RegionDumper
        src/tools/RegionDumperMain.cpp
        ${WORLD_SOURCES}
        src/core/Log.cpp  # Need logging
)

# Link required libraries to the region dumper
target_link_libraries(RegionDumper PRIVATE
        ZLIB::ZLIBSTATIC
)

# Set include directories for the region dumper
target_include_directories(RegionDumper PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/core
        ${PROJECT_SOURCE_DIR}/src/world
        ${PROJECT_SOURCE_DIR}/ext/zlib
)

# Compiler options for region dumper
target_compile_features(RegionDumper PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(RegionDumper PRIVATE /W4)
else()
    target_compile_options(RegionDumper PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── NEW: Create test executable for region dumper ─────────────────────────
add_executable(TestRegionDumper
        src/tests/test_region_dumper.cpp
        ${WORLD_SOURCES}
        src/core/Log.cpp
        # Need logging
)

# Link required libraries to the test
target_link_libraries(TestRegionDumper PRIVATE
        ZLIB::ZLIBSTATIC
)

# Set include directories for the test
target_include_directories(TestRegionDumper PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/core
        ${PROJECT_SOURCE_DIR}/src/world
        ${PROJECT_SOURCE_DIR}/ext/zlib
)

# Compiler options for test
target_compile_features(TestRegionDumper PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(TestRegionDumper PRIVATE /W4)
else()
    target_compile_options(TestRegionDumper PRIVATE -Wall -Wextra -Wpedantic)
endif()
# ────────────────────────────────────────────────────────────────────────────

# ── Copy shaders into .app/Contents/Resources on macOS ────────────────────────
if(APPLE)
    add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/shaders"
            "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Resources/shaders"
    )
    # Copy assets directory for texture atlas and block models
    add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Resources/assets"
    )
else()
    add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:MyVoxelGame>/shaders"
    )
    # Copy assets directory for texture atlas and block models
    add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:MyVoxelGame>/assets"
    )
endif()
# ────────────────────────────────────────────────────────────────────────────

# 10) Platform‐specific OpenGL linking for the game
if(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(MyVoxelGame PRIVATE OpenGL::GL)
elseif(APPLE)
    target_link_libraries(MyVoxelGame PRIVATE
            ${COCOA_FRAMEWORK}
            ${IOKIT_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
            ${OpenGL_FRAMEWORK}
    )
else()
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(MyVoxelGame PRIVATE X11::X11 OpenGL::GL)
endif()

# ── Optional: Install targets ──────────────────────────────────────────────
install(TARGETS MyVoxelGame DESTINATION bin)
install(TARGETS RegionDumper DESTINATION bin)
install(TARGETS TestRegionDumper DESTINATION bin)
# ────────────────────────────────────────────────────────────────────────────

# ── Optional: Print configuration summary ──────────────────────────────────────
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "MyVoxelGame v${PROJECT_VERSION} Configuration Summary")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "Build Type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:         C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:             ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(APPLE)
    message(STATUS "macOS Architectures:  ${CMAKE_OSX_ARCHITECTURES}")
    message(STATUS "macOS Deployment:     ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if(WIN32 AND BUILD_WIN32_32BIT)
    message(STATUS "Windows Target:       32-bit")
else()
    message(STATUS "Windows Target:       64-bit")
endif()
message(STATUS "")
message(STATUS "Executables:")
message(STATUS "  MyVoxelGame:        Main game executable")
message(STATUS "  RegionDumper:       Minecraft region file parser")
message(STATUS "  TestRegionDumper:   Region parser test suite")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  GLFW:               Bundled (ext/glfw)")
message(STATUS "  GLAD:               Bundled (ext/glad)")
message(STATUS "  GLM:                Bundled (ext/glm)")
message(STATUS "  STB Image:          Bundled (ext/stb_image)")
message(STATUS "  zlib:               Bundled (ext/zlib)")
message(STATUS "  OpenAL:             Bundled (ext/OpenAL)")
message(STATUS "  ImGui:              Bundled (ext/imgui)")
message(STATUS "  nlohmann/json:      FetchContent (v3.11.3)")
message(STATUS "")
message(STATUS "Output Directory:     ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "")