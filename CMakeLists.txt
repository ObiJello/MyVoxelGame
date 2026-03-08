cmake_minimum_required(VERSION 3.19)
project(MyVoxelGame LANGUAGES C CXX VERSION 0.1.0)

# ── Include debug symbols in Release builds (for Sentry crash symbolication) ──
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/Zi)
    else()
        add_compile_options(-g)
    endif()
endif()

# ── On macOS, build universal (arm64 + x86_64) libraries by default.
#     The game executable may be restricted to arm64-only when using Homebrew Vulkan,
#     but all shared libraries and the launcher are always universal. ───────────
if(APPLE)
    option(ENABLE_VULKAN "Enable Vulkan rendering backend" ON)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.7.6" CACHE STRING "Minimum macOS version" FORCE)

    # Always build libraries as universal so the launcher (and anything else
    # that doesn't depend on Vulkan) works on both Intel and Apple Silicon.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)

    # Track whether the game target needs to be restricted to arm64
    # (only when using Homebrew MoltenVK which is arm64-only)
    set(GAME_OSX_ARM64_ONLY FALSE)
    if(ENABLE_VULKAN AND (NOT DEFINED ENV{VULKAN_SDK} OR "$ENV{VULKAN_SDK}" STREQUAL ""))
        set(GAME_OSX_ARM64_ONLY TRUE)
        message(STATUS "Vulkan (Homebrew) - game will be arm64 only, launcher universal")
    else()
        message(STATUS "Building universal binaries (arm64 + x86_64)")
    endif()
    message(STATUS "macOS Library Architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()
# ────────────────────────────────────────────────────────────────────────────

# 1) Force C++20
set(CMAKE_CXX_STANDARD 20)
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
    
    # Define Windows version for ASIO and Windows APIs
    # Windows 7 (0x0601) as minimum for proper ASIO support
    add_compile_definitions(_WIN32_WINNT=0x0601)
    add_compile_definitions(WINVER=0x0601)
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

# ── Add Sentry Native SDK for crash reporting ────────────────────────────────
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SENTRY_BACKEND "crashpad" CACHE STRING "" FORCE)
FetchContent_Declare(
        sentry
        GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
        GIT_TAG 0.7.17
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sentry)
# ────────────────────────────────────────────────────────────────────────────

# ── Add Boost.Asio for networking (header-only) ────────────────────────────
# Try to find system Boost first
find_package(Boost 1.70 QUIET)

if(Boost_FOUND)
    message(STATUS "Found system Boost: ${Boost_VERSION}")
else()
    message(STATUS "System Boost not found, downloading standalone Asio...")
    
    # Download standalone Asio which includes necessary Boost components
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-28-0
        GIT_SHALLOW TRUE
    )
    
    FetchContent_GetProperties(asio)
    if(NOT asio_POPULATED)
        FetchContent_Populate(asio)
        # Add Asio include directory (it includes its own Boost subset)
        set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
        # Create an interface library for Asio
        add_library(asio INTERFACE)
        target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIR})
        # Define ASIO_STANDALONE to use Asio without Boost
        target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
        # Set Boost variables for compatibility
        set(Boost_FOUND TRUE)
        set(Boost_INCLUDE_DIRS ${ASIO_INCLUDE_DIR})
    endif()
endif()
# ────────────────────────────────────────────────────────────────────────────

# 4) ImGui
add_library(imgui
        ext/imgui/imgui.cpp
        ext/imgui/imgui_draw.cpp
        ext/imgui/imgui_tables.cpp
        ext/imgui/imgui_widgets.cpp
        ext/imgui/backends/imgui_impl_glfw.cpp
        ext/imgui/backends/imgui_impl_opengl3.cpp
        src/client/renderer/debug/DebugSystem.cpp
        src/client/renderer/debug/DebugSystem.hpp
        src/server/world/interfaces/IChunkLoader.hpp
        src/server/world/interfaces/IChunkSaver.hpp
        src/common/world/gen/IChunkGenerator.hpp
        src/server/world/interfaces/INeighborProvider.hpp
        src/server/world/cache/ChunkCache.cpp
        src/server/world/cache/ChunkCache.hpp
        src/common/world/gen/ProceduralChunkGenerator.cpp
        src/common/world/gen/ProceduralChunkGenerator.hpp
        src/server/world/storage/AsyncChunkSaver.cpp
        src/server/world/storage/AsyncChunkSaver.hpp
        src/server/world/storage/AnvilRegionWriter.cpp
        src/server/world/storage/AnvilRegionWriter.hpp
        src/server/world/storage/AnvilChunkSaver.cpp
        src/server/world/storage/AnvilChunkSaver.hpp
)
target_include_directories(imgui PRIVATE
        ext/imgui
        ext/imgui/backends
        ${PROJECT_SOURCE_DIR}/ext/glm
        ${PROJECT_SOURCE_DIR}/ext/zlib
        ${PROJECT_SOURCE_DIR}/src
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

# 5a) Terrain library - static library from your custom terrain generation system
# For MD5 hashing: Use CommonCrypto on macOS (built-in, universal), OpenSSL elsewhere
file(GLOB_RECURSE TERRAIN_LIBRARY_SOURCES
        "src/my_terrain_library/src/*.cpp"
)

add_library(terrain_library STATIC ${TERRAIN_LIBRARY_SOURCES})
target_include_directories(terrain_library PUBLIC
        src/my_terrain_library/include
        ${PROJECT_SOURCE_DIR}/ext/md5  # MD5 wrapper (uses CommonCrypto on macOS, OpenSSL elsewhere)
        ${PROJECT_SOURCE_DIR}/ext/zlib  # zlib headers for NBT compression
)
target_link_libraries(terrain_library PUBLIC zlibstatic)
target_compile_features(terrain_library PUBLIC cxx_std_20)

# MSVC doesn't define M_PI by default
if(MSVC)
    target_compile_definitions(terrain_library PUBLIC _USE_MATH_DEFINES)
endif()

# MD5 hashing: macOS uses CommonCrypto (built-in), other platforms use
# self-contained implementation in ext/md5/ - no OpenSSL needed

# 5b) Optional Vulkan backend
option(ENABLE_VULKAN "Enable Vulkan rendering backend" ON)
if(ENABLE_VULKAN)
    set(HAS_VULKAN FALSE)
    set(VULKAN_IS_SDK FALSE)

    # 1) Check for Vulkan SDK via VULKAN_SDK env var (preferred - provides universal binaries)
    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        set(VULKAN_SDK_PATH "$ENV{VULKAN_SDK}")
        find_library(VULKAN_LIBRARY NAMES vulkan PATHS "${VULKAN_SDK_PATH}/lib" NO_DEFAULT_PATH)
        find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h PATHS "${VULKAN_SDK_PATH}/include" NO_DEFAULT_PATH)
        if(VULKAN_LIBRARY AND VULKAN_INCLUDE_DIR)
            message(STATUS "Vulkan found via SDK: ${VULKAN_LIBRARY}")
            set(HAS_VULKAN TRUE)
            set(VULKAN_IS_SDK TRUE)
            # Prefer the dylib directly over xcframework
            set(MOLTENVK_LIBRARY "${VULKAN_SDK_PATH}/lib/libMoltenVK.dylib")
            if(NOT EXISTS "${MOLTENVK_LIBRARY}")
                find_library(MOLTENVK_LIBRARY NAMES MoltenVK PATHS "${VULKAN_SDK_PATH}/lib" NO_DEFAULT_PATH)
            endif()
        endif()
    endif()

    # 2) Fallback: try find_package (may find Homebrew or system Vulkan)
    if(NOT HAS_VULKAN)
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            set(VULKAN_LIBRARY "${Vulkan_LIBRARY}")
            set(VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}")
            message(STATUS "Vulkan found via system/Homebrew: ${VULKAN_LIBRARY}")
            set(HAS_VULKAN TRUE)
        endif()
    endif()

    # 3) Last resort: try Homebrew paths directly on macOS
    if(NOT HAS_VULKAN AND APPLE)
        find_library(VULKAN_LIBRARY NAMES vulkan PATHS /opt/homebrew/lib /usr/local/lib)
        find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h PATHS /opt/homebrew/include /usr/local/include)
        if(VULKAN_LIBRARY AND VULKAN_INCLUDE_DIR)
            message(STATUS "Vulkan found via Homebrew: ${VULKAN_LIBRARY}")
            set(HAS_VULKAN TRUE)
            find_library(MOLTENVK_LIBRARY NAMES MoltenVK PATHS /opt/homebrew/lib /usr/local/lib)
        endif()
    endif()

    if(NOT HAS_VULKAN)
        message(STATUS "Vulkan not found, Vulkan backend disabled")
    endif()
else()
    set(HAS_VULKAN FALSE)
endif()

    # (Architecture was already set at the top of CMakeLists.txt)

# SPIR-V shader compilation (if Vulkan enabled and glslc available)
if(HAS_VULKAN)
    find_program(GLSLC glslc PATHS $ENV{VULKAN_SDK}/bin /opt/homebrew/bin /usr/local/bin)
    if(GLSLC)
        message(STATUS "glslc found: ${GLSLC}")
        # Compile Vulkan shaders to SPIR-V
        set(VK_SHADER_DIR "${PROJECT_SOURCE_DIR}/shaders")
        set(VK_SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
        file(MAKE_DIRECTORY ${VK_SPV_DIR})

        set(VK_SHADERS
            ${VK_SHADER_DIR}/block_vk.vert
            ${VK_SHADER_DIR}/block_vk.frag
            ${VK_SHADER_DIR}/crosshair_vk.vert
            ${VK_SHADER_DIR}/crosshair_vk.frag
            ${VK_SHADER_DIR}/highlight_vk.vert
            ${VK_SHADER_DIR}/highlight_vk.frag
        )
        set(VK_SPV_FILES)
        foreach(SHADER ${VK_SHADERS})
            get_filename_component(SHADER_NAME ${SHADER} NAME)
            set(SPV_FILE ${VK_SPV_DIR}/${SHADER_NAME}.spv)
            add_custom_command(
                OUTPUT ${SPV_FILE}
                COMMAND ${GLSLC} ${SHADER} -o ${SPV_FILE}
                DEPENDS ${SHADER}
                COMMENT "Compiling SPIR-V: ${SHADER_NAME}"
            )
            list(APPEND VK_SPV_FILES ${SPV_FILE})
        endforeach()
        add_custom_target(compile_shaders ALL DEPENDS ${VK_SPV_FILES})
    else()
        message(STATUS "glslc not found, using pre-compiled SPIR-V shaders")
    endif()
endif()

# 6) The game executable (on macOS, it's a bundle)
if(APPLE)
    add_executable(MyVoxelGame MACOSX_BUNDLE
            src/main.cpp
            src/platform/PlatformMain.cpp
            src/platform/Time.cpp
            src/client/input/Input.cpp
            src/common/core/Log.cpp
            src/common/core/JobSystem.cpp
            src/common/core/ThreadAllocator.cpp
            src/client/renderer/shader/Shader.cpp
            src/common/world/block/BlockModel.cpp
            src/common/world/block/Block.cpp
            src/server/world/storage/RegionFile.cpp
            src/server/world/storage/RegionFileCache.cpp
            src/server/world/storage/NBTParser.cpp
            src/server/world/storage/RegionDumper.cpp
            src/common/physics/RayCast.cpp
            src/common/physics/Physics.cpp
            src/common/entity/Inventory.cpp
            src/client/input/PlayerController.cpp
            src/client/entity/Player.cpp
            src/client/renderer/mesh/BlockHighlight.cpp
            src/client/renderer/mesh/ChunkRenderer.cpp
            src/client/renderer/mesh/ChunkMeshData.cpp
            src/client/renderer/mesh/FluidMeshBuilder.cpp
            src/client/renderer/mesh/Mesher.cpp
            src/client/renderer/mesh/GPUDataPool.cpp
            src/client/renderer/debug/Crosshair.cpp
            src/client/renderer/texture/AtlasBuilder.cpp
            src/client/renderer/texture/TextureAnimator.cpp
            src/common/world/block/BlockRegistry.cpp
            src/server/player/ServerPlayer.cpp
            src/server/world/storage/SectionDataUnpacker.cpp
            src/server/world/storage/MinecraftChunkLoader.cpp
            src/common/world/level/World.cpp
            src/server/world/ChunkProvider.cpp
            src/server/world/MyTerrainGenerator.cpp
            src/common/world/chunk/Chunk.cpp
            src/server/world/tracking/DirtyTracker.cpp
            src/server/world/storage/AsyncChunkSaver.cpp
            src/server/world/storage/AnvilRegionWriter.cpp
            src/server/world/storage/AnvilChunkSaver.cpp
            src/server/world/storage/MinecraftChunkLoaderImpl.cpp
            src/common/world/gen/ProceduralChunkGenerator.cpp
            src/server/world/cache/ChunkCache.cpp
            src/platform/GameDirectory.cpp
            src/common/world/math/WorldCoordinates.cpp
            # Minecraft Java Edition Architecture files
            src/server/IntegratedServer.cpp
            src/client/world/ClientChunkManager.cpp
            src/client/world/PendingDiffsManager.cpp
            src/server/world/ServerWorkerPool.cpp
            src/client/world/ClientWorkerPool.cpp
            src/server/world/ignore.cpp
            src/client/renderer/mesh/ClientMeshManager.cpp
            # Network layer
            src/common/network/NetworkConnection.cpp
            src/server/network/NetworkServer.cpp
            src/server/network/ServerConnection.cpp
            src/server/network/listeners/HandshakePacketListener.cpp
            src/server/network/listeners/LoginPacketListener.cpp
            src/server/network/listeners/ServerPlayPacketListener.cpp
            
            # New session management system
            src/server/world/ticketing/ChunkTicketManager.cpp
            src/server/world/watch/ChunkWatchIndex.cpp
            src/server/world/status/ChunkStatusManager.cpp
            src/server/world/tracking/SectionChangeAccumulator.cpp
            src/server/world/tracking/ChunkDeltaBroadcaster.cpp
            src/server/network/SendScheduler.cpp
            src/server/session/PlayerSession.cpp
            src/server/session/PlayerSessionManager.cpp
            
            src/common/network/packets/HandshakeC2S.cpp
            src/common/network/packets/LoginStartC2S.cpp
            src/common/network/packets/KeepAliveC2S.cpp
            src/client/network/NetworkClient.cpp
            src/client/network/ClientConnection.cpp
            src/client/network/NetworkIOService.cpp
            src/client/network/ClientPacketHandler.cpp

            # Render backend abstraction
            src/client/renderer/backend/opengl/GLBackend.cpp
    )
    # Add Vulkan backend source if available
    if(HAS_VULKAN)
        target_sources(MyVoxelGame PRIVATE
            src/client/renderer/backend/vulkan/VKBackend.cpp
        )
    endif()
    set_target_properties(MyVoxelGame PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist"
    )
    # Restrict game to arm64-only when using Homebrew Vulkan (arm64-only MoltenVK).
    # Libraries are still universal so the launcher can link against them.
    if(GAME_OSX_ARM64_ONLY)
        set_target_properties(MyVoxelGame PROPERTIES OSX_ARCHITECTURES "arm64")
    endif()
else()
    add_executable(MyVoxelGame
            src/main.cpp
            src/pch.hpp
            src/platform/PlatformMain.cpp
            src/platform/PlatformMain.hpp
            src/platform/Time.cpp
            src/platform/Time.hpp
            src/platform/GameDirectory.cpp
            src/platform/GameDirectory.hpp

            # Core systems
            src/common/core/Log.cpp
            src/common/core/Log.hpp
            src/common/core/JobSystem.cpp
            src/common/core/JobSystem.hpp
            src/common/core/ThreadAllocator.cpp
            src/common/core/ThreadAllocator.hpp
            src/common/core/Config.hpp

            # Client input
            src/client/input/Input.cpp
            src/client/input/Input.hpp
            src/client/input/PlayerController.cpp
            src/client/input/PlayerController.hpp
            
            # Client entity
            src/client/entity/Player.cpp
            src/client/entity/Player.hpp

            # Client rendering - Shader
            src/client/renderer/shader/Shader.cpp
            src/client/renderer/shader/Shader.hpp

            # Client rendering - Mesh
            src/client/renderer/mesh/BlockHighlight.cpp
            src/client/renderer/mesh/BlockHighlight.hpp
            src/client/renderer/mesh/ChunkMeshData.cpp
            src/client/renderer/mesh/ChunkMeshData.hpp
            src/client/renderer/mesh/ChunkRenderer.cpp
            src/client/renderer/mesh/ChunkRenderer.hpp
            src/client/renderer/mesh/ClientMeshManager.cpp
            src/client/renderer/mesh/ClientMeshManager.hpp
            src/client/renderer/mesh/GPUDataPool.cpp
            src/client/renderer/mesh/GPUDataPool.hpp
            src/client/renderer/mesh/FluidMeshBuilder.cpp
            src/client/renderer/mesh/FluidMeshBuilder.hpp
            src/client/renderer/mesh/MeshManager.cpp
            src/client/renderer/mesh/MeshManager.hpp
            src/client/renderer/mesh/Mesher.cpp
            src/client/renderer/mesh/Mesher.hpp
            src/client/renderer/mesh/SectionMesh.hpp
            src/client/renderer/mesh/MeshJobData.hpp
            src/client/renderer/mesh/SnapshotBuilder.hpp

            # Client rendering - Debug
            src/client/renderer/debug/Crosshair.cpp
            src/client/renderer/debug/Crosshair.hpp
            src/client/renderer/debug/DebugSystem.cpp
            src/client/renderer/debug/DebugSystem.hpp

            # Client rendering - Core
            src/client/renderer/core/Camera.hpp
            src/client/renderer/core/Frustum.hpp
            src/client/renderer/core/Vertex.hpp

            # Client rendering - Texture
            src/client/renderer/texture/AtlasBuilder.cpp
            src/client/renderer/texture/AtlasBuilder.hpp
            src/client/renderer/texture/TextureAnimator.cpp
            src/client/renderer/texture/TextureAnimator.hpp

            # Client world
            src/client/world/ClientChunkManager.cpp
            src/client/world/ClientChunkManager.hpp
            src/client/world/ClientWorkerPool.cpp
            src/client/world/ClientWorkerPool.hpp
            src/client/world/PendingDiffsManager.cpp
            src/client/world/PendingDiffsManager.hpp

            # Client networking
            src/client/network/ClientConnection.cpp
            src/client/network/ClientConnection.hpp
            src/client/network/NetworkClient.cpp
            src/client/network/NetworkClient.hpp
            src/client/network/NetworkIOService.cpp
            src/client/network/NetworkIOService.hpp
            src/client/network/ClientPacketHandler.cpp
            src/client/network/ClientPacketHandler.hpp

            # Common world - Block
            src/common/world/block/BlockModel.cpp
            src/common/world/block/BlockModel.hpp
            src/common/world/block/Block.cpp
            src/common/world/block/Block.hpp
            src/common/world/block/BlockInteraction.hpp
            src/common/world/block/BlockRegistry.cpp
            src/common/world/block/BlockRegistry.hpp
            src/common/world/block/Blocks.hpp

            # Common world - Chunk
            src/common/world/chunk/Chunk.cpp
            src/common/world/chunk/Chunk.hpp
            src/common/world/chunk/ChunkSection.hpp
            src/common/world/chunk/IBlockAccess.hpp

            # Common world - Level
            src/common/world/level/World.cpp
            src/common/world/level/World.hpp
            src/common/world/level/WorldGlobals.hpp

            # Common world - Gen
            src/common/world/gen/ProceduralChunkGenerator.cpp
            src/common/world/gen/ProceduralChunkGenerator.hpp
            src/common/world/gen/IChunkGenerator.hpp

            # Common world - Math
            src/common/world/math/WorldCoordinates.cpp
            src/common/world/math/WorldCoordinates.hpp
            src/common/world/math/WorldMath.hpp

            # Common physics
            src/common/physics/Physics.cpp
            src/common/physics/Physics.hpp
            src/common/physics/RayCast.cpp
            src/common/physics/RayCast.hpp

            # Common entity
            src/common/entity/Inventory.cpp
            src/common/entity/Inventory.hpp

            # Common network
            src/common/network/MessageQueue.hpp
            src/common/network/PacketTypes.hpp
            src/common/network/PacketRegistry.hpp
            src/common/network/NetworkConnection.cpp
            src/common/network/NetworkConnection.hpp
            src/common/network/IPacket.hpp
            src/common/network/packets/S2CPackets.hpp

            # Server
            src/server/IntegratedServer.cpp
            src/server/IntegratedServer.hpp
            src/server/world/ChunkProvider.cpp
            src/server/world/ChunkProvider.hpp
            src/server/world/MyTerrainGenerator.cpp
            src/server/world/MyTerrainGenerator.hpp
            src/server/world/ServerWorkerPool.cpp
            src/server/world/ServerWorkerPool.hpp
            src/server/world/ignore.cpp
            src/server/world/ignore.hpp

            # ========================================================================
            # CUSTOM TERRAIN GENERATION (MyTerrainGenerator wrapper)
            # ========================================================================

            src/server/player/ServerPlayer.cpp
            src/server/player/ServerPlayer.hpp

            # Server world - Cache
            src/server/world/cache/ChunkCache.cpp
            src/server/world/cache/ChunkCache.hpp

            # Server world - Interfaces
            src/server/world/interfaces/IChunkLoader.hpp
            src/server/world/interfaces/IChunkSaver.hpp
            src/server/world/interfaces/INeighborProvider.hpp

            # Server world - Storage
            src/server/world/storage/AnvilChunkSaver.cpp
            src/server/world/storage/AnvilChunkSaver.hpp
            src/server/world/storage/AnvilRegionWriter.cpp
            src/server/world/storage/AnvilRegionWriter.hpp
            src/server/world/storage/AsyncChunkSaver.cpp
            src/server/world/storage/AsyncChunkSaver.hpp
            src/server/world/storage/MinecraftChunkLoader.cpp
            src/server/world/storage/MinecraftChunkLoader.hpp
            src/server/world/storage/MinecraftChunkLoaderImpl.cpp
            src/server/world/storage/MinecraftChunkLoaderImpl.hpp
            src/server/world/storage/NBTParser.cpp
            src/server/world/storage/NBTParser.hpp
            src/server/world/storage/RegionDumper.cpp
            src/server/world/storage/RegionDumper.hpp
            src/server/world/storage/RegionFile.cpp
            src/server/world/storage/RegionFile.hpp
            src/server/world/storage/RegionFileCache.cpp
            src/server/world/storage/RegionFileCache.hpp
            src/server/world/storage/SectionDataUnpacker.cpp
            src/server/world/storage/SectionDataUnpacker.hpp

            # Server world - Tracking
            src/server/world/tracking/DirtyTracker.cpp
            src/server/world/tracking/DirtyTracker.hpp

            # Server networking
            src/server/network/NetworkServer.cpp
            src/server/network/NetworkServer.hpp
            src/server/network/ServerConnection.cpp
            src/server/network/ServerConnection.hpp
            src/server/network/listeners/HandshakePacketListener.cpp
            src/server/network/listeners/HandshakePacketListener.hpp
            src/server/network/listeners/LoginPacketListener.cpp
            src/server/network/listeners/LoginPacketListener.hpp
            src/server/network/listeners/ServerPlayPacketListener.cpp
            src/server/network/listeners/ServerPlayPacketListener.hpp
            
            # New session management system
            src/server/world/ticketing/ChunkTicketManager.cpp
            src/server/world/ticketing/ChunkTicketManager.hpp
            src/server/world/watch/ChunkWatchIndex.cpp
            src/server/world/watch/ChunkWatchIndex.hpp
            src/server/world/status/ChunkStatusManager.cpp
            src/server/world/status/ChunkStatusManager.hpp
            src/server/world/tracking/SectionChangeAccumulator.cpp
            src/server/world/tracking/SectionChangeAccumulator.hpp
            src/server/world/tracking/ChunkDeltaBroadcaster.cpp
            src/server/world/tracking/ChunkDeltaBroadcaster.hpp
            src/server/network/SendScheduler.cpp
            src/server/network/SendScheduler.hpp
            src/server/session/PlayerSession.cpp
            src/server/session/PlayerSession.hpp
            src/server/session/PlayerSessionManager.cpp
            src/server/session/PlayerSessionManager.hpp
            
            # Common network packets
            src/common/network/packets/HandshakeC2S.cpp
            src/common/network/packets/HandshakeC2S.hpp
            src/common/network/packets/LoginStartC2S.cpp
            src/common/network/packets/LoginStartC2S.hpp
            src/common/network/packets/KeepAliveC2S.cpp
            src/common/network/packets/KeepAliveC2S.hpp

            # Render backend abstraction
            src/client/renderer/backend/opengl/GLBackend.cpp
            src/client/renderer/backend/opengl/GLBackend.hpp
    )
    # Add Vulkan backend source if available (non-Apple builds)
    if(HAS_VULKAN)
        target_sources(MyVoxelGame PRIVATE
            src/client/renderer/backend/vulkan/VKBackend.cpp
        )
    endif()
endif()

# 7) PCH
target_precompile_headers(MyVoxelGame PRIVATE src/pch.hpp)
# Ensure SPIR-V shaders are compiled before MyVoxelGame links (so post-build copy gets fresh .spv)
if(TARGET compile_shaders)
    add_dependencies(MyVoxelGame compile_shaders)
endif()

# 8) Includes
target_include_directories(MyVoxelGame PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/ext/glfw/include
        ${PROJECT_SOURCE_DIR}/ext/glad/include
        ${PROJECT_SOURCE_DIR}/ext/glm
        ${PROJECT_SOURCE_DIR}/ext/stb_image
        ${PROJECT_SOURCE_DIR}/ext/zlib
        ${PROJECT_SOURCE_DIR}/ext/OpenAL/include
        ${PROJECT_SOURCE_DIR}/ext/imgui
        ${PROJECT_SOURCE_DIR}/ext/imgui/backends
        ${CMAKE_BINARY_DIR}/generated
        ${Boost_INCLUDE_DIRS}
        # ========================================================================
        # CUSTOM TERRAIN LIBRARY
        # ========================================================================
        src/my_terrain_library/include
)

# Add Asio or Boost includes
if(TARGET asio)
    target_include_directories(MyVoxelGame PRIVATE ${ASIO_INCLUDE_DIR})
endif()

# 9) Link libraries
target_link_libraries(MyVoxelGame PRIVATE
        glfw
        glad
        stb_image
        zlibstatic     # static zlib
        OpenAL::OpenAL       # static OpenAL
        imgui
        nlohmann_json::nlohmann_json  # ADDED: JSON library for block models
        terrain_library  # Custom terrain generation library
        sentry::sentry  # Crash reporting
)

# Link with Asio if using standalone
if(TARGET asio)
    target_link_libraries(MyVoxelGame PRIVATE asio)
endif()

# Link Vulkan if available
if(HAS_VULKAN)
    target_compile_definitions(MyVoxelGame PRIVATE HAS_VULKAN=1)
    target_compile_definitions(imgui PRIVATE HAS_VULKAN=1)
    target_link_libraries(MyVoxelGame PRIVATE ${VULKAN_LIBRARY})
    target_include_directories(MyVoxelGame PRIVATE ${VULKAN_INCLUDE_DIR})
    # Add ImGui Vulkan backend to imgui target
    target_sources(imgui PRIVATE
        ext/imgui/backends/imgui_impl_vulkan.cpp
    )
    target_link_libraries(imgui PRIVATE ${VULKAN_LIBRARY})
    target_include_directories(imgui PRIVATE ${VULKAN_INCLUDE_DIR})

    if(APPLE)
        # ── Bundle Vulkan libraries into .app so users don't need to install anything ──

        # Set RPATH so the executable finds dylibs in Contents/Frameworks/
        set_target_properties(MyVoxelGame PROPERTIES
            INSTALL_RPATH "@executable_path/../Frameworks"
            BUILD_RPATH "@executable_path/../Frameworks"
        )

        # Find MoltenVK library and ICD manifest
        if(NOT MOLTENVK_LIBRARY)
            find_library(MOLTENVK_LIBRARY NAMES MoltenVK
                PATHS $ENV{VULKAN_SDK}/lib /opt/homebrew/lib /usr/local/lib)
        endif()
        find_file(MOLTENVK_ICD_JSON NAMES MoltenVK_icd.json
            PATHS $ENV{VULKAN_SDK}/share/vulkan/icd.d
                  /opt/homebrew/etc/vulkan/icd.d
                  /usr/local/etc/vulkan/icd.d
                  /opt/homebrew/share/vulkan/icd.d)

        # Copy Vulkan loader into Frameworks/
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VULKAN_LIBRARY}"
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Frameworks/libvulkan.1.dylib"
            COMMENT "Bundling Vulkan loader into app"
        )
        # Copy MoltenVK into Frameworks/
        if(MOLTENVK_LIBRARY)
            add_custom_command(TARGET MyVoxelGame POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MOLTENVK_LIBRARY}"
                    "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Frameworks/libMoltenVK.dylib"
                COMMENT "Bundling MoltenVK into app"
            )
            message(STATUS "MoltenVK will be bundled: ${MOLTENVK_LIBRARY}")
        else()
            message(WARNING "MoltenVK not found - Vulkan will require system MoltenVK at runtime")
        endif()

        # Fix install names and re-sign all bundled dylibs
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND install_name_tool -id "@rpath/libvulkan.1.dylib"
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Frameworks/libvulkan.1.dylib"
            COMMAND codesign --force --sign -
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Frameworks/libvulkan.1.dylib"
            COMMAND codesign --force --sign -
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Frameworks/libMoltenVK.dylib"
            COMMENT "Fixing install names and re-signing bundled Vulkan dylibs"
        )

        # Fix the executable to link against the bundled loader
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_SOURCE_DIR}/tools/fix_vulkan_rpath.sh
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/MacOS/MyVoxelGame"
            COMMENT "Fixing executable Vulkan loader reference"
        )

        # Create bundled ICD manifest that points to the Frameworks/ copy
        file(WRITE "${CMAKE_BINARY_DIR}/MoltenVK_icd.json"
            "{\n"
            "    \"file_format_version\": \"1.0.0\",\n"
            "    \"ICD\": {\n"
            "        \"library_path\": \"../../../Frameworks/libMoltenVK.dylib\",\n"
            "        \"api_version\": \"1.2.0\",\n"
            "        \"is_portability_driver\": true\n"
            "    }\n"
            "}\n"
        )
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Resources/vulkan/icd.d"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/MoltenVK_icd.json"
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
            COMMENT "Bundling MoltenVK ICD manifest"
        )

        # Copy SPIR-V shaders to bundle
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/shaders/block_vk.vert.spv"
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Resources/shaders/block_vk.vert.spv"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/shaders/block_vk.frag.spv"
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Resources/shaders/block_vk.frag.spv"
        )

        # Don't hardcode GLFW's Vulkan path - let it find the bundled one via RPATH
        message(STATUS "Vulkan libraries will be bundled in .app/Contents/Frameworks/")
    endif()

    message(STATUS "Vulkan backend: ENABLED")
else()
    message(STATUS "Vulkan backend: DISABLED")
endif()

# ── Generate dSYM and strip binary for Sentry symbolication (Release only) ────
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET MyVoxelGame POST_BUILD
        COMMAND dsymutil "$<TARGET_FILE:MyVoxelGame>"
            -o "$<TARGET_BUNDLE_DIR:MyVoxelGame>.dSYM"
        COMMENT "Generating dSYM for Sentry symbolication"
    )
    add_custom_command(TARGET MyVoxelGame POST_BUILD
        COMMAND strip -x "$<TARGET_FILE:MyVoxelGame>"
        COMMENT "Stripping debug symbols from release binary"
    )
    # Upload dSYM to Sentry only when distributing (configure with -DJALIN=ON)
    option(JALIN "Upload dSYM to Sentry for distribution builds" OFF)
    if(JALIN)
        find_program(SENTRY_CLI sentry-cli)
        if(SENTRY_CLI)
            add_custom_command(TARGET MyVoxelGame POST_BUILD
                COMMAND ${SENTRY_CLI} debug-files upload "$<TARGET_BUNDLE_DIR:MyVoxelGame>.dSYM"
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                COMMENT "Uploading dSYM to Sentry"
            )
        else()
            message(WARNING "sentry-cli not found — dSYM will not be uploaded automatically")
        endif()
    endif()
elseif(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # Windows: Upload PDB debug symbols to Sentry on Release builds
    find_program(SENTRY_CLI sentry-cli)
    if(SENTRY_CLI)
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND ${SENTRY_CLI} debug-files upload "$<TARGET_PDB_FILE:MyVoxelGame>"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Uploading PDB to Sentry"
        )
    else()
        message(STATUS "sentry-cli not found — PDB will not be uploaded to Sentry")
    endif()
endif()

# ── Bundle crashpad handler into .app on macOS ────────────────────────────────
if(APPLE AND TARGET crashpad_handler)
    add_custom_command(TARGET MyVoxelGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>"
            "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/MacOS/crashpad_handler"
        COMMENT "Bundling crashpad_handler into app"
    )
endif()

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

# ── Auto version bump + release for game (Universal/JALIN builds only) ──
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release" AND JALIN)
        # Auto-increment game build number before compiling (edits Config.hpp directly)
        add_custom_target(bump_game_version ALL
            COMMAND bash "${PROJECT_SOURCE_DIR}/tools/bump_version.sh"
                game
                "${PROJECT_SOURCE_DIR}/tools/game_build_number"
                "0" "1"
                "${PROJECT_SOURCE_DIR}"
            COMMENT "Auto-incrementing game version"
        )
        add_dependencies(MyVoxelGame bump_game_version)

        # POST_BUILD: zip + upload
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND bash "${PROJECT_SOURCE_DIR}/tools/auto_release.sh"
                game
                "${PROJECT_SOURCE_DIR}/tools/game_build_number"
                "0" "1"
                "MyVoxelGame.app"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                "ObiJello/MyVoxelGame-Download"
            COMMENT "Auto-releasing game"
        )
        # Update Info.plist version in bundle
        add_custom_command(TARGET MyVoxelGame POST_BUILD
            COMMAND bash "${PROJECT_SOURCE_DIR}/tools/update_plist_version.sh"
                "${PROJECT_SOURCE_DIR}/tools/game_build_number"
                "$<TARGET_BUNDLE_DIR:MyVoxelGame>/Contents/Info.plist"
                "0" "1"
            COMMENT "Updating bundle version in Info.plist"
        )
endif()

# ── Optional: Install targets ──────────────────────────────────────────────
install(TARGETS MyVoxelGame DESTINATION bin)
# ────────────────────────────────────────────────────────────────────────────

# ═══════════════════════════════════════════════════════════════════════════════
# ── ObeyCraft Launcher (auto-updater) ─────────────────────────────────────────
# ═══════════════════════════════════════════════════════════════════════════════

# ImGui library for launcher (no game-specific code like DebugSystem)
add_library(imgui_launcher STATIC
    ext/imgui/imgui.cpp
    ext/imgui/imgui_draw.cpp
    ext/imgui/imgui_tables.cpp
    ext/imgui/imgui_widgets.cpp
    ext/imgui/backends/imgui_impl_glfw.cpp
    ext/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_launcher PUBLIC
    ext/imgui
    ext/imgui/backends
)
target_compile_features(imgui_launcher PUBLIC cxx_std_20)

# Platform linking for imgui_launcher
if(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui_launcher PRIVATE glfw glad OpenGL::GL)
elseif(APPLE)
    target_link_libraries(imgui_launcher PRIVATE
        glfw glad
        ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK} ${OpenGL_FRAMEWORK}
    )
else()
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui_launcher PRIVATE glfw glad X11::X11 OpenGL::GL)
endif()

# Find or build libcurl for the launcher
# - macOS: system curl (always available)
# - Windows: try system first, then build from source with Schannel (native TLS)
# - Linux: try system first
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    if(APPLE)
        # macOS always has curl as a system library
        find_library(CURL_LIBRARY curl)
        if(CURL_LIBRARY)
            set(CURL_FOUND TRUE)
            add_library(CURL::libcurl UNKNOWN IMPORTED)
            set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION "${CURL_LIBRARY}")
            message(STATUS "Found macOS system curl")
        endif()
    elseif(WIN32)
        # Build curl from source with Schannel (Windows native TLS) - no OpenSSL needed
        message(STATUS "System curl not found - building from source with Schannel...")
        set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)  # Windows native TLS
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_ZLIB OFF CACHE BOOL "" FORCE)          # We don't need zlib in curl
        set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
        set(HTTP_ONLY ON CACHE BOOL "" FORCE)             # We only need HTTP/HTTPS
        FetchContent_Declare(
            curl
            GIT_REPOSITORY https://github.com/curl/curl.git
            GIT_TAG curl-8_7_1
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(curl)
        set(CURL_FOUND TRUE)
        # Create the CURL::libcurl alias that our target expects
        if(NOT TARGET CURL::libcurl)
            add_library(CURL::libcurl ALIAS libcurl_static)
        endif()
        message(STATUS "Built curl from source (static, Schannel TLS)")
    endif()
endif()

if(CURL_FOUND)
    message(STATUS "Found curl - building ObeyCraft Launcher")

    # Minizip sources for zip extraction
    set(MINIZIP_SOURCES
        ext/zlib/contrib/minizip/unzip.c
        ext/zlib/contrib/minizip/ioapi.c
    )
    if(WIN32)
        list(APPEND MINIZIP_SOURCES ext/zlib/contrib/minizip/iowin32.c)
    endif()

    # Launcher sources
    set(LAUNCHER_SOURCES
        src/launcher/main.cpp
        src/launcher/LauncherApp.cpp
        src/launcher/ui/LauncherUI.cpp
        src/launcher/ui/LauncherTheme.cpp
        src/launcher/updater/VersionInfo.cpp
        src/launcher/updater/GitHubAPI.cpp
        src/launcher/updater/Downloader.cpp
        src/launcher/updater/Installer.cpp
        src/launcher/platform/ProcessLauncher.cpp
        # Shared code compiled into launcher
        src/common/core/Log.cpp
        src/platform/GameDirectory.cpp
        ${MINIZIP_SOURCES}
    )

    if(APPLE)
        add_executable(ObeyCraftLauncher MACOSX_BUNDLE ${LAUNCHER_SOURCES})
        set_target_properties(ObeyCraftLauncher PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/LauncherInfo.plist"
        )
    else()
        add_executable(ObeyCraftLauncher ${LAUNCHER_SOURCES})
    endif()

    target_include_directories(ObeyCraftLauncher PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/ext/glfw/include
        ${PROJECT_SOURCE_DIR}/ext/glad/include
        ${PROJECT_SOURCE_DIR}/ext/glm
        ${PROJECT_SOURCE_DIR}/ext/stb_image
        ${PROJECT_SOURCE_DIR}/ext/zlib
        ${PROJECT_SOURCE_DIR}/ext/zlib/contrib/minizip
        ${PROJECT_SOURCE_DIR}/ext/imgui
        ${PROJECT_SOURCE_DIR}/ext/imgui/backends
    )

    target_compile_features(ObeyCraftLauncher PRIVATE cxx_std_20)

    # Auto-increment launcher version (Release builds only, NOT universal/JALIN)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT JALIN)
        # Auto-increment launcher version before compiling (edits LauncherConfig.hpp directly)
        add_custom_target(bump_launcher_version ALL
            COMMAND bash "${PROJECT_SOURCE_DIR}/tools/bump_version.sh"
                launcher
                "${PROJECT_SOURCE_DIR}/tools/launcher_build_number"
                "1" "0"
                "${PROJECT_SOURCE_DIR}"
            COMMENT "Auto-incrementing launcher version"
        )
        add_dependencies(ObeyCraftLauncher bump_launcher_version)
    endif()

    target_link_libraries(ObeyCraftLauncher PRIVATE
        glfw glad stb_image zlibstatic
        imgui_launcher
        nlohmann_json::nlohmann_json
        CURL::libcurl
    )

    if(APPLE)
        find_library(CF_FRAMEWORK CoreFoundation)
        target_link_libraries(ObeyCraftLauncher PRIVATE
            ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK} ${OpenGL_FRAMEWORK}
            ${CF_FRAMEWORK}
        )
        # Copy font into bundle Resources
        add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:ObeyCraftLauncher>/Contents/Resources/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/ext/imgui/misc/fonts/Roboto-Medium.ttf"
                "$<TARGET_BUNDLE_DIR:ObeyCraftLauncher>/Contents/Resources/fonts/Roboto-Medium.ttf"
            COMMENT "Copying launcher fonts"
        )
        # Copy launcher assets if they exist
        if(EXISTS "${PROJECT_SOURCE_DIR}/assets/launcher")
            add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${PROJECT_SOURCE_DIR}/assets/launcher"
                    "$<TARGET_BUNDLE_DIR:ObeyCraftLauncher>/Contents/Resources/launcher"
                COMMENT "Copying launcher assets"
            )
        endif()
        # Copy app icon into bundle Resources
        if(EXISTS "${PROJECT_SOURCE_DIR}/assets/launcher/AppIcon.icns")
            add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PROJECT_SOURCE_DIR}/assets/launcher/AppIcon.icns"
                    "$<TARGET_BUNDLE_DIR:ObeyCraftLauncher>/Contents/Resources/AppIcon.icns"
                COMMENT "Copying launcher app icon"
            )
        endif()
        # Ad-hoc sign the launcher so macOS doesn't flag it as damaged
        add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
            COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:ObeyCraftLauncher>"
            COMMENT "Ad-hoc signing ObeyCraftLauncher.app"
        )
        # Auto version bump + release for launcher (Release only, NOT universal/JALIN)
        if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT JALIN)
            add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
                COMMAND bash "${PROJECT_SOURCE_DIR}/tools/auto_release.sh"
                    launcher
                    "${PROJECT_SOURCE_DIR}/tools/launcher_build_number"
                    "1" "0"
                    "ObeyCraftLauncher.app"
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                    "ObiJello/MyVoxelGame-Download"
                COMMENT "Auto-releasing launcher"
            )
        endif()
    elseif(WIN32)
        # Windows system libraries needed for curl/networking
        target_link_libraries(ObeyCraftLauncher PRIVATE ws2_32 crypt32)
        find_package(OpenGL REQUIRED)
        target_link_libraries(ObeyCraftLauncher PRIVATE OpenGL::GL)
        # Copy font next to executable on Windows
        add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:ObeyCraftLauncher>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/ext/imgui/misc/fonts/Roboto-Medium.ttf"
                "$<TARGET_FILE_DIR:ObeyCraftLauncher>/fonts/Roboto-Medium.ttf"
            COMMENT "Copying launcher fonts"
        )
        # Copy launcher assets on Windows if they exist
        if(EXISTS "${PROJECT_SOURCE_DIR}/assets/launcher")
            add_custom_command(TARGET ObeyCraftLauncher POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${PROJECT_SOURCE_DIR}/assets/launcher"
                    "$<TARGET_FILE_DIR:ObeyCraftLauncher>/launcher"
                COMMENT "Copying launcher assets"
            )
        endif()
    endif()

    install(TARGETS ObeyCraftLauncher DESTINATION bin)
else()
    message(STATUS "curl not found - ObeyCraft Launcher will NOT be built (install curl or build on macOS/Windows)")
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
message(STATUS "Executables:")
message(STATUS "  MyVoxelGame:        Main game executable")
if(CURL_FOUND)
message(STATUS "  ObeyCraftLauncher:  Auto-update launcher")
endif()
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
message(STATUS "  Boost.Asio:         System (header-only)")
message(STATUS "")
message(STATUS "Output Directory:     ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "")