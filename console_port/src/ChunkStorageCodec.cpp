#include "ChunkStorageCodec.h"
#include "net.minecraft.world.level.tile.h"
namespace console {
namespace {
void checkLayer(const DataLayer& layer){
    if(!layer.data.data || layer.data.length!=32768)throw IoError("Invalid flat chunk nibble storage");
}
void checkRecord(const ChunkRecord& record){
    if(!record.extra || !record.lowerBlocks || !record.upperBlocks || !record.lowerData || !record.upperData || !record.lowerSkyLight || !record.upperSkyLight || !record.lowerBlockLight || !record.upperBlockLight)throw IoError("Incomplete chunk storage record");
}
std::array<std::uint16_t,256> heights(const std::array<std::uint8_t,65536>& blocks){
    std::array<std::uint16_t,256> result{};
    for(unsigned column=0;column<256;++column)for(unsigned y=0;y<256;++y){
        unsigned id=blocks[column*256+y];
        if(id==255 || !Tile::supported[id])throw IoError("Block "+std::to_string(id)+" is not registered in the generation storage adapter");
        if(Tile::lightBlockFor(id))result[column]=y+1;
    }
    return result;
}
// Each section exports original x/z/y columns. Flat columns span all 256 y
// values, so a memcpy of the whole chunk would interleave the two sections.
template<unsigned columnBytes>
void split(const unsigned char* flat,unsigned char* sections){
    for(unsigned column=0;column<256;++column)for(unsigned section=0;section<2;++section)
        std::copy_n(flat+column*columnBytes*2+section*columnBytes,columnBytes,sections+section*256*columnBytes+column*columnBytes);
}
template<unsigned columnBytes>
void join(const unsigned char* sections,unsigned char* flat){
    for(unsigned column=0;column<256;++column)for(unsigned section=0;section<2;++section)
        std::copy_n(sections+section*256*columnBytes+column*columnBytes,columnBytes,flat+column*columnBytes*2+section*columnBytes);
}
}
std::unique_ptr<ChunkRecord> ChunkStorageCodec::capture(const ChunkStorage& storage,const ChunkRecord& context){
    checkRecord(context);checkLayer(storage.metadata);checkLayer(storage.skyLight);checkLayer(storage.blockLight);
    const auto actualHeights=heights(storage.blocks);
    if(actualHeights!=storage.heightmap)throw IoError("Recalculate the host heightmap before capturing the chunk");
    auto result=std::make_unique<ChunkRecord>();result->x=context.x;result->z=context.z;result->lastUpdate=context.lastUpdate;result->terrainPopulated=context.terrainPopulated;
    result->extra.reset(static_cast<CompoundTag*>(context.extra->copy()));
    if(context.legacyRootExtra)result->legacyRootExtra.reset(static_cast<CompoundTag*>(context.legacyRootExtra->copy()));
    result->biomeValues=storage.biomes;
    for(unsigned x=0;x<16;++x)for(unsigned z=0;z<16;++z)
        result->heightValues[z*16+x]=static_cast<unsigned char>(actualHeights[x*16+z]);
    std::vector<unsigned char> blocks(65536),nibbles(32768);
    byteArray blockView(blocks.data(),blocks.size()),nibbleView(nibbles.data(),nibbles.size());
    split<128>(storage.blocks.data(),blocks.data());result->lowerBlocks->setData(blockView,0);result->upperBlocks->setData(blockView,32768);
    split<64>(storage.metadata.data.data,nibbles.data());result->lowerData->setData(nibbleView,0);result->upperData->setData(nibbleView,16384);
    split<64>(storage.skyLight.data.data,nibbles.data());result->lowerSkyLight->setData(nibbleView,0);result->upperSkyLight->setData(nibbleView,16384);
    split<64>(storage.blockLight.data.data,nibbles.data());result->lowerBlockLight->setData(nibbleView,0);result->upperBlockLight->setData(nibbleView,16384);
    return result;
}
RestoredChunkStorage ChunkStorageCodec::restore(const ChunkRecord& record){
    checkRecord(record);RestoredChunkStorage result{std::make_unique<ChunkStorage>(),false};auto& storage=*result.storage;
    std::vector<unsigned char> blocks(65536),nibbles(32768);
    byteArray blockView(blocks.data(),blocks.size()),nibbleView(nibbles.data(),nibbles.size());
    record.lowerBlocks->getData(blockView,0);record.upperBlocks->getData(blockView,32768);join<128>(blocks.data(),storage.blocks.data());
    storage.heightmap=heights(storage.blocks);storage.biomes=record.biomeValues;storage.minHeight=256;
    for(unsigned x=0;x<16;++x)for(unsigned z=0;z<16;++z){
        unsigned column=x*16+z;storage.minHeight=std::min(storage.minHeight,static_cast<int>(storage.heightmap[column]));
        if(storage.heightmap[column]!=record.heightValues[z*16+x])result.needsLightRebuild=true;
    }
    record.lowerData->getData(nibbleView,0);record.upperData->getData(nibbleView,16384);join<64>(nibbles.data(),storage.metadata.data.data);
    record.lowerSkyLight->getData(nibbleView,0);record.upperSkyLight->getData(nibbleView,16384);join<64>(nibbles.data(),storage.skyLight.data.data);
    record.lowerBlockLight->getData(nibbleView,0);record.upperBlockLight->getData(nibbleView,16384);join<64>(nibbles.data(),storage.blockLight.data.data);
    storage.emissiveAdded=true; // Original LevelChunk load initialization.
    if(result.needsLightRebuild){storage.hasGapsToCheck=true;storage.columnFlags.fill(0x11);}
    return result;
}
}
