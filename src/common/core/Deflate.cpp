// File: src/common/core/Deflate.cpp
#include "common/core/Deflate.hpp"

#include <libdeflate.h>

#include <algorithm>
#include <memory>

namespace Core::Deflate {

    namespace {

        struct CompressorDeleter {
            void operator()(libdeflate_compressor* c) const { libdeflate_free_compressor(c); }
        };
        struct DecompressorDeleter {
            void operator()(libdeflate_decompressor* d) const { libdeflate_free_decompressor(d); }
        };

        // A compressor is fixed to its level at allocation: one per level per
        // thread, made on first use (a few hundred KB each at the high levels).
        libdeflate_compressor* CompressorFor(int level) {
            thread_local std::unique_ptr<libdeflate_compressor, CompressorDeleter> compressors[13];
            auto& c = compressors[level];
            if (!c) c.reset(libdeflate_alloc_compressor(level));
            return c.get();
        }

        libdeflate_decompressor* ThreadDecompressor() {
            thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d(
                libdeflate_alloc_decompressor());
            return d.get();
        }

        libdeflate_result DecompressInto(libdeflate_decompressor* d, bool gzip, const uint8_t* in, size_t n,
                                         uint8_t* out, size_t cap, size_t* produced) {
            size_t consumed = 0;
            return gzip ? libdeflate_gzip_decompress_ex(d, in, n, out, cap, &consumed, produced)
                        : libdeflate_zlib_decompress_ex(d, in, n, out, cap, &consumed, produced);
        }

    } // namespace

    bool Compress(const uint8_t* in, size_t n, std::vector<uint8_t>& out, Format format, int level) {
        if (level < 0) level = 6;
        if (level > 12) level = 12;
        libdeflate_compressor* c = CompressorFor(level);
        if (!c) return false;
        std::vector<uint8_t> result;
        switch (format) {
            case Format::Zlib: result.resize(libdeflate_zlib_compress_bound(c, n)); break;
            case Format::Gzip: result.resize(libdeflate_gzip_compress_bound(c, n)); break;
            case Format::Raw:  result.resize(libdeflate_deflate_compress_bound(c, n)); break;
        }
        size_t written = 0;
        switch (format) {
            case Format::Zlib: written = libdeflate_zlib_compress(c, in, n, result.data(), result.size()); break;
            case Format::Gzip: written = libdeflate_gzip_compress(c, in, n, result.data(), result.size()); break;
            case Format::Raw:  written = libdeflate_deflate_compress(c, in, n, result.data(), result.size()); break;
        }
        // 0 means the bound was too small, which the bound functions rule out.
        if (written == 0) return false;
        result.resize(written);
        out = std::move(result);
        return true;
    }

    bool DecompressExact(const uint8_t* in, size_t n, uint8_t* out, size_t outSize, Format format) {
        libdeflate_decompressor* d = ThreadDecompressor();
        if (!d) return false;
        size_t produced = 0;
        libdeflate_result rc = LIBDEFLATE_BAD_DATA;
        switch (format) {
            case Format::Zlib: rc = libdeflate_zlib_decompress(d, in, n, out, outSize, &produced); break;
            case Format::Gzip: rc = libdeflate_gzip_decompress(d, in, n, out, outSize, &produced); break;
            case Format::Raw:  rc = libdeflate_deflate_decompress(d, in, n, out, outSize, &produced); break;
        }
        return rc == LIBDEFLATE_SUCCESS && produced == outSize;
    }

    Status Decompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t maxBytes) {
        libdeflate_decompressor* d = ThreadDecompressor();
        if (!d || !in || n < 2 || maxBytes == 0) return Status::Corrupt;
        const bool gzip = in[0] == 0x1f && in[1] == 0x8b;

        // gzip ends with ISIZE, the decompressed size mod 2^32: straight into
        // `out` when it is plausible. A stream with trailing bytes or several
        // members reads a wrong ISIZE and falls through to the growing path.
        if (gzip && n >= 18) {
            const size_t isize = static_cast<size_t>(in[n - 4]) | (static_cast<size_t>(in[n - 3]) << 8) |
                                 (static_cast<size_t>(in[n - 2]) << 16) | (static_cast<size_t>(in[n - 1]) << 24);
            if (isize > 0 && isize <= maxBytes) {
                out.resize(isize);
                size_t produced = 0;
                if (DecompressInto(d, true, in, n, out.data(), out.size(), &produced) == LIBDEFLATE_SUCCESS) {
                    out.resize(produced);
                    return Status::Ok;
                }
            }
        }

        // Unknown size: a per-thread scratch buffer that keeps the largest
        // size a stream needed, so a worker reading chunk after chunk sizes
        // it once; each miss restarts the stream at twice the room.
        thread_local std::vector<uint8_t> scratch;
        const size_t start = std::min(maxBytes, n * 4 + 8192);
        if (scratch.size() < start) scratch.resize(start);
        Status status = Status::Corrupt;
        for (;;) {
            const size_t cap = std::min(scratch.size(), maxBytes);
            size_t produced = 0;
            const libdeflate_result rc = DecompressInto(d, gzip, in, n, scratch.data(), cap, &produced);
            if (rc == LIBDEFLATE_SUCCESS) {
                out.assign(scratch.data(), scratch.data() + produced);
                status = Status::Ok;
                break;
            }
            if (rc != LIBDEFLATE_INSUFFICIENT_SPACE) break;               // bad data
            if (cap >= maxBytes) { status = Status::TooLarge; break; }
            scratch.resize(std::min(cap * 2, maxBytes));
        }
        // Not a one-off giant kept alive for the thread's lifetime.
        if (scratch.size() > (32u << 20)) {
            scratch.clear();
            scratch.shrink_to_fit();
        }
        return status;
    }

    uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc) {
        return libdeflate_crc32(crc, data, n);
    }

} // namespace Core::Deflate
