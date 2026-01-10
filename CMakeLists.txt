cmake_minimum_required(VERSION 3.19)
project(MyVoxelGame LANGUAGES CXX VERSION 0.1.0)

# ── On macOS, build both Apple Silicon (arm64) and Intel (x86_64) slices,
#     and force a minimum deployment target ─────────────────────────────────
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.7.6" CACHE STRING "Minimum macOS version" FORCE)
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
        "src/my_terrain_library_old/src/*.cpp"
)

add_library(terrain_library STATIC ${TERRAIN_LIBRARY_SOURCES})
target_include_directories(terrain_library PUBLIC
        src/my_terrain_library_old/include
        ${PROJECT_SOURCE_DIR}/ext/md5  # MD5 wrapper (uses CommonCrypto on macOS, OpenSSL elsewhere)
)
target_compile_features(terrain_library PUBLIC cxx_std_20)

# Link crypto library based on platform
if(APPLE)
    # macOS: Use built-in CommonCrypto (universal binary support)
    # No linking needed - CommonCrypto is part of libSystem
else()
    # Other platforms: Use OpenSSL
    find_package(OpenSSL REQUIRED)
    target_link_libraries(terrain_library PUBLIC OpenSSL::Crypto)
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
            src/common/world/chunk/Chunk.cpp
            src/server/world/tracking/DirtyTracker.cpp
            src/server/world/storage/AsyncChunkSaver.cpp
            src/server/world/storage/AnvilRegionWriter.cpp
            src/server/world/storage/AnvilChunkSaver.cpp
            src/server/world/storage/MinecraftChunkLoaderImpl.cpp
            src/common/world/gen/ProceduralChunkGenerator.cpp
            src/server/world/MyTerrainGenerator.cpp
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
    )
    set_target_properties(MyVoxelGame PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist"
    )
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
            src/server/world/ServerWorkerPool.cpp
            src/server/world/ServerWorkerPool.hpp
            src/server/world/ignore.cpp
            src/server/world/ignore.hpp

            # ========================================================================
            # CUSTOM TERRAIN GENERATION (MyTerrainGenerator wrapper)
            # ========================================================================
            src/server/world/MyTerrainGenerator.cpp
            src/server/world/MyTerrainGenerator.hpp

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
    )
endif()

# 7) PCH
target_precompile_headers(MyVoxelGame PRIVATE src/pch.hpp)

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
        ${Boost_INCLUDE_DIRS}
        # ========================================================================
        # CUSTOM TERRAIN LIBRARY
        # ========================================================================
        src/my_terrain_library_old/include
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
)

# Link with Asio if using standalone
if(TARGET asio)
    target_link_libraries(MyVoxelGame PRIVATE asio)
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

# ── Optional: Install targets ──────────────────────────────────────────────
install(TARGETS MyVoxelGame DESTINATION bin)
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
message(STATUS "  Boost.Asio:         System (header-only)")
message(STATUS "")
message(STATUS "Output Directory:     ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "")