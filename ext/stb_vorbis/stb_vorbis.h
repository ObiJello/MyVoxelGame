// stb_vorbis — Ogg Vorbis decoder by Sean Barrett (public domain / MIT, see
// stb_vorbis.c). Vendored single-file library; this header exposes its API
// declarations only. The implementation is compiled once, in
// stb_vorbis_impl.c (the `stb_vorbis` static library, ext/stb_vorbis/
// CMakeLists.txt).
#ifndef OBEY_STB_VORBIS_H
#define OBEY_STB_VORBIS_H
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#endif
