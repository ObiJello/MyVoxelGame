// File: src/server/world/storage/anvil/RegionStore.hpp
//
// The open-region-file cache.
//
// NOT THREAD-SAFE, AND DELIBERATELY SO. AnvilRegion holds an unsynchronised
// FILE* and a sector bitmap; the caller must serialise every call. AnvilChunkIo
// is that caller and owns the mutex.
//
// ONE store serves both directions, which is not a detail. Reads happen on
// chunk-worker threads (ChunkProvider::GetChunk blocks on one) while writes
// happen on the storage thread, and a write RELOCATES a chunk — allocate new
// sectors, patch the header, free the old. A second store with its own cached
// sector table would still be pointing at the freed extent, which by then may
// hold a different chunk entirely. That is exactly the read/write skew the
// old RegionFile + AnvilRegionWriter pair had.
//
// Vanilla keeps 256 region files open (RegionFileStorage.MAX_CACHE_SIZE) and
// evicts least-recently-used; matching that keeps the file-descriptor cost
// predictable on a world being streamed in every direction.
#pragma once

#include "server/world/storage/anvil/AnvilRegion.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include "common/world/level/DimensionId.hpp"

#include <filesystem>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace Game::Anvil {

    // Which family of region files: chunk terrain, or the 1.17+ split entity
    // storage. Same container, different folder and different NBT payload.
    enum class RegionKind : uint8_t { Chunks = 0, Entities = 1 };

    class RegionStore {
    public:
        static constexpr size_t kMaxOpenRegions = 256;   // RegionFileStorage.MAX_CACHE_SIZE

        // WRITABLE stores can only be built from a SaveRoot. That is the whole
        // point of the token: a folder outside obeycraft/saves cannot produce
        // one, so it cannot be written to.
        explicit RegionStore(SaveRoot root);

        // READ-ONLY over an arbitrary world folder — an imported Minecraft
        // save. Reading is not the dangerous direction, and gating it would
        // mean we could not open the player's own worlds at all.
        static std::unique_ptr<RegionStore> OpenReadOnly(std::filesystem::path worldRoot);

        ~RegionStore();

        RegionStore(const RegionStore&)            = delete;
        RegionStore& operator=(const RegionStore&) = delete;

        // Caller must hold AnvilChunkIo's mutex.
        //
        // `createIfMissing` must be false for a read. A writable store opens
        // its files O_CREAT, so a read that went through the same path would
        // leave an empty .mca behind for every chunk the player merely walked
        // near — which is exactly what the first run of this produced.
        //
        // Returns nullptr when the file is absent (and not being created) or
        // cannot be opened; `error` is set only for a real failure.
        AnvilRegion* Get(DimensionId dim, RegionKind kind, int regionX, int regionZ,
                         std::string& error, bool createIfMissing);

        // Convenience: resolve a chunk to its region and local coordinates.
        static int RegionCoord(int chunkCoord) { return chunkCoord >> 5; }      // floors for negatives
        static int LocalCoord (int chunkCoord) { return chunkCoord & 31; }

        // Flush and close everything. Safe to call twice.
        void CloseAll();

        size_t OpenCount() const { return m_lru.size(); }

    private:
        struct Key {
            DimensionId dim;
            RegionKind  kind;
            int32_t     rx;
            int32_t     rz;
            bool operator==(const Key& o) const {
                return dim == o.dim && kind == o.kind && rx == o.rx && rz == o.rz;
            }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const {
                // Dimension and kind are tiny; fold them into the high bits so
                // neighbouring regions of one dimension stay well spread.
                uint64_t h = (uint64_t(uint32_t(k.rx)) << 32) ^ uint32_t(k.rz);
                h ^= (uint64_t(static_cast<uint8_t>(k.dim)) << 3) ^ static_cast<uint8_t>(k.kind);
                h *= 0x9E3779B97F4A7C15ull;
                return static_cast<size_t>(h ^ (h >> 29));
            }
        };
        struct Entry {
            std::unique_ptr<AnvilRegion> region;
            std::list<Key>::iterator     lruPos;
        };

        std::filesystem::path FilePath(DimensionId dim, RegionKind kind, int rx, int rz) const;

        RegionStore(std::filesystem::path worldRoot, bool writable)
            : m_worldRoot(std::move(worldRoot)), m_writable(writable) {}

        std::filesystem::path m_worldRoot;
        bool                  m_writable;

        std::unordered_map<Key, Entry, KeyHash> m_open;
        std::list<Key>                          m_lru;    // front = most recent
    };

} // namespace Game::Anvil
