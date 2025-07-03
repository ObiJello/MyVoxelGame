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

# 4) ImGui (no demo window) - FIXED: Added GLM include directory
add_library(imgui
        ext/imgui/imgui.cpp
        ext/imgui/imgui_draw.cpp
        ext/imgui/imgui_tables.cpp
        ext/imgui/imgui_widgets.cpp
        ext/imgui/backends/imgui_impl_glfw.cpp
        ext/imgui/backends/imgui_impl_opengl3.cpp
        src/debug/DebugSystem.cpp
        src/debug/DebugSystem.hpp
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

# 6) The game executable (on macOS, it's a bundle)
if(APPLE)
    add_executable(MyVoxelGame MACOSX_BUNDLE
            src/main.cpp
            src/platform/PlatformMain.cpp
            src/platform/Time.cpp
            src/platform/Input.cpp
            src/core/Log.cpp
            src/core/JobSystem.cpp
            src/render/Shader.cpp
            src/render/ChunkRenderer.cpp
            src/render/TextureAtlas.cpp
            src/render/BlockHighlight.cpp
            src/game/BlockRegistry.cpp
            src/game/BlockModel.cpp          # ADDED: Block model JSON loader
            src/game/Mesher.cpp
            src/game/ChunkProvider.cpp
            src/game/WorldManager.cpp
            src/game/RayCast.cpp
            src/game/WorldAccess.cpp
            src/game/Inventory.cpp
            src/game/PlayerController.cpp
            src/game/Physics.cpp
            src/render/Crosshair.cpp
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
            src/render/Shader.cpp
            src/render/ChunkRenderer.cpp
            src/render/TextureAtlas.cpp
            src/game/BlockRegistry.cpp
            src/game/BlockModel.cpp          # ADDED: Block model JSON loader
            src/game/Mesher.cpp
            src/game/ChunkProvider.cpp
            src/game/WorldManager.cpp
            src/game/RayCast.cpp
            src/game/RayCast.hpp
            src/game/WorldAccess.cpp
            src/game/WorldAccess.hpp
            src/game/Inventory.cpp
            src/game/Inventory.hpp
            src/game/PlayerController.cpp
            src/game/PlayerController.hpp
            src/render/BlockHighlight.hpp
            src/render/BlockHighlight.cpp
            src/game/Physics.cpp
            src/game/Physics.hpp
            src/render/Crosshair.cpp
            src/render/Crosshair.hpp
            src/render/AtlasBuilder.cpp
            src/render/AtlasBuilder.hpp
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
        ${PROJECT_SOURCE_DIR}/ext/glfw/include
        ${PROJECT_SOURCE_DIR}/ext/glad/include
        ${PROJECT_SOURCE_DIR}/ext/glm
        ${PROJECT_SOURCE_DIR}/ext/stb_image
        ${PROJECT_SOURCE_DIR}/ext/zlib           # for zconf.h, zlib.h
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