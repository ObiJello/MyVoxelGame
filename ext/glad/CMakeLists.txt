# ext/glad/CMakeLists.txt
#
# Build GLAD as a static library.
# Assumes the source layout: 
#    ext/glad/include/glad/…
#    ext/glad/src/glad.c

cmake_minimum_required(VERSION 3.0)
project(glad LANGUAGES C)

# Create a static library “glad” from glad.c
add_library(glad STATIC
    src/glad.c
)

# Expose the include/ directory to anyone linking against glad
target_include_directories(glad
    PUBLIC 
      ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# It’s C code, but if you ever compile in a C++ project, make sure it’s linked correctly:
target_compile_definitions(glad PUBLIC GLAD_GL_IMPLEMENTATION)

