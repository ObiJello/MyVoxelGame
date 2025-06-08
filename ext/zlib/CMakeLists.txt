cmake_minimum_required(VERSION 3.12...3.31)

project(
        zlib
        LANGUAGES C
        VERSION 1.4.1.1
        HOMEPAGE_URL "https://zlib.net/"
        DESCRIPTION "a general-purpose lossless data-compression library")

# ============================================================================
# CPack metadata
# ============================================================================
set(CPACK_PACKAGE_VENDOR "zlib-Project")
set(CPACK_PACKAGE_DESCRIPTION_FILE ${zlib_SOURCE_DIR}/README)
set(CPACK_RESOURCE_FILE_LICENSE ${zlib_SOURCE_DIR}/LICENSE)
set(CPACK_RESOURCE_FILE_README ${zlib_SOURCE_DIR}/README)

# ============================================================================
# configuration options
# ============================================================================
# Force static‐only build: disable shared, enable static
option(ZLIB_BUILD_SHARED   "Enable building zlib shared library" OFF)
option(ZLIB_BUILD_STATIC   "Enable building zlib static library" ON)

# Disable examples/tests so they don’t try to link nonexistent targets
option(ZLIB_BUILD_TESTING   "Enable Zlib Examples as tests" OFF)
option(ZLIB_BUILD_MINIZIP   "Enable building libminizip contrib library" OFF)
option(ZLIB_INSTALL         "Enable installation of zlib" ON)
option(ZLIB_PREFIX          "prefix for all types and library functions, see zconf.h.in" OFF)
mark_as_advanced(ZLIB_PREFIX)

if(WIN32)
    option(ZLIB_INSTALL_COMPAT_DLL "Install a copy as zlib1.dll" ON)
endif()

get_property(IS_MULTI GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT IS_MULTI)
    message(STATUS "No CMAKE_BUILD_TYPE set -- using Release")
    set(CMAKE_BUILD_TYPE Release)
endif()

include(CheckCSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CMakePackageConfigHelpers)
include(CheckTypeSize)
include(CPack)
include(GNUInstallDirs)

set(CPACK_INCLUDED TRUE)

# ============================================================================
# Generate zconf.h from zconf.h.template
# ============================================================================
if(NOT ZLIB_CONF_WRITTEN)
    set(Z_PREFIX ${ZLIB_PREFIX})
    set(CONF_OUT_FILE ${zlib_BINARY_DIR}/zconf.h.cmakein)
    file(READ ${zlib_SOURCE_DIR}/zconf.h ZCONF_CONTENT LIMIT 245)
    file(WRITE ${CONF_OUT_FILE} ${ZCONF_CONTENT})
    file(APPEND ${CONF_OUT_FILE} "#cmakedefine Z_PREFIX 1\n")
    file(APPEND ${CONF_OUT_FILE} "#cmakedefine HAVE_STDARG_H 1\n")
    file(APPEND ${CONF_OUT_FILE} "#cmakedefine HAVE_UNISTD_H 1\n")
    file(READ ${zlib_SOURCE_DIR}/zconf.h ZCONF_CONTENT OFFSET 244)
    set(FIRST_ITEM TRUE)

    foreach(item IN LISTS ZCONF_CONTENT)
        if(FIRST_ITEM)
            string(APPEND OUT_CONTENT ${item})
            set(FIRST_ITEM FALSE)
        else()
            string(APPEND OUT_CONTENT "\;" ${item})
        endif()
    endforeach()

    file(APPEND ${CONF_OUT_FILE} ${OUT_CONTENT})
    set(ZLIB_CONF_WRITTEN
            TRUE
            CACHE BOOL "zconf.h.cmakein was created")
    mark_as_advanced(ZLIB_CONF_WRITTEN)
endif()

# ============================================================================
# Feature checks: large file support, fseeko, headers, hidden‐visibility
# ============================================================================
set(CMAKE_REQUIRED_DEFINITIONS -D_LARGEFILE64_SOURCE=1)
check_type_size(off64_t OFF64_T)
unset(CMAKE_REQUIRED_DEFINITIONS)

check_function_exists(fseeko HAVE_FSEEKO)
check_include_file(stdarg.h HAVE_STDARG_H)
check_include_file(unistd.h HAVE_UNISTD_H)

if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "-WX")
else()
    set(CMAKE_REQUIRED_FLAGS "-WError")
endif()

check_c_source_compiles(
        "
        #include <stdlib.h>
        static void f(void) __attribute__ ((visibility(\"hidden\")));
        int main(void) {return 0;}
    "
        HAVE___ATTR__VIS_HIDDEN)

unset(CMAKE_COMPILE_FLAGS)
set(ZLIB_PC ${zlib_BINARY_DIR}/zlib.pc)
configure_file(${zlib_SOURCE_DIR}/zlib.pc.cmakein ${ZLIB_PC} @ONLY)
configure_file(${zlib_BINARY_DIR}/zconf.h.cmakein ${zlib_BINARY_DIR}/zconf.h)

# ============================================================================
# zlib source files & public/private headers
# ============================================================================
set(ZLIB_PUBLIC_HDRS
        ${zlib_BINARY_DIR}/zconf.h
        ${zlib_SOURCE_DIR}/zlib.h
)

set(ZLIB_PRIVATE_HDRS
        ${zlib_SOURCE_DIR}/crc32.h
        ${zlib_SOURCE_DIR}/deflate.h
        ${zlib_SOURCE_DIR}/gzguts.h
        ${zlib_SOURCE_DIR}/inffast.h
        ${zlib_SOURCE_DIR}/inffixed.h
        ${zlib_SOURCE_DIR}/inflate.h
        ${zlib_SOURCE_DIR}/infback.h
        ${zlib_SOURCE_DIR}/inftrees.h
        ${zlib_SOURCE_DIR}/trees.h
        ${zlib_SOURCE_DIR}/zutil.h
)

set(ZLIB_SRCS
        ${zlib_SOURCE_DIR}/adler32.c
        ${zlib_SOURCE_DIR}/compress.c
        ${zlib_SOURCE_DIR}/crc32.c
        ${zlib_SOURCE_DIR}/deflate.c
        ${zlib_SOURCE_DIR}/gzclose.c
        ${zlib_SOURCE_DIR}/gzlib.c
        ${zlib_SOURCE_DIR}/gzread.c
        ${zlib_SOURCE_DIR}/gzwrite.c
        ${zlib_SOURCE_DIR}/inflate.c
        ${zlib_SOURCE_DIR}/infback.c
        ${zlib_SOURCE_DIR}/inftrees.c
        ${zlib_SOURCE_DIR}/inffast.c
        ${zlib_SOURCE_DIR}/trees.c
        ${zlib_SOURCE_DIR}/uncompr.c
        ${zlib_SOURCE_DIR}/zutil.c
)

if(WIN32)
    set(zlib_static_suffix "s")
    set(CMAKE_DEBUG_POSTFIX "d")
endif()

# ============================================================================
# Build a static library (zlibstatic) only
# ============================================================================
if(ZLIB_BUILD_STATIC)
    add_library(zlibstatic STATIC
            ${ZLIB_SRCS}
    )

    add_library(ZLIB::ZLIBSTATIC ALIAS zlibstatic)

    target_include_directories(
            zlibstatic
            PUBLIC
            $<BUILD_INTERFACE:${zlib_BINARY_DIR}>
            $<BUILD_INTERFACE:${zlib_SOURCE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_compile_definitions(
            zlibstatic
            PRIVATE
            ZLIB_BUILD
            $<$<BOOL:NOT:${HAVE_FSEEKO}>:NO_FSEEKO>
            $<$<BOOL:${HAVE___ATTR__VIS_HIDDEN}>:HAVE_HIDDEN>
            $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_DEPRECATE>
            $<$<BOOL:${MSVC}>:_CRT_NONSTDC_NO_DEPRECATE>
            PUBLIC $<$<BOOL:${OFF64_T}>:_LARGEFILE64_SOURCE=1>
    )

    set_target_properties(
            zlibstatic
            PROPERTIES
            EXPORT_NAME ZLIBSTATIC
            OUTPUT_NAME z${zlib_static_suffix}
    )
endif()

# (We deliberately do not call add_library(zlib SHARED …) because ZLIB_BUILD_SHARED=OFF.)

# ============================================================================
# Installation rules
# ============================================================================
if(ZLIB_INSTALL)
    if(ZLIB_BUILD_STATIC)
        install(
                TARGETS zlibstatic
                COMPONENT Development
                EXPORT zlibStaticExport
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        )
        install(
                EXPORT zlibStaticExport
                FILE ZLIB-static.cmake
                NAMESPACE ZLIB::
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib
        )

        install(
                FILES ${ZLIB_PUBLIC_HDRS}
                COMPONENT Development
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        )
    endif()

    configure_package_config_file(
            ${zlib_SOURCE_DIR}/zlibConfig.cmake.in
            ${zlib_BINARY_DIR}/ZLIBConfig.cmake
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib
    )
    write_basic_package_version_file(
            "${zlib_BINARY_DIR}/ZLIBConfigVersion.cmake"
            VERSION "${zlib_VERSION}"
            COMPATIBILITY AnyNewerVersion
    )
    install(
            FILES ${zlib_BINARY_DIR}/ZLIBConfig.cmake
            ${zlib_BINARY_DIR}/ZLIBConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib
    )
endif()

# ============================================================================
# Skip tests & minizip since we disabled ZLIB_BUILD_TESTING
# ============================================================================
# (Nothing to do here because ZLIB_BUILD_TESTING=OFF and ZLIB_BUILD_MINIZIP=OFF)