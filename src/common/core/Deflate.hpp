// File: src/common/core/Deflate.hpp
//
// Every deflate stream the game and the launcher read or write — region
// chunks, NBT files (level.dat, player data, entities), network frames, zip
// entries, PNGs — goes through libdeflate here. zlib is gone from the build.
//
// libdeflate works on whole buffers only: no streaming. Everything here is
// already a whole buffer (a chunk, a file, a frame, a zip entry), so that
// costs nothing but one thing — a zlib stream does not record its own
// decompressed size, so Decompress may have to grow its output and start
// again (see there). A gzip stream's trailer and a zip entry's header do say
// it, and are sized once.
//
// Output is the standard zlib / gzip / raw deflate format any reader takes
// (Minecraft included); the bytes differ from zlib's at the same level, as
// any two encoders' do.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Core::Deflate {

    enum class Format : uint8_t {
        Zlib,   // RFC 1950: region chunks (id 2), network frames
        Gzip,   // RFC 1952: .dat NBT files, region chunks (id 1)
        Raw,    // RFC 1951: zip entries
    };

    // Level 0..12 on libdeflate's scale; 6 matches zlib's default in speed
    // class and is what everything here used. A negative level means 6.
    bool Compress(const uint8_t* in, size_t n, std::vector<uint8_t>& out, Format format, int level = 6);

    // A stream whose decompressed size is known exactly (a network frame's
    // header, a zip entry's). False unless it decodes to exactly `outSize`.
    bool DecompressExact(const uint8_t* in, size_t n, uint8_t* out, size_t outSize, Format format);

    enum class Status : uint8_t { Ok, Corrupt, TooLarge };

    // A zlib or gzip stream of unknown size, the wrapper taken from the bytes
    // (gzip's 1f 8b magic, else zlib) rather than from what the caller was
    // told — a region file whose id byte disagrees with its data still reads.
    // Trailing bytes after the stream are ignored, as zlib's inflate did.
    // A gzip stream is sized from its trailer; a zlib stream starts at 4x the
    // input and doubles (restarting each time — libdeflate cannot resume) up
    // to `maxBytes`, past which it is TooLarge.
    Status Decompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t maxBytes);

    // CRC-32 as zip and gzip use it; `crc` continues a running value.
    uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

} // namespace Core::Deflate
