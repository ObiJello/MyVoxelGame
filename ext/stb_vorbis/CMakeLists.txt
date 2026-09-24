# stb_vorbis — Ogg Vorbis decoding for the sound engine (client/sound).
# Public domain / MIT (see the licence block at the end of stb_vorbis.c),
# fetched from https://github.com/nothings/stb (v1.22).
add_library(stb_vorbis STATIC stb_vorbis_impl.c)
target_include_directories(stb_vorbis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Third-party code: not ours to keep warning-clean.
if(MSVC)
    target_compile_options(stb_vorbis PRIVATE /w)
    # stb_vorbis uses the CRT's fopen etc. for its file entry points.
    target_compile_definitions(stb_vorbis PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(stb_vorbis PRIVATE -w)
endif()
set_target_properties(stb_vorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)
