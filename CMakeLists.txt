cmake_minimum_required(VERSION 3.19)
project(MyVoxelGame LANGUAGES CXX)

# 1) Force C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2) Ensure binaries go into bin/
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# 3) Vendor directories
add_subdirectory(ext/glfw)
add_subdirectory(ext/glad)
add_subdirectory(ext/zlib)
add_subdirectory(ext/OpenAL)
add_subdirectory(ext/stb_image)

# 4) Add the executable from src/main.cpp and related sources
add_executable(MyVoxelGame
        src/main.cpp
        src/platform/PlatformMain.cpp
        src/platform/Time.cpp
        src/platform/Input.cpp
        src/core/Log.cpp
        src/core/JobSystem.cpp
        src/render/Shader.cpp
        src/render/QuadRenderer.cpp
        src/game/BlockRegistry.cpp
)

# 5) Precompile header
target_precompile_headers(MyVoxelGame PRIVATE
        src/pch.hpp
)

# 6) Include directories (platform, core, render, game, plus ext/)
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
        ${PROJECT_SOURCE_DIR}/ext/zlib
        ${PROJECT_SOURCE_DIR}/ext/OpenAL/include
)

# 7) Link against vendored libraries
target_link_libraries(MyVoxelGame PRIVATE
        glfw
        glad
        stb_image
        zlib
        OpenAL::OpenAL
)

# ── Copy shaders directory to the build output ─────────────────────────────
add_custom_command(TARGET MyVoxelGame
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:MyVoxelGame>/shaders
)
# ────────────────────────────────────────────────────────────────────────────

# 8) Platform-specific OpenGL linking
if(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(MyVoxelGame PRIVATE OpenGL::GL)
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(OpenGL_FRAMEWORK OpenGL)
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