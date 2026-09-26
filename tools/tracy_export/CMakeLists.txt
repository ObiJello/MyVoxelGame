# tracy-export, built against a Tracy source checkout the same way Tracy's own
# csvexport is. Not part of the game build — tools/build_tracy_tools.sh
# configures it with -DTRACY_SRC=<checkout of the pinned tag>.
cmake_minimum_required(VERSION 3.16)

if(NOT TRACY_SRC)
    message(FATAL_ERROR "Pass -DTRACY_SRC=<tracy checkout>; tools/build_tracy_tools.sh does this for you")
endif()

set(NO_STATISTICS OFF)
include(${TRACY_SRC}/cmake/version.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(tracy-export LANGUAGES C CXX VERSION ${TRACY_VERSION_STRING})

include(${TRACY_SRC}/cmake/config.cmake)
include(${TRACY_SRC}/cmake/vendor.cmake)
include(${TRACY_SRC}/cmake/server.cmake)

add_executable(tracy-export TracyExport.cpp)
target_link_libraries(tracy-export PRIVATE TracyServer)
