#pragma once

#include "world/level/chunk/storage/RegionFile.h"
#include "nbt/CompoundTag.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <optional>

// Reference: net/minecraft/world/level/chunk/storage/RegionFileStorage.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

/**
 * RegionFileStorage - Manages a cache of RegionFile instances
 * Reference: RegionFileStorage.java
 */
class RegionFileStorage {
public:
    // Reference: RegionFileStorage.java constructor
    RegionFileStorage(const RegionStorageInfo& info, const std::string& folder, bool sync);

    ~RegionFileStorage();

    // Reference: RegionFileStorage.java read(ChunkPos)
    std::unique_ptr<nbt::CompoundTag> read(const ChunkPos& pos);

    // Reference: RegionFileStorage.java write(ChunkPos, CompoundTag)
    void write(const ChunkPos& pos, const nbt::CompoundTag* tag);

    // Reference: RegionFileStorage.java flush()
    void flush();

    // Reference: RegionFileStorage.java close()
    void close();

    // Reference: RegionFileStorage.java hasChunk(ChunkPos)
    bool hasChunk(const ChunkPos& pos);

    // Get storage info
    const RegionStorageInfo& getInfo() const { return m_info; }

protected:
    // Reference: RegionFileStorage.java getRegionFile(ChunkPos)
    RegionFile* getRegionFile(const ChunkPos& pos);

private:
    RegionStorageInfo m_info;
    std::string m_folder;
    bool m_sync;

    mutable std::mutex m_mutex;
    std::unordered_map<int64_t, std::unique_ptr<RegionFile>> m_regionCache;

    // Pack region coordinates into a single key
    static int64_t getRegionKey(int regionX, int regionZ) {
        return (static_cast<int64_t>(regionX) & 0xFFFFFFFFL) |
               ((static_cast<int64_t>(regionZ) & 0xFFFFFFFFL) << 32);
    }
};

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
