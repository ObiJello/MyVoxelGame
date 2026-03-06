#include "world/level/chunk/storage/RegionFileStorage.h"
#include "nbt/NbtIo.h"
#include <filesystem>
#include <sstream>

// Reference: net/minecraft/world/level/chunk/storage/RegionFileStorage.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

// Reference: RegionFileStorage.java constructor
RegionFileStorage::RegionFileStorage(const RegionStorageInfo& info,
                                     const std::string& folder, bool sync)
    : m_info(info)
    , m_folder(folder)
    , m_sync(sync)
{
    // Create directory if needed
    std::filesystem::create_directories(folder);
}

RegionFileStorage::~RegionFileStorage() {
    close();
}

// Reference: RegionFileStorage.java getRegionFile(ChunkPos)
RegionFile* RegionFileStorage::getRegionFile(const ChunkPos& pos) {
    int regionX = RegionFile::getRegionX(pos.x());
    int regionZ = RegionFile::getRegionZ(pos.z());
    int64_t key = getRegionKey(regionX, regionZ);

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_regionCache.find(key);
    if (it != m_regionCache.end()) {
        return it->second.get();
    }

    // Create new RegionFile
    std::string fileName = RegionFile::getRegionFileName(regionX, regionZ);
    std::string path = m_folder + "/" + fileName;
    std::string externalDir = m_folder + "/external";

    try {
        auto regionFile = std::make_unique<RegionFile>(m_info, path, externalDir, m_sync);
        RegionFile* ptr = regionFile.get();
        m_regionCache[key] = std::move(regionFile);
        return ptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}

// Reference: RegionFileStorage.java read(ChunkPos)
std::unique_ptr<nbt::CompoundTag> RegionFileStorage::read(const ChunkPos& pos) {
    RegionFile* regionFile = getRegionFile(pos);
    if (!regionFile) {
        return nullptr;
    }

    auto inputStream = regionFile->getChunkDataInputStream(pos);
    if (!inputStream) {
        return nullptr;
    }

    try {
        return nbt::NbtIo::read(*inputStream);
    } catch (const std::exception&) {
        return nullptr;
    }
}

// Reference: RegionFileStorage.java write(ChunkPos, CompoundTag)
void RegionFileStorage::write(const ChunkPos& pos, const nbt::CompoundTag* tag) {
    RegionFile* regionFile = getRegionFile(pos);
    if (!regionFile) {
        return;
    }

    if (tag == nullptr) {
        // Remove chunk (write empty data)
        // For now, we don't support removal - just skip
        return;
    }

    auto outputStream = regionFile->getChunkDataOutputStream(pos);
    if (!outputStream) {
        return;
    }

    try {
        nbt::NbtIo::write(*tag, *outputStream);
        // Stream finalizes on destruction
    } catch (const std::exception&) {
        // Log error
    }
}

// Reference: RegionFileStorage.java hasChunk(ChunkPos)
bool RegionFileStorage::hasChunk(const ChunkPos& pos) {
    RegionFile* regionFile = getRegionFile(pos);
    if (!regionFile) {
        return false;
    }
    return regionFile->hasChunk(pos);
}

// Reference: RegionFileStorage.java flush()
void RegionFileStorage::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_regionCache) {
        if (pair.second) {
            pair.second->flush();
        }
    }
}

// Reference: RegionFileStorage.java close()
void RegionFileStorage::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_regionCache) {
        if (pair.second) {
            pair.second->close();
        }
    }
    m_regionCache.clear();
}

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
