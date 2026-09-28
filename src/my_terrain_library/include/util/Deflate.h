#pragma once
// zlib / gzip streams for the terrain library, over libdeflate (MIT) — the
// same library the game uses (common/core/Deflate); the library keeps its own
// copy of these few lines so its standalone parity build needs nothing of the
// game's. Whole buffers only: libdeflate does not stream.

#include <libdeflate.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace minecraft {
namespace util {
namespace deflate {

    enum class Format { Zlib, Gzip };

    namespace detail {
        struct CompressorDeleter {
            void operator()(libdeflate_compressor* c) const { libdeflate_free_compressor(c); }
        };
        struct DecompressorDeleter {
            void operator()(libdeflate_decompressor* d) const { libdeflate_free_decompressor(d); }
        };
        // Level 6: zlib's default, which every caller here used.
        inline libdeflate_compressor* Compressor() {
            thread_local std::unique_ptr<libdeflate_compressor, CompressorDeleter> c(libdeflate_alloc_compressor(6));
            return c.get();
        }
        inline libdeflate_decompressor* Decompressor() {
            thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d(
                libdeflate_alloc_decompressor());
            return d.get();
        }
    } // namespace detail

    // False only if the compressor could not be allocated.
    inline bool compress(const void* in, size_t n, std::vector<char>& out, Format format) {
        libdeflate_compressor* c = detail::Compressor();
        if (!c) return false;
        out.resize(format == Format::Gzip ? libdeflate_gzip_compress_bound(c, n)
                                          : libdeflate_zlib_compress_bound(c, n));
        const size_t written = format == Format::Gzip
            ? libdeflate_gzip_compress(c, in, n, out.data(), out.size())
            : libdeflate_zlib_compress(c, in, n, out.data(), out.size());
        out.resize(written);
        return written != 0;
    }

    // A zlib or gzip stream (the wrapper from its magic bytes, trailing bytes
    // ignored); its size unknown, so the output starts at 4x the input and
    // doubles, restarting each time, up to maxBytes. False on bad data or
    // past the cap.
    inline bool decompress(const void* in, size_t n, std::vector<char>& out,
                           size_t maxBytes = size_t(1) << 30) {
        libdeflate_decompressor* d = detail::Decompressor();
        const auto* bytes = static_cast<const uint8_t*>(in);
        if (!d || n < 2) return false;
        const bool gzip = bytes[0] == 0x1f && bytes[1] == 0x8b;
        out.resize(std::min(maxBytes, n * 4 + 8192));
        for (;;) {
            size_t consumed = 0, produced = 0;
            const libdeflate_result rc = gzip
                ? libdeflate_gzip_decompress_ex(d, in, n, out.data(), out.size(), &consumed, &produced)
                : libdeflate_zlib_decompress_ex(d, in, n, out.data(), out.size(), &consumed, &produced);
            if (rc == LIBDEFLATE_SUCCESS) {
                out.resize(produced);
                return true;
            }
            if (rc != LIBDEFLATE_INSUFFICIENT_SPACE || out.size() >= maxBytes) {
                out.clear();
                return false;
            }
            out.resize(std::min(out.size() * 2, maxBytes));
        }
    }

} // namespace deflate
} // namespace util
} // namespace minecraft
