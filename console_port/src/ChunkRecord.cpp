#include "ChunkRecord.h"
namespace console {
ChunkRecord::ChunkRecord(){
    CompressedTileStorage::staticCtor();SparseDataStorage::staticCtor();SparseLightStorage::staticCtor();
    lowerBlocks=std::make_unique<CompressedTileStorage>(true);upperBlocks=std::make_unique<CompressedTileStorage>(true);
    lowerData=std::make_unique<SparseDataStorage>(true);upperData=std::make_unique<SparseDataStorage>(true);
    lowerSkyLight=std::make_unique<SparseLightStorage>(true,true);upperSkyLight=std::make_unique<SparseLightStorage>(true,true);
    lowerBlockLight=std::make_unique<SparseLightStorage>(false,true);upperBlockLight=std::make_unique<SparseLightStorage>(false,true);
    extra=std::make_unique<CompoundTag>();
}
void ChunkRecord::write(DataOutputStream* dos){
    if(!dos || !extra || !lowerBlocks || !upperBlocks || !lowerData || !upperData || !lowerSkyLight || !upperSkyLight || !lowerBlockLight || !upperBlockLight)throw IoError("Incomplete chunk record");
    writePrefix(dos);NbtIo::write(extra.get(),dos);
}
std::unique_ptr<ChunkRecord> ChunkRecord::read(DataInputStream* dis){
    if(!dis)throw IoError("Null chunk input");
    const auto version=dis->readShort();
    if(version!=SAVE_FILE_VERSION_COMPRESSED_CHUNK_STORAGE)throw IoError("Unsupported chunk record version");
    auto result=std::make_unique<ChunkRecord>();
    result->x=dis->readInt();result->z=dis->readInt();
    result->lastUpdate=dis->readLong(); // The archived loader narrows this to int.
    result->lowerBlocks->read(dis);result->upperBlocks->read(dis);
    result->lowerData->read(dis);result->upperData->read(dis);
    result->lowerSkyLight->read(dis);result->upperSkyLight->read(dis);
    result->lowerBlockLight->read(dis);result->upperBlockLight->read(dis);
    if(!dis->readFully(result->heightmap))throw EndOfStream();
    result->terrainPopulated=static_cast<std::uint16_t>(dis->readShort());
    // Original migration for saves predating the post-post-processing flag.
    if((result->terrainPopulated&allNeighbours)==allNeighbours)result->terrainPopulated|=postProcessed;
    if(!dis->readFully(result->biomes))throw EndOfStream();
    result->extra.reset(NbtIo::read(dis));if(!result->extra)throw IoError("Chunk entity data must be an NBT compound");
    return result;
}
}
