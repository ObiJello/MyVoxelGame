// File: src/server/world/storage/anvil/AnvilRegion.cpp
#include "server/world/storage/anvil/AnvilRegion.hpp"

#include "common/core/Log.hpp"

#include <algorithm>
#include <string_view>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <zlib.h>

#if defined(_WIN32)
  #include <io.h>
  #define OBEY_FSYNC(fd)      _commit(fd)
  #define OBEY_FILENO(f)      _fileno(f)
  #define OBEY_FSEEK(f, o, w) _fseeki64((f), (o), (w))
  #define OBEY_FTELL(f)       _ftelli64(f)
#else
  #include <unistd.h>
  #define OBEY_FSYNC(fd)      ::fsync(fd)
  #define OBEY_FILENO(f)      ::fileno(f)
  #define OBEY_FSEEK(f, o, w) ::fseeko((f), static_cast<off_t>(o), (w))
  #define OBEY_FTELL(f)       static_cast<int64_t>(::ftello(f))
#endif

namespace Game::Anvil {

    namespace {
        constexpr size_t kBitsPerWord = 64;

        uint32_t ReadBE32(const uint8_t* p) {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                 | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
        }
        void WriteBE32(uint8_t* p, uint32_t v) {
            p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
            p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
        }

        size_t SectorsFor(size_t bytes) { return (bytes + AnvilRegion::kSectorBytes - 1) / AnvilRegion::kSectorBytes; }

        std::string Errno(std::string_view what) {
            return std::string(what) + ": " + std::strerror(errno);
        }

        // std::filesystem::path::c_str() is wchar_t* on Windows, which fopen
        // cannot take. One helper keeps every call site identical.
        std::FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
            const std::wstring wmode(mode, mode + std::strlen(mode));
            std::FILE* f = nullptr;
            if (_wfopen_s(&f, p.c_str(), wmode.c_str()) != 0) return nullptr;
            return f;
#else
            return std::fopen(p.c_str(), mode);
#endif
        }
    } // namespace

    // ── SectorBitmap ────────────────────────────────────────────────────────

    void SectorBitmap::EnsureBits(size_t bits) {
        const size_t words = (bits + kBitsPerWord - 1) / kBitsPerWord;
        if (words > m_words.size()) m_words.resize(words, 0);
    }

    void SectorBitmap::Force(size_t from, size_t count) {
        if (count == 0) return;
        EnsureBits(from + count);
        for (size_t i = from; i < from + count; ++i) {
            m_words[i / kBitsPerWord] |= (uint64_t{1} << (i % kBitsPerWord));
        }
        if (from + count > m_highWater) m_highWater = from + count;
    }

    void SectorBitmap::Free(size_t from, size_t count) {
        if (count == 0) return;
        EnsureBits(from + count);
        for (size_t i = from; i < from + count; ++i) {
            m_words[i / kBitsPerWord] &= ~(uint64_t{1} << (i % kBitsPerWord));
        }
        // m_highWater is deliberately not lowered. It only ever bounds where
        // Allocate starts extending, and shrinking it would let a later
        // allocation hand out sectors that the file does not physically cover.
    }

    bool SectorBitmap::Test(size_t index) const {
        const size_t w = index / kBitsPerWord;
        if (w >= m_words.size()) return false;
        return (m_words[w] >> (index % kBitsPerWord)) & 1u;
    }

    size_t SectorBitmap::Allocate(size_t count) {
        if (count == 0) return m_highWater;

        // First fit over the used range, exactly like RegionBitmap.allocate.
        size_t run = 0;
        for (size_t i = 2; i < m_highWater; ++i) {
            if (Test(i)) {
                run = 0;
            } else if (++run == count) {
                const size_t start = i + 1 - count;
                Force(start, count);
                return start;
            }
        }
        // No gap big enough: extend past the last used sector.
        const size_t start = m_highWater;
        Force(start, count);
        return start;
    }

    // ── AnvilRegion ─────────────────────────────────────────────────────────

    AnvilRegion::~AnvilRegion() {
        if (m_file) {
            std::string ignored;
            if (m_writable) Flush(ignored);
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

    std::unique_ptr<AnvilRegion> AnvilRegion::Open(const std::filesystem::path& file,
                                                   bool writable, std::string& error) {
        std::unique_ptr<AnvilRegion> r(new AnvilRegion());
        r->m_path     = file;
        r->m_dir      = file.parent_path();
        r->m_writable = writable;

        // r.<x>.<z>.mca -> the coordinates, needed for .mcc names.
        {
            const std::string stem = file.filename().string();
            int rx = 0, rz = 0;
            if (std::sscanf(stem.c_str(), "r.%d.%d.mca", &rx, &rz) == 2) {
                r->m_regionX = rx;
                r->m_regionZ = rz;
            }
        }

        if (writable) {
            std::error_code ec;
            std::filesystem::create_directories(r->m_dir, ec);
        }

        if (writable) {
            // "r+b" preserves an existing file; it fails when there is none,
            // and only then do we create one. Opening "w+b" unconditionally
            // would truncate every region file on load.
            r->m_file = OpenFile(file, "r+b");
            if (!r->m_file) r->m_file = OpenFile(file, "w+b");
        } else {
            r->m_file = OpenFile(file, "rb");
        }
        if (!r->m_file) { error = Errno("open " + file.string()); return nullptr; }
        std::setvbuf(r->m_file, nullptr, _IONBF, 0);   // we do our own buffering

        if (OBEY_FSEEK(r->m_file, 0, SEEK_END) != 0) { error = Errno("seek"); return nullptr; }
        const int64_t size = OBEY_FTELL(r->m_file);
        if (size < 0) { error = Errno("tell"); return nullptr; }
        r->m_fileSize = static_cast<uint64_t>(size);

        if (!r->ReadHeader(error)) return nullptr;
        return r;
    }

    bool AnvilRegion::ReadAt(uint64_t offset, void* dst, size_t n, std::string& error) {
        if (n == 0) return true;
        if (OBEY_FSEEK(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) { error = Errno("seek"); return false; }
        if (std::fread(dst, 1, n, m_file) != n) {
            error = std::feof(m_file) ? "unexpected end of region file" : Errno("read");
            return false;
        }
        return true;
    }

    bool AnvilRegion::WriteAt(uint64_t offset, const void* src, size_t n, std::string& error) {
        if (n == 0) return true;
        if (OBEY_FSEEK(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) { error = Errno("seek"); return false; }
        if (std::fwrite(src, 1, n, m_file) != n) { error = Errno("write"); return false; }
        if (offset + n > m_fileSize) m_fileSize = offset + n;
        return true;
    }

    bool AnvilRegion::ReadHeader(std::string& error) {
        // A brand-new (or empty) file has no header yet; every slot reads as
        // absent and the first write lays one down.
        if (m_fileSize < static_cast<uint64_t>(kHeaderBytes)) {
            if (m_fileSize != 0) {
                Log::Warning("[Anvil] %s: truncated header (%llu bytes), treating as empty",
                             m_path.filename().string().c_str(),
                             static_cast<unsigned long long>(m_fileSize));
            }
            return true;
        }

        std::vector<uint8_t> header(kHeaderBytes);
        if (!ReadAt(0, header.data(), header.size(), error)) return false;

        const size_t fileSectors = static_cast<size_t>(m_fileSize / kSectorBytes);

        for (int i = 0; i < kSlots; ++i) {
            m_offsets[i]    = ReadBE32(&header[i * 4]);
            m_timestamps[i] = ReadBE32(&header[kSectorBytes + i * 4]);

            const uint32_t entry = m_offsets[i];
            if (entry == 0) continue;

            const size_t sector = entry >> 8;
            const size_t count  = entry & 0xFF;

            // Validate BEFORE reserving. The terrain-library port feeds raw
            // header values straight into force(), so a corrupt file throws
            // out of a constructor; here a bad slot is just an absent chunk.
            const bool sane = sector >= 2 && count >= 1 && (sector + count) <= fileSectors;
            if (!sane) {
                Log::Warning("[Anvil] %s: slot %d has a bad extent (sector %zu, count %zu, file %zu sectors) — dropping it",
                             m_path.filename().string().c_str(), i, sector, count, fileSectors);
                m_offsets[i] = 0;
                continue;
            }
            m_used.Force(sector, count);
        }
        return true;
    }

    bool AnvilRegion::WriteHeader(std::string& error) {
        std::vector<uint8_t> header(kHeaderBytes, 0);
        for (int i = 0; i < kSlots; ++i) {
            WriteBE32(&header[i * 4], m_offsets[i]);
            WriteBE32(&header[kSectorBytes + i * 4], m_timestamps[i]);
        }
        return WriteAt(0, header.data(), header.size(), error);
    }

    bool AnvilRegion::Has(int localX, int localZ) const {
        const int slot = SlotIndex(localX, localZ);
        if (slot < 0 || slot >= kSlots) return false;
        return m_offsets[slot] != 0;
    }

    std::filesystem::path AnvilRegion::ExternalPath(int localX, int localZ) const {
        // Vanilla names the .mcc by ABSOLUTE chunk coordinates and puts it in
        // the same folder as the .mca.
        const int cx = m_regionX * 32 + localX;
        const int cz = m_regionZ * 32 + localZ;
        return m_dir / ("c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc");
    }

    bool AnvilRegion::Inflate(const uint8_t* src, size_t n, uint8_t compression,
                              std::vector<uint8_t>& out, std::string& error) {
        if (compression == kCompressionNone) {
            out.assign(src, src + n);
            return true;
        }
        if (compression == kCompressionLz4) {
            error = "LZ4-compressed chunk (region-file-compression=lz4) is not supported";
            return false;
        }
        if (compression != kCompressionZlib && compression != kCompressionGzip) {
            error = "unknown compression id " + std::to_string(compression);
            return false;
        }

        z_stream s{};
        // 15 + 32 auto-detects zlib vs gzip, which covers ids 1 and 2 with one
        // path and also tolerates a file whose id byte disagrees with its
        // actual wrapper.
        if (inflateInit2(&s, 15 + 32) != Z_OK) { error = "inflateInit2 failed"; return false; }

        std::vector<uint8_t> result;
        result.resize(n * 4 + 8192);
        s.next_in  = const_cast<Bytef*>(src);
        s.avail_in = static_cast<uInt>(n);

        size_t produced = 0;
        for (;;) {
            if (produced == result.size()) {
                if (result.size() >= kMaxDecompressedBytes) {
                    inflateEnd(&s);
                    // Say the real numbers. "implausibly large" told a player
                    // nothing about what was wrong or what to do about it.
                    error = "chunk inflates past the "
                          + std::to_string(kMaxDecompressedBytes / (1024 * 1024))
                          + " MB cap (" + std::to_string(n / 1024) + " KB compressed)";
                    return false;
                }
                result.resize(std::min(result.size() * 2, kMaxDecompressedBytes));
            }
            s.next_out  = result.data() + produced;
            s.avail_out = static_cast<uInt>(result.size() - produced);

            const int rc = inflate(&s, Z_NO_FLUSH);
            produced = result.size() - s.avail_out;

            if (rc == Z_STREAM_END) break;
            if (rc == Z_OK) continue;
            // Z_BUF_ERROR here means no progress is possible — with input
            // exhausted it is a truncated stream, not a small output buffer.
            // The terrain-library port loops forever on exactly this.
            inflateEnd(&s);
            error = "inflate failed (" + std::to_string(rc) + ")";
            return false;
        }
        inflateEnd(&s);
        result.resize(produced);
        out = std::move(result);
        return true;
    }

    bool AnvilRegion::Read(int localX, int localZ, std::vector<uint8_t>& out, std::string& error) {
        error.clear();
        const int slot = SlotIndex(localX, localZ);
        if (slot < 0 || slot >= kSlots) { error = "chunk is outside this region"; return false; }

        const uint32_t entry = m_offsets[slot];
        if (entry == 0) return false;                       // absent, not an error

        const size_t sector = entry >> 8;
        const size_t count  = entry & 0xFF;
        const uint64_t base = static_cast<uint64_t>(sector) * kSectorBytes;

        uint8_t head[kChunkHeader];
        if (!ReadAt(base, head, sizeof(head), error)) return false;

        const int32_t declared = static_cast<int32_t>(ReadBE32(head));
        const uint8_t version  = head[4];

        if (declared <= 0) { error = "chunk stream length is " + std::to_string(declared); return false; }

        if (version & kExternalFlag) {
            const auto ext = ExternalPath(localX, localZ);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(ext, ec)) {
                error = "external chunk file is missing: " + ext.filename().string();
                return false;
            }
            const auto extSize = std::filesystem::file_size(ext, ec);
            if (ec) { error = "cannot size " + ext.filename().string(); return false; }
            // Compressed bytes against the COMPRESSED cap. This used to test
            // against kMaxDecompressedBytes, which is a different quantity
            // entirely — see the note on the two caps.
            if (extSize > kMaxCompressedBytes) {
                error = "external chunk file is " + std::to_string(extSize / (1024 * 1024))
                      + " MB, past the " + std::to_string(kMaxCompressedBytes / (1024 * 1024))
                      + " MB compressed cap (" + ExternalPath(localX, localZ).filename().string()
                      + ")";
                return false;
            }

            std::vector<uint8_t> raw(static_cast<size_t>(extSize));
            std::FILE* f = OpenFile(ext, "rb");
            if (!f) { error = Errno("open " + ext.string()); return false; }
            const size_t got = std::fread(raw.data(), 1, raw.size(), f);
            std::fclose(f);
            if (got != raw.size()) { error = "short read on " + ext.filename().string(); return false; }

            // The .mcc holds the compressed stream only — no 5-byte header.
            return Inflate(raw.data(), raw.size(), static_cast<uint8_t>(version & ~kExternalFlag), out, error);
        }

        const size_t payload = static_cast<size_t>(declared) - 1;
        const size_t avail   = count * kSectorBytes - kChunkHeader;
        if (payload > avail) {
            error = "chunk stream is truncated (declared " + std::to_string(payload)
                  + " bytes, sectors hold " + std::to_string(avail) + ")";
            return false;
        }

        std::vector<uint8_t> raw(payload);
        if (payload > 0 && !ReadAt(base + kChunkHeader, raw.data(), raw.size(), error)) return false;
        return Inflate(raw.data(), raw.size(), version, out, error);
    }

    bool AnvilRegion::Write(int localX, int localZ, const std::vector<uint8_t>& payload,
                            uint8_t compression, std::string& error) {
        if (!m_writable) { error = "region opened read-only"; return false; }
        const int slot = SlotIndex(localX, localZ);
        if (slot < 0 || slot >= kSlots) { error = "chunk is outside this region"; return false; }
        if (payload.empty())            { error = "refusing to write an empty payload"; return false; }

        const uint32_t oldEntry  = m_offsets[slot];
        const size_t   oldSector = oldEntry >> 8;
        const size_t   oldCount  = oldEntry & 0xFF;

        const size_t framed  = payload.size() + kChunkHeader;
        size_t       sectors = SectorsFor(framed);
        const bool   external = sectors >= kExternalThresholdSectors;

        std::filesystem::path extTmp, extFinal;
        if (external) {
            // Oversized: the stream goes to a sibling .mcc and the region keeps
            // a 5-byte stub in one sector.
            extFinal = ExternalPath(localX, localZ);
            extTmp   = extFinal;
            extTmp  += ".tmp";
            std::FILE* f = OpenFile(extTmp, "wb");
            if (!f) { error = Errno("create " + extTmp.string()); return false; }
            const size_t put = std::fwrite(payload.data(), 1, payload.size(), f);
            const bool flushed = (std::fflush(f) == 0);
            std::fclose(f);
            if (put != payload.size() || !flushed) {
                std::error_code ec; std::filesystem::remove(extTmp, ec);
                error = "short write on " + extTmp.filename().string();
                return false;
            }
            sectors = 1;
        }

        // ALLOCATE BEFORE FREEING. The old sectors stay reserved until the new
        // payload and the new header are both on disk, so a crash at any point
        // leaves the previous chunk intact and reachable. The port in
        // my_terrain_library frees first and can overwrite the live copy.
        const size_t newSector = m_used.Allocate(sectors);
        const uint64_t base    = static_cast<uint64_t>(newSector) * kSectorBytes;

        std::vector<uint8_t> block(sectors * kSectorBytes, 0);
        if (external) {
            WriteBE32(block.data(), 1);                                  // length: version byte only
            block[4] = static_cast<uint8_t>(compression | kExternalFlag);
        } else {
            WriteBE32(block.data(), static_cast<uint32_t>(payload.size() + 1));
            block[4] = compression;
            std::memcpy(block.data() + kChunkHeader, payload.data(), payload.size());
        }
        // The tail of the final sector is zeroed rather than left as whatever
        // was there. Vanilla tolerates garbage; zeroing makes a byte-diff of
        // two runs meaningful.
        if (!WriteAt(base, block.data(), block.size(), error)) return false;
        if (!Sync(error, "payload")) return false;

        m_offsets[slot]    = static_cast<uint32_t>((newSector << 8) | (sectors & 0xFF));
        m_timestamps[slot] = static_cast<uint32_t>(std::time(nullptr));
        if (!WriteHeader(error)) return false;
        if (!Sync(error, "header")) return false;

        if (external) {
            std::error_code ec;
            std::filesystem::rename(extTmp, extFinal, ec);
            if (ec) { error = "cannot commit " + extFinal.filename().string(); return false; }
        } else if (oldEntry != 0) {
            // A chunk that used to be external and no longer is leaves a stale
            // .mcc that Read would never consult but that would linger forever.
            std::error_code ec;
            std::filesystem::remove(ExternalPath(localX, localZ), ec);
        }

        // Only now is the old extent unreachable.
        if (oldEntry != 0 && oldSector >= 2) m_used.Free(oldSector, oldCount);
        return true;
    }

    bool AnvilRegion::Clear(int localX, int localZ, std::string& error) {
        if (!m_writable) { error = "region opened read-only"; return false; }
        const int slot = SlotIndex(localX, localZ);
        if (slot < 0 || slot >= kSlots) { error = "chunk is outside this region"; return false; }

        const uint32_t oldEntry = m_offsets[slot];
        if (oldEntry == 0) return true;

        m_offsets[slot]    = 0;
        m_timestamps[slot] = static_cast<uint32_t>(std::time(nullptr));
        if (!WriteHeader(error)) return false;
        if (!Sync(error, "header")) return false;

        std::error_code ec;
        std::filesystem::remove(ExternalPath(localX, localZ), ec);

        const size_t oldSector = oldEntry >> 8;
        const size_t oldCount  = oldEntry & 0xFF;
        if (oldSector >= 2) m_used.Free(oldSector, oldCount);
        return true;
    }

    bool AnvilRegion::Sync(std::string& error, const char* what) {
        if (std::fflush(m_file) != 0) { error = Errno(std::string("fflush ") + what); return false; }
        if (OBEY_FSYNC(OBEY_FILENO(m_file)) != 0) { error = Errno(std::string("fsync ") + what); return false; }
        return true;
    }

    bool AnvilRegion::Flush(std::string& error) {
        if (!m_writable || !m_file) return true;

        // Vanilla's padToFullSector: a well-formed region file is always a
        // whole number of 4096-byte sectors.
        const uint64_t padded = static_cast<uint64_t>(SectorsFor(static_cast<size_t>(m_fileSize))) * kSectorBytes;
        if (padded != m_fileSize) {
            const uint8_t zero = 0;
            if (!WriteAt(padded - 1, &zero, 1, error)) return false;
        }
        return Sync(error, "flush");
    }

} // namespace Game::Anvil
