// File: src/common/sync/FileDeltaSync.cpp
#include "common/sync/FileDeltaSync.hpp"

#include "common/core/Deflate.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace Sync {

    // ── MurmurHash3 x64 128 ──────────────────────────────────────────────────
    namespace {
        inline uint64_t Rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
        inline uint64_t Fmix64(uint64_t k) {
            k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
            k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
            k ^= k >> 33; return k;
        }
        constexpr uint64_t kC1 = 0x87c37b91114253d5ULL;
        constexpr uint64_t kC2 = 0x4cf5ad432745937fULL;

        inline uint64_t Load64(const uint8_t* p) {
            uint64_t x; std::memcpy(&x, p, 8); return x;   // little-endian hosts
        }

        // Mix one full 16-byte block into (h1,h2).
        inline void MixBlock(uint64_t& h1, uint64_t& h2, const uint8_t* block) {
            uint64_t k1 = Load64(block);
            uint64_t k2 = Load64(block + 8);
            k1 *= kC1; k1 = Rotl64(k1, 31); k1 *= kC2; h1 ^= k1;
            h1 = Rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
            k2 *= kC2; k2 = Rotl64(k2, 33); k2 *= kC1; h2 ^= k2;
            h2 = Rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
        }

        // Fold the trailing 0..15 bytes and finalize.
        inline Hash128 Finalize(uint64_t h1, uint64_t h2, const uint8_t* tail,
                                size_t tailLen, uint64_t totalLen) {
            uint64_t k1 = 0, k2 = 0;
            switch (tailLen & 15) {
                case 15: k2 ^= uint64_t(tail[14]) << 48; [[fallthrough]];
                case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
                case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
                case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
                case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
                case 10: k2 ^= uint64_t(tail[9])  << 8;  [[fallthrough]];
                case 9:  k2 ^= uint64_t(tail[8]);
                         k2 *= kC2; k2 = Rotl64(k2, 33); k2 *= kC1; h2 ^= k2;
                         [[fallthrough]];
                case 8:  k1 ^= uint64_t(tail[7]) << 56; [[fallthrough]];
                case 7:  k1 ^= uint64_t(tail[6]) << 48; [[fallthrough]];
                case 6:  k1 ^= uint64_t(tail[5]) << 40; [[fallthrough]];
                case 5:  k1 ^= uint64_t(tail[4]) << 32; [[fallthrough]];
                case 4:  k1 ^= uint64_t(tail[3]) << 24; [[fallthrough]];
                case 3:  k1 ^= uint64_t(tail[2]) << 16; [[fallthrough]];
                case 2:  k1 ^= uint64_t(tail[1]) << 8;  [[fallthrough]];
                case 1:  k1 ^= uint64_t(tail[0]);
                         k1 *= kC1; k1 = Rotl64(k1, 31); k1 *= kC2; h1 ^= k1;
                         break;
                default: break;
            }
            h1 ^= totalLen; h2 ^= totalLen;
            h1 += h2; h2 += h1;
            h1 = Fmix64(h1); h2 = Fmix64(h2);
            h1 += h2; h2 += h1;
            return Hash128{h1, h2};
        }
    } // namespace

    Hash128 HashBytes(const uint8_t* data, size_t n, uint32_t seed) {
        uint64_t h1 = seed, h2 = seed;
        const size_t nblocks = n / 16;
        for (size_t i = 0; i < nblocks; ++i) MixBlock(h1, h2, data + i * 16);
        return Finalize(h1, h2, data + nblocks * 16, n & 15, n);
    }

    void Murmur3Incremental::Update(const uint8_t* data, size_t n) {
        m_total += n;
        // Fill a partial tail buffer first.
        if (m_tailLen > 0) {
            const size_t need = 16 - m_tailLen;
            const size_t take = (n < need) ? n : need;
            std::memcpy(m_tail + m_tailLen, data, take);
            m_tailLen += take; data += take; n -= take;
            if (m_tailLen < 16) return;
            MixBlock(m_h1, m_h2, m_tail);
            m_tailLen = 0;
        }
        const size_t nblocks = n / 16;
        for (size_t i = 0; i < nblocks; ++i) MixBlock(m_h1, m_h2, data + i * 16);
        const size_t rem = n & 15;
        if (rem) { std::memcpy(m_tail, data + nblocks * 16, rem); m_tailLen = rem; }
    }

    Hash128 Murmur3Incremental::Finish() const {
        return Finalize(m_h1, m_h2, m_tail, m_tailLen, m_total);
    }

    // ── Rolling checksum ─────────────────────────────────────────────────────
    void RollingChecksum::Reset(const uint8_t* p, size_t len) {
        uint32_t s1 = 0, s2 = 0;
        for (size_t i = 0; i < len; ++i) {
            s1 += p[i];
            s2 += static_cast<uint32_t>(len - i) * p[i];
        }
        a = s1 & 0xFFFF;
        b = s2 & 0xFFFF;
    }

    void RollingChecksum::Roll(uint8_t out, uint8_t in, size_t len) {
        a = (a - out + in) & 0xFFFF;
        b = (b - static_cast<uint32_t>(len) * out + a) & 0xFFFF;
    }

    // ── MappedFile ───────────────────────────────────────────────────────────
    bool MappedFile::Open(const std::string& path) {
        Close();
#ifdef _WIN32
        HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(fh, &sz) || sz.QuadPart <= 0) { CloseHandle(fh); return false; }
        HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mh) { CloseHandle(fh); return false; }
        void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
        if (!view) { CloseHandle(mh); CloseHandle(fh); return false; }
        m_fileHandle = fh;
        m_mapHandle = mh;
        m_data = static_cast<const uint8_t*>(view);
        m_size = static_cast<uint64_t>(sz.QuadPart);
        return true;
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
        const size_t len = static_cast<size_t>(st.st_size);
        void* p = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return false; }
        m_fd = fd;
        m_mapLen = len;
        m_data = static_cast<const uint8_t*>(p);
        m_size = static_cast<uint64_t>(len);
        return true;
#endif
    }

    void MappedFile::Close() {
#ifdef _WIN32
        if (m_data) UnmapViewOfFile(const_cast<uint8_t*>(m_data));
        if (m_mapHandle) CloseHandle(m_mapHandle);
        if (m_fileHandle && m_fileHandle != INVALID_HANDLE_VALUE) CloseHandle(m_fileHandle);
        m_mapHandle = nullptr; m_fileHandle = nullptr;
#else
        if (m_data && m_mapLen) ::munmap(const_cast<uint8_t*>(m_data), m_mapLen);
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1; m_mapLen = 0;
#endif
        m_data = nullptr; m_size = 0;
    }

    // ── Block signatures ─────────────────────────────────────────────────────
    std::vector<BlockSignature> ComputeSignatures(const MappedFile& base) {
        std::vector<BlockSignature> sigs;
        if (!base.valid()) return sigs;
        const uint8_t* p = base.data();
        const uint64_t n = base.size();
        const uint64_t full = n / kBlockSize;
        sigs.reserve(static_cast<size_t>(full));
        for (uint64_t i = 0; i < full; ++i) {
            const uint8_t* blk = p + i * kBlockSize;
            BlockSignature s;
            RollingChecksum rc; rc.Reset(blk, kBlockSize);
            s.weak = rc.Value();
            s.strong = HashBytes(blk, kBlockSize);
            sigs.push_back(s);
        }
        return sigs;
    }

    // ── Delta builder ────────────────────────────────────────────────────────
    void BuildDelta(const uint8_t* newData, uint64_t newSize,
                    uint32_t blockSize,
                    const std::vector<BlockSignature>& clientSigs,
                    const DeltaSink& sink) {
        // Pending coalesced COPY run (contiguous in the base file).
        uint64_t copyOffset = 0, copyLen = 0;
        bool haveCopy = false;
        auto flushCopy = [&]() {
            if (haveCopy) { sink.onCopy(copyOffset, copyLen); haveCopy = false; copyLen = 0; }
        };
        // Pending literal run [litStart, pos) flushed in chunks.
        uint64_t litStart = 0;
        auto flushLiteralUpTo = [&](uint64_t pos) {
            // Emit any pending COPY first so ops stay in file order, and so a
            // COPY run never coalesces across an intervening literal gap.
            if (pos > litStart) flushCopy();
            while (pos > litStart) {
                const uint64_t take = std::min<uint64_t>(pos - litStart, kLiteralChunk);
                sink.onLiteral(newData + litStart, take);
                litStart += take;
            }
        };
        auto emitCopyBlock = [&](uint64_t baseOffset) {
            if (haveCopy && baseOffset == copyOffset + copyLen) {
                copyLen += blockSize;
            } else {
                flushCopy();
                copyOffset = baseOffset; copyLen = blockSize; haveCopy = true;
            }
        };

        // No usable signatures (or empty file): everything is literal.
        if (blockSize == 0 || clientSigs.empty() || newSize < blockSize) {
            flushLiteralUpTo(newSize);
            return;
        }

        // weak -> candidate signature indices.
        std::unordered_map<uint32_t, std::vector<uint32_t>> table;
        table.reserve(clientSigs.size() * 2);
        for (uint32_t i = 0; i < clientSigs.size(); ++i)
            table[clientSigs[i].weak].push_back(i);

        RollingChecksum rc; rc.Reset(newData, blockSize);
        uint64_t pos = 0;
        const uint64_t last = newSize - blockSize;   // inclusive upper bound of window start
        for (;;) {
            bool matched = false;
            auto it = table.find(rc.Value());
            if (it != table.end()) {
                const Hash128 strong = HashBytes(newData + pos, blockSize);
                for (uint32_t idx : it->second) {
                    if (clientSigs[idx].strong == strong) {
                        // Match: flush any literal run that ends here, emit copy.
                        flushLiteralUpTo(pos);
                        emitCopyBlock(static_cast<uint64_t>(idx) * blockSize);
                        pos += blockSize;
                        litStart = pos;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) {
                if (pos > last) break;             // no room for another full window
                rc.Reset(newData + pos, blockSize);
                continue;
            }
            if (pos >= last) break;                // last window start reached, no match
            rc.Roll(newData[pos], newData[pos + blockSize], blockSize);
            ++pos;
        }
        // Everything from the last flushed literal position to EOF is literal
        // (this includes any unmatched tail and the final partial block).
        flushLiteralUpTo(newSize);
        flushCopy();
    }

    // ── Little-endian helpers ─────────────────────────────────────────────────
    void PutU16(std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    }
    void PutU32(std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
    }
    void PutU64(std::vector<uint8_t>& v, uint64_t x) {
        for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
    }
    uint16_t GetU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
    uint32_t GetU32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    uint64_t GetU64(const uint8_t* p) {
        uint64_t x = 0;
        for (int i = 0; i < 8; ++i) x |= uint64_t(p[i]) << (8 * i);
        return x;
    }

    // ── Frame codec ──────────────────────────────────────────────────────────
    std::vector<uint8_t> EncodeFrame(MsgType type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> frame;
        frame.reserve(payload.size() + 9);
        frame.push_back(static_cast<uint8_t>(type));
        PutU32(frame, static_cast<uint32_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());
        // crc over type + payload (everything but the header length field and
        // the crc itself).
        uint32_t crc = Core::Deflate::Crc32(&frame[0], 1);                 // type byte
        if (!payload.empty())
            crc = Core::Deflate::Crc32(payload.data(), payload.size(), crc);
        PutU32(frame, crc);
        return frame;
    }

    bool DecodeHeader(const uint8_t header[5], MsgType& outType, uint32_t& outLen) {
        outType = static_cast<MsgType>(header[0]);
        outLen = GetU32(header + 1);
        return outLen <= kMaxFramePayload;
    }

    bool VerifyCrc(MsgType type, const uint8_t* payload, uint32_t len, uint32_t crc) {
        uint8_t t = static_cast<uint8_t>(type);
        uint32_t c = Core::Deflate::Crc32(&t, 1);
        if (len) c = Core::Deflate::Crc32(payload, len, c);
        return c == crc;
    }

    // ── Path helpers ──────────────────────────────────────────────────────────
    std::string ExpandTilde(const std::string& path) {
        if (path.empty() || path[0] != '~') return path;
        const char* home = std::getenv("HOME");
#ifdef _WIN32
        if (!home) home = std::getenv("USERPROFILE");
#endif
        if (!home) return path;
        if (path.size() == 1) return home;            // "~"
        if (path[1] == '/' || path[1] == '\\') return std::string(home) + path.substr(1);
        return path;                                   // "~user" not supported
    }

} // namespace Sync
