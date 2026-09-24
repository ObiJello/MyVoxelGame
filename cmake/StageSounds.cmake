# Stage the extracted Minecraft sounds into the built game.
#
#   cmake -DSRC=<repo>/assets -DDST=<build>/assets -P cmake/StageSounds.cmake
#
# Runs POST_BUILD on every build of MyVoxelGame, so it must cost nothing when
# nothing changed: the sounds are ~370 MB / ~5,000 files, and even
# `copy_directory_if_different` reads every one of them to compare (measured
# ~1.6 s a build). Instead tools/extract_mc_sounds.py leaves a one-line stamp,
# assets/sounds/.extracted (the index and a digest of every file it copied),
# and the tree is re-copied only when the staged stamp differs from it.
#
# No extracted sounds (a fresh checkout, a CI box): nothing is staged and the
# game runs silent, which it announces in its log.

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "StageSounds.cmake: pass -DSRC=<assets> -DDST=<staged assets>")
endif()

set(_src_stamp "${SRC}/sounds/.extracted")
# Beside the tree, not in it: the copy below brings the source stamp along
# early, and the staged stamp must only exist once everything is in place.
set(_dst_stamp "${DST}/.sounds_staged")

if(NOT EXISTS "${_src_stamp}" OR NOT EXISTS "${SRC}/sounds.json")
    return()
endif()

file(READ "${_src_stamp}" _want)
set(_have "")
if(EXISTS "${_dst_stamp}")
    file(READ "${_dst_stamp}" _have)
endif()
if(_want STREQUAL _have)
    return()
endif()

message(STATUS "Staging Minecraft sounds into ${DST}")
# Replace, not merge: a new Minecraft version drops and renames files.
file(REMOVE "${_dst_stamp}")
file(REMOVE_RECURSE "${DST}/sounds")
file(MAKE_DIRECTORY "${DST}")
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SRC}/sounds" "${DST}/sounds"
        RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "StageSounds.cmake: copying ${SRC}/sounds failed (${_rc})")
endif()
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy "${SRC}/sounds.json" "${DST}/sounds.json"
        RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "StageSounds.cmake: copying ${SRC}/sounds.json failed (${_rc})")
endif()
# The stamp goes last: an interrupted copy leaves no stamp, so the next build
# copies again.
file(WRITE "${_dst_stamp}" "${_want}")
