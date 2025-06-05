cmake_minimum_required(VERSION 3.19)
project(MyVoxelGame LANGUAGES CXX)

# 1) Define CMake presets (if you want to use them).
#    You can skip presets initially and just configure in CLion manually.
#    But here’s an example you can drop into cmake/CMakePresets.json later:
#    {
#      "version": 3,
#      "cmakeMinimumRequired": { "major": 3, "minor": 19 },
#      "configurePresets": [
#        {
#          "name": "mac-clang",
#          "generator": "Ninja",
#          "cacheVariables": {
#            "CMAKE_C_COMPILER": "clang",
#            "CMAKE_CXX_COMPILER": "clang++",
#            "CMAKE_BUILD_TYPE": "Debug"
#          }
#        },
#        {
#          "name": "win-msvc",
#          "generator": "Ninja",
#          "cacheVariables": {
#            "CMAKE_C_COMPILER": "cl.exe",
#            "CMAKE_CXX_COMPILER": "cl.exe",
#            "CMAKE_BUILD_TYPE": "Debug"
#          }
#        }
#      ]
#    }

# 2) Force C++17/C++20 (we’ll write code targeting C++17 for now).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 3) Set up an output folder for compiled binaries
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# 4) Make an 'ext' directory visible for finding vendored libs
add_subdirectory(ext/glfw)
add_subdirectory(ext/glad)
add_subdirectory(ext/zlib)
add_subdirectory(ext/OpenAL)
add_subdirectory(ext/stb_image)

# 5) Create your executable target; for now just src/main.cpp
add_executable(MyVoxelGame
    src/main.cpp
)

# 6) Link against vendored dependencies (we’ll vendor and build GLFW, GLAD, glm, stb_image, zlib, OpenAL Soft in ext/)
#    In ext/, you will add those projects (see Step 3).
target_link_libraries(MyVoxelGame PRIVATE
    glfw
    glad
    stb_image
    zlib
    OpenAL::OpenAL
)

# 7) Specify include directories (so that #include <GLFW/glfw3.h>, <glad/glad.h>, <glm/glm.hpp>, etc. work):
target_include_directories(MyVoxelGame PRIVATE
    ${PROJECT_SOURCE_DIR}/ext/glfw/include
    ${PROJECT_SOURCE_DIR}/ext/glad/include
    ${PROJECT_SOURCE_DIR}/ext/glm
    ${PROJECT_SOURCE_DIR}/ext/stb_image
    ${PROJECT_SOURCE_DIR}/ext/zlib
    ${PROJECT_SOURCE_DIR}/ext/OpenAL/include
)

# 8) Platform‐specific settings (optional; CLion usually auto-detects them)
if(WIN32)
    # On Windows, link to Windows-specific libs (e.g. opengl32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(MyVoxelGame PRIVATE OpenGL::GL)
elseif(APPLE)
    # On macOS, link to the OpenGL framework
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
    # On Linux, link to X11/GL if needed (CLion + GLFW often pulls this in automatically)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(MyVoxelGame PRIVATE X11::X11 OpenGL::GL)
endif()

