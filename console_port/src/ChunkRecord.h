#pragma once
#include "CompressedTileStorage.h"
#include "SparseDataStorage.h"
#include "SparseLightStorage.h"
#include "NbtIo.h"
#include "ConsoleSaveVersion.h"
namespace console {
// Serialization boundary for legacy NBT and version-8 chunks. Entity/tick NBT is retained
// intact; ScheduledTickCodec explicitly restores ticks when requested. This class
// does not instantiate entities or automatically modify a simulation queue.
class ChunkRecord {
    void writePrefix(DataOutputStream* dos);
    void writeCompressedBlockData(DataOutputStream* dos);
    void writeCompressedDataData(DataOutputStream* dos);
    void writeCompressedSkyLightData(DataOutputStream* dos);
    void writeCompressedBlockLightData(DataOutputStream* dos);
public:
    static constexpr unsigned allNeighbours=1022,postProcessed=1024;
    int x=0,z=0;
    std::int64_t lastUpdate=0;
    std::array<unsigned char,256> heightValues{},biomeValues{};
    byteArray heightmap{heightValues.data(),256},biomes{biomeValues.data(),256};
    std::uint16_t terrainPopulated=0;
    std::unique_ptr<CompressedTileStorage> lowerBlocks,upperBlocks;
    std::unique_ptr<SparseDataStorage> lowerData,upperData;
    std::unique_ptr<SparseLightStorage> lowerSkyLight,upperSkyLight,lowerBlockLight,upperBlockLight;
    std::unique_ptr<CompoundTag> extra;
    // Unknown outer fields from the older NBT chunk format. Version-8 records
    // have no outer compound; keep this separate from their entity/tick payload.
    std::unique_ptr<CompoundTag> legacyRootExtra;
    ChunkRecord();
    ChunkRecord(const ChunkRecord&)=delete;
    ChunkRecord& operator=(const ChunkRecord&)=delete;
    // Moving would invalidate the borrowed heightmap/biome views.
    ChunkRecord(ChunkRecord&&)=delete;
    byteArray getBiomes(){return biomes;}
    void write(DataOutputStream* dos);
    static std::unique_ptr<ChunkRecord> read(DataInputStream* dis);
    void writeLegacy(DataOutputStream* dos);
    static std::unique_ptr<ChunkRecord> readLegacy(DataInputStream* dis);
};
}
