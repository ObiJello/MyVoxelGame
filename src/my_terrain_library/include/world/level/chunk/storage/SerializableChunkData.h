#pragma once

#include "world/ChunkPos.h"
#include "world/IChunk.h"
#include "world/LevelChunkSection.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/level/block/state/BlockState.h"
#include "nbt/CompoundTag.h"
#include "nbt/ListTag.h"
#include <array>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
#include <map>
#include <set>
#include <string>

// Reference: net/minecraft/world/level/chunk/storage/SerializableChunkData.java
//
// The on-disk form of a chunk, in vanilla Anvil layout. MC 26.3 writes every
// chunk it unloads this way — a proto chunk at any status as well as a full
// one — and reads it back with everything it had, so a partly generated
// chunk survives both unloading and a server restart.
//
// This port writes PROTO chunks only. FULL chunks belong to the embedder,
// which converts and saves them in its own code; the library reads them back
// (as neighbours whose status satisfies every dependency) but never writes
// one. What a proto carries, as in MC:
//   blocks, biomes, heightmaps, status, inhabited time
//   structure starts and references ("structures")
//   pending block entities, generated entities, post-processing positions
// The generator schedules no ticks and makes no UpgradeData, blending or
// below-zero retrogen data, so those fields are never written; a writable
// world is always one this port generated. Plus one engine extension, "obeycraft:structure_spawn_areas" (the
// structure spawn areas recorded at FEATURES, see IChunk::StructureSpawnArea),
// and "obeycraft:finalize_spawn" on generated entities the engine still has
// to finalize. Vanilla ignores both keys.
//
// The layout follows the DataVersion being written: saves before 5006 name a
// block state's fields Name/Properties (26.3: id/properties), and saves before
// 5013 call TERRAIN "carvers" (MergeTerrainChunkStatusFix). Reads accept both.

namespace minecraft {

namespace world {
class ProtoChunk;
}

namespace world {
namespace level {
namespace chunk {
namespace storage {

using ChunkStatusPtr = ::minecraft::world::chunk::status::ChunkStatus;
using ProtoChunk = ::minecraft::world::ProtoChunk;

class SerializableChunkData {
public:
    struct SectionData {
        int y = 0;
        std::vector<BlockState*> blockPalette;      // MC palette order
        std::vector<int32_t> blockIndices;          // 4096, empty = no block_states
        std::vector<std::string> biomePalette;
        std::vector<int32_t> biomeIndices;          // 64, empty = no biomes

        // A copyOf snapshot instead (MC: LevelChunkSection.copy()): the
        // container's own palette and packed ids, copied as they are, and
        // repacked into MC's palette order by write() on the I/O thread.
        bool snapshot = false;
        int snapshotBits = 0;                       // container bits per entry
        std::vector<BlockState*> snapshotPalette;   // container palette (bits <= 8)
        std::vector<int64_t> snapshotData;          // container storage
        std::vector<BlockState*> snapshotCells;     // global palette: the 4096 states
        std::array<const biome::Biome*, 64> snapshotBiomes{};
    };

    /**
     * Parse a saved chunk. Returns nullptr when the tag has no Status, which
     * MC treats as "no chunk here" (SerializableChunkData.parse). Throws
     * std::runtime_error on a structurally broken tag.
     * Reference: SerializableChunkData.parse(LevelHeightAccessor, ..., CompoundTag)
     */
    static std::unique_ptr<SerializableChunkData> parse(
        int minY, int height, const nbt::CompoundTag& chunkData, BlockState* airBlock);

    /**
     * Snapshot a proto chunk. Must run while nothing writes the chunk. As in
     * MC this only copies (section palettes and packed ids, heightmaps,
     * block-entity text, entities); the packing into the on-disk form is
     * write()'s, which the save runs on the I/O thread. The snapshot owns
     * everything it holds, so the chunk may be freed right after.
     * Reference: SerializableChunkData.copyOf(ServerLevel, ChunkAccess)
     */
    static std::unique_ptr<SerializableChunkData> copyOf(ProtoChunk& chunk, int64_t gameTime);

    /**
     * Encode for the given DataVersion.
     * Reference: SerializableChunkData.write()
     */
    std::unique_ptr<nbt::CompoundTag> write(int dataVersion) const;

    /**
     * Build the proto chunk (any saved status, FULL included).
     * Reference: SerializableChunkData.read(ServerLevel, PoiManager, RegionStorageInfo, ChunkPos)
     */
    std::unique_ptr<ProtoChunk> read(
        const ChunkPos& expectedPos,
        int minY,
        int height,
        BlockState* airBlock,
        BlockState* defaultBlock,
        BlockRegistry* registry) const;

    /**
     * Status of a saved chunk without a full parse; EMPTY for a missing tag.
     * Reference: SerializableChunkData.getChunkStatusFromTag(CompoundTag)
     */
    static const ChunkStatusPtr* getChunkStatusFromTag(const nbt::CompoundTag* tag);

    const ChunkPos& getChunkPos() const { return m_chunkPos; }
    const ChunkStatusPtr& getChunkStatus() const { return *m_chunkStatus; }
    int64_t getLastUpdateTime() const { return m_lastUpdateTime; }
    int64_t getInhabitedTime() const { return m_inhabitedTime; }
    bool isLightCorrect() const { return m_lightCorrect; }

private:
    SerializableChunkData() = default;

    ChunkPos m_chunkPos;
    int m_minSectionY = 0;
    int64_t m_lastUpdateTime = 0;
    int64_t m_inhabitedTime = 0;
    const ChunkStatusPtr* m_chunkStatus = nullptr;
    bool m_lightCorrect = false;

    std::vector<SectionData> m_sections;
    std::map<levelgen::Heightmap::Types, std::vector<int64_t>> m_heightmaps;

    // Indexed by section index (min section = 0); empty sets are absent.
    std::vector<std::vector<int16_t>> m_postProcessing;

    // Pending block-entity tags, full NBT including x/y/z.
    std::vector<std::unique_ptr<nbt::CompoundTag>> m_blockEntities;
    // copyOf keeps them as the chunk holds them ((y,z,x) -> canonical text);
    // write() parses them on the I/O thread.
    std::vector<std::pair<std::tuple<int, int, int>, std::string>> m_blockEntityText;
    // Generated entities (proto chunks only).
    std::vector<::minecraft::world::IChunk::GeneratedEntity> m_entities;
    std::vector<::minecraft::world::IChunk::StructureSpawnArea> m_spawnAreas;

    // "structures": {starts:{...}, References:{...}}
    std::unique_ptr<nbt::CompoundTag> m_structureData;
};

/**
 * Chunk data versions this port reads and writes.
 */
namespace ChunkSerializer {

// Minecraft 26.3-pre-2 (version.json world_version).
constexpr int32_t DATA_VERSION = 5018;
// BlockStateFieldNamesFix: Name/Properties -> id/properties.
constexpr int32_t BLOCK_STATE_FIELD_NAMES_VERSION = 5006;
// MergeTerrainChunkStatusFix: noise/surface/carvers -> terrain.
constexpr int32_t MERGED_TERRAIN_STATUS_VERSION = 5013;

} // namespace ChunkSerializer

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
