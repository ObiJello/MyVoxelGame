// File: src/common/sync/FileDeltaSync.hpp
//
// The rsync-style delta engine for the LAN chat.db migration feature. This
// half is deliberately IO-free and network-free (no Boost.Asio, no sockets):
// hashing, the rolling checksum, block signatures, the delta builder, the
// on-wire frame codec, a read-only memory map, and the streaming file
// reconstruct. FileSyncService drives it over a socket.
//
// Keeping this module out of DebugSystem and free of Asio keeps it easy to
// reason about and impossible for it to drag heavy headers into shared code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Sync {

    // ── 128-bit hash (MurmurHash3 x64 128) ───────────────────────────────────
    // Fast, well-distributed, NON-cryptographic. Used two ways:
    //   • per-block strong hash, to confirm a weak (rolling) checksum match;
    //   • whole-file hash, as the final verification gate before any swap.
    //
    // Collision tradeoff: two distinct 4 KB blocks share a 128-bit Murmur3
    // digest with probability ~2^-128 per pair; across ~10^5 blocks the odds
    // of any false block match are astronomically small. And even a false
    // block match cannot corrupt the target: the reconstructed file's own
    // whole-file 128-bit hash is checked against the source's before anything
    // is swapped, so a bad match only causes a verify failure and an automatic
    // retry on the next launch. There is no adversary here (the user's own two
    // Macs on their own LAN), so a cryptographic hash would buy nothing.
    struct Hash128 {
        uint64_t h1 = 0;
        uint64_t h2 = 0;
        bool operator==(const Hash128& o) const { return h1 == o.h1 && h2 == o.h2; }
        bool operator!=(const Hash128& o) const { return !(*this == o); }
    };

    // One-shot digest of a buffer.
    Hash128 HashBytes(const uint8_t* data, size_t n, uint32_t seed = 0);

    // Streaming digest for the whole file (never buffers more than 16 bytes).
    class Murmur3Incremental {
    public:
        explicit Murmur3Incremental(uint32_t seed = 0) : m_h1(seed), m_h2(seed) {}
        void Update(const uint8_t* data, size_t n);
        Hash128 Finish() const;   // non-destructive
    private:
        uint64_t m_h1, m_h2;
        uint64_t m_total = 0;
        uint8_t  m_tail[16] = {};
        size_t   m_tailLen = 0;
    };

    // ── rsync weak rolling checksum (Adler-style, modulo 2^16) ────────────────
    struct RollingChecksum {
        uint32_t a = 0, b = 0;
        uint32_t Value() const { return (a & 0xFFFF) | (b << 16); }
        // Compute fresh over window [p, p+len).
        void Reset(const uint8_t* p, size_t len);
        // Slide the window by one byte: drop `out` (leftmost), add `in`
        // (new rightmost). `len` is the window size.
        void Roll(uint8_t out, uint8_t in, size_t len);
    };

    // ── Read-only memory map (POSIX mmap / Windows MapViewOfFile) ─────────────
    // Gives whole-file random access without committing the file to RSS, so a
    // 400 MB file costs no heap. A zero-length or missing file maps to
    // {data=nullptr, size=0} with valid()==false.
    class MappedFile {
    public:
        MappedFile() = default;
        ~MappedFile() { Close(); }
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        bool Open(const std::string& path);   // false if missing/empty/failed
        void Close();
        bool valid() const { return m_data != nullptr; }
        const uint8_t* data() const { return m_data; }
        uint64_t size() const { return m_size; }
    private:
        const uint8_t* m_data = nullptr;
        uint64_t m_size = 0;
#ifdef _WIN32
        void* m_fileHandle = nullptr;   // HANDLE
        void* m_mapHandle = nullptr;    // HANDLE
#else
        int m_fd = -1;
        size_t m_mapLen = 0;
#endif
    };

    // ── Block signatures (built by the client from its base/target file) ──────
    inline constexpr uint32_t kBlockSize = 4096;   // ~4 KB rsync blocks

    struct BlockSignature {
        uint32_t weak = 0;
        Hash128  strong;
    };

    // Signs full blocks of `kBlockSize`; a trailing partial block is not
    // signed (its bytes simply arrive as literals — correct, slightly less
    // efficient). Reads sequentially from the map.
    std::vector<BlockSignature> ComputeSignatures(const MappedFile& base);

    // ── Delta ops (server scans its new file against client signatures) ───────
    // Coalesces contiguous COPYs and chunks literals. baseOffset/len refer to
    // the CLIENT's base file. Returns false only on a hard internal error.
    struct DeltaSink {
        std::function<void(uint64_t baseOffset, uint64_t len)> onCopy;
        std::function<void(const uint8_t* data, uint64_t len)> onLiteral;
    };

    void BuildDelta(const uint8_t* newData, uint64_t newSize,
                    uint32_t blockSize,
                    const std::vector<BlockSignature>& clientSigs,
                    const DeltaSink& sink);

    // ── On-wire frame codec ───────────────────────────────────────────────────
    // Frame = [u8 type][u32 payloadLen LE][payload][u32 crc32 LE of type+payload].
    enum class MsgType : uint8_t {
        Signatures = 1,   // client -> server
        FileInfo   = 2,   // server -> client
        Copy       = 3,   // server -> client
        Literal    = 4,   // server -> client
        End        = 5,   // server -> client
        Result     = 6,   // client -> server
        Refuse     = 7,   // server -> client (e.g. Messages is running)
        Hello      = 8,   // client -> server: friendship ticket (gates the direct path)
    };

    // Sane caps checked before any allocation.
    inline constexpr uint32_t kMaxFramePayload = 64u * 1024u * 1024u;   // 64 MiB
    inline constexpr uint64_t kMaxFileSize     = 8ull * 1024 * 1024 * 1024; // 8 GiB
    inline constexpr uint64_t kLiteralChunk    = 512u * 1024u;          // flush literals in 512 KiB chunks

    // Little-endian helpers.
    void PutU16(std::vector<uint8_t>& v, uint16_t x);
    void PutU32(std::vector<uint8_t>& v, uint32_t x);
    void PutU64(std::vector<uint8_t>& v, uint64_t x);
    uint16_t GetU16(const uint8_t* p);
    uint32_t GetU32(const uint8_t* p);
    uint64_t GetU64(const uint8_t* p);

    // Build a full framed message (header + payload + trailing crc) ready to
    // write to the socket.
    std::vector<uint8_t> EncodeFrame(MsgType type, const std::vector<uint8_t>& payload);

    // Parse a header (5 bytes: type + payloadLen). Returns false if the length
    // exceeds kMaxFramePayload.
    bool DecodeHeader(const uint8_t header[5], MsgType& outType, uint32_t& outLen);

    // Verify a received frame's trailing crc against (type + payload).
    bool VerifyCrc(MsgType type, const uint8_t* payload, uint32_t len, uint32_t crc);

    // ── Runtime path helpers ──────────────────────────────────────────────────
    std::string ExpandTilde(const std::string& path);

} // namespace Sync
