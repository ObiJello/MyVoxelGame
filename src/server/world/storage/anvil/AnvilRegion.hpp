// File: src/server/world/storage/anvil/AnvilRegion.hpp
//
// One .mca region file, read AND write. Modelled directly on vanilla
// RegionFile.java (minecraft_code/decompiled_net/.../chunk/storage/), not on
// the port in my_terrain_library — that port inverts the allocate/free order
// (it frees the old sectors before allocating the new ones, so a write can
// land on the only copy of the chunk), caps the sector bitmap at a 32 MB file
// with a std::bitset<8192> whose grow path always throws, and silently drops
// any chunk needing more than 255 sectors.
//
// ONE container for both directions, deliberately. The engine used to have a
// read-only RegionFile beside a write-only AnvilRegionWriter; the reader
// cached its sector table for the process lifetime, so the first write that
// relocated a chunk left the reader pointing at sectors that had since been
// handed to a different chunk.
//
// FILE LAYOUT
//   0x0000..0x0FFF   1024 big-endian u32 offsets, (sector << 8) | sectorCount
//                    0 means "chunk not present"
//   0x1000..0x1FFF   1024 big-endian u32 timestamps, unix SECONDS
//   0x2000..         payload sectors, 4096 bytes each
//
// PAYLOAD at sector*4096
//   i32 length       byte count of what follows, INCLUDING the version byte
//   u8  version      1 gzip, 2 zlib (the only one we emit), 3 none, 4 lz4;
//                    bit 0x80 means the payload lives in an external .mcc
//   u8[length-1]     compressed NBT
//
// THREADING: none. One AnvilRegion is owned and touched by exactly one
// thread; RegionStore enforces that.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Game::Anvil {

    // Free-sector tracker. Vanilla's RegionBitmap, but over a growable
    // vector<uint64_t> rather than a fixed bitset, so a region larger than
    // 32 MB is an ordinary file rather than an exception.
    class SectorBitmap {
    public:
        SectorBitmap() { Force(0, 2); }          // sectors 0 and 1 are the header

        void Force(size_t from, size_t count);
        void Free (size_t from, size_t count);
        bool Test (size_t index) const;

        // First-fit over free runs, exactly like RegionBitmap.allocate: scan
        // for a gap of `count`, else extend past the end.
        size_t Allocate(size_t count);

        size_t HighestUsedEnd() const { return m_highWater; }

    private:
        void EnsureBits(size_t bits);

        std::vector<uint64_t> m_words;
        size_t                m_highWater = 2;   // one past the last used sector
    };

    class AnvilRegion {
    public:
        static constexpr int      kSectorBytes = 4096;
        static constexpr int      kSlots       = 1024;   // 32x32 chunks
        static constexpr int      kHeaderBytes = 2 * kSectorBytes;
        static constexpr int      kChunkHeader = 5;      // i32 length + u8 version

        static constexpr uint8_t  kCompressionGzip = 1;
        static constexpr uint8_t  kCompressionZlib = 2;  // vanilla default, the only one we write
        static constexpr uint8_t  kCompressionNone = 3;
        static constexpr uint8_t  kCompressionLz4  = 4;
        static constexpr uint8_t  kExternalFlag    = 0x80;

        // A payload needing this many sectors goes to region/c.<cx>.<cz>.mcc
        // and leaves a 5-byte stub behind.
        static constexpr size_t   kExternalThresholdSectors = 256;

        // Two SEPARATE caps, because they bound two different quantities and
        // conflating them was a real bug: the external-file check compared a
        // COMPRESSED on-disk size against the DECOMPRESSED cap, so a .mcc was
        // rejected for being bigger than a limit that never applied to it.
        //
        // Neither exists in MC — RegionFile.createExternalChunkInputStream just
        // opens the file. They are ours, and their only job is to stop a
        // corrupt length field or a crafted file from asking for all of memory.
        // So they are set high enough that no legitimate chunk trips them.
        //
        // For scale: a normal region chunk is tens of KB, a normal external
        // .mcc under a megabyte. A real observed pathological case — one chunk
        // holding a million primed TNT entities — was 64 MB compressed and
        // 1,096 MB inflated, a 17x ratio. That one is genuinely beyond what the
        // server can hold, and refusing it is the correct outcome; what was
        // wrong was refusing it for the wrong reason with an unactionable
        // message.
        static constexpr size_t   kMaxCompressedBytes   = 128u * 1024u * 1024u;
        static constexpr size_t   kMaxDecompressedBytes = 256u * 1024u * 1024u;

        ~AnvilRegion();
        AnvilRegion(const AnvilRegion&)            = delete;
        AnvilRegion& operator=(const AnvilRegion&) = delete;

        // `writable` false opens read-only and makes Write/Clear refuse.
        // A missing file is created only when writable.
        static std::unique_ptr<AnvilRegion> Open(const std::filesystem::path& file,
                                                 bool writable,
                                                 std::string& error);

        bool Has(int localX, int localZ) const;

        // Decompressed NBT bytes. Returns false with `error` set on a real
        // failure; returns false with `error` EMPTY when the chunk is simply
        // absent, which is the common case and not worth logging.
        bool Read(int localX, int localZ, std::vector<uint8_t>& out, std::string& error);

        // `payload` must ALREADY be compressed with `compression`. Compression
        // happens on the caller's thread so it stays off the single I/O thread.
        bool Write(int localX, int localZ, const std::vector<uint8_t>& payload,
                   uint8_t compression, std::string& error);

        // Drop a chunk: zero its slot, free its sectors, delete any .mcc.
        // entities/*.mca needs this — an entity chunk that empties out is
        // removed rather than written as an empty list.
        bool Clear(int localX, int localZ, std::string& error);

        // Pad to a whole sector and flush. Vanilla guarantees a region file is
        // always a multiple of 4096 bytes.
        bool Flush(std::string& error);

        const std::filesystem::path& Path() const { return m_path; }

        static int SlotIndex(int localX, int localZ) { return localX + localZ * 32; }

    private:
        AnvilRegion() = default;

        bool ReadHeader(std::string& error);
        bool WriteHeader(std::string& error);
        bool ReadAt (uint64_t offset, void* dst, size_t n, std::string& error);
        bool WriteAt(uint64_t offset, const void* src, size_t n, std::string& error);
        bool Sync(std::string& error, const char* what);
        bool Inflate(const uint8_t* src, size_t n, uint8_t compression,
                     std::vector<uint8_t>& out, std::string& error);
        std::filesystem::path ExternalPath(int localX, int localZ) const;

        std::filesystem::path m_path;
        std::filesystem::path m_dir;
        int                   m_regionX = 0;
        int                   m_regionZ = 0;
        std::FILE*            m_file    = nullptr;
        bool                  m_writable = false;
        uint64_t              m_fileSize = 0;

        uint32_t     m_offsets[kSlots]    = {};
        uint32_t     m_timestamps[kSlots] = {};
        SectorBitmap m_used;
    };

} // namespace Game::Anvil
