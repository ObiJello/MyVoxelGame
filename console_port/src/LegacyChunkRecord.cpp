#include "ChunkRecord.h"
namespace console {
namespace {
byteArray requiredArray(CompoundTag* tag,const wchar_t* name,unsigned size){
    auto* value=dynamic_cast<ByteArrayTag*>(tag->get(name));
    if(!value || value->data.length!=size || !value->data.data)throw IoError("Missing or invalid legacy chunk array");
    return value->data;
}
constexpr const wchar_t* fields[]={L"xPos",L"zPos",L"LastUpdate",L"Blocks",L"Data",L"SkyLight",L"BlockLight",L"HeightMap",L"TerrainPopulated",L"TerrainPopulatedFlags",L"Biomes"};
}
std::unique_ptr<ChunkRecord> ChunkRecord::readLegacy(DataInputStream* dis){
    if(!dis)throw IoError("Null legacy chunk input");
    std::unique_ptr<CompoundTag> root(NbtIo::read(dis));
    if(!root || !dynamic_cast<CompoundTag*>(root->get(L"Level")))throw IoError("Legacy chunk requires a Level compound");
    auto* level=root->getCompound(L"Level");
    auto* blocks=dynamic_cast<ByteArrayTag*>(level->get(L"Blocks"));
    if(!blocks || (blocks->data.length!=32768 && blocks->data.length!=65536))throw IoError("Unsupported legacy chunk height");
    const unsigned size=blocks->data.length;
    auto blockData=requiredArray(level,L"Blocks",size),data=requiredArray(level,L"Data",size/2);
    auto sky=requiredArray(level,L"SkyLight",size/2),light=requiredArray(level,L"BlockLight",size/2);
    auto heights=requiredArray(level,L"HeightMap",256);
    // Validate required coordinates rather than allowing malformed chunks to be
    // silently loaded into slot (0,0). Arrays remain borrowed from their tags.
    if(!dynamic_cast<IntTag*>(level->get(L"xPos")) || !dynamic_cast<IntTag*>(level->get(L"zPos")))throw IoError("Missing legacy chunk coordinates");
    auto result=std::make_unique<ChunkRecord>();result->x=level->getInt(L"xPos");result->z=level->getInt(L"zPos");result->lastUpdate=level->getLong(L"LastUpdate");
    result->lowerBlocks->setData(blockData,0);result->lowerData->setData(data,0);
    result->lowerSkyLight->setData(sky,0);result->lowerBlockLight->setData(light,0);
    if(size==65536){
        result->upperBlocks->setData(blockData,32768);result->upperData->setData(data,16384);
        result->upperSkyLight->setData(sky,16384);result->upperBlockLight->setData(light,16384);
    }
    std::copy_n(heights.data,256,result->heightValues.begin());
    if(level->contains(L"TerrainPopulated")){
        result->terrainPopulated=level->getByte(L"TerrainPopulated");
        if(result->terrainPopulated>=1)result->terrainPopulated=allNeighbours|postProcessed;
    }else{
        result->terrainPopulated=static_cast<std::uint16_t>(level->getShort(L"TerrainPopulatedFlags"));
        if((result->terrainPopulated&allNeighbours)==allNeighbours)result->terrainPopulated|=postProcessed;
    }
    result->biomeValues.fill(255); // Original LevelChunk::init's unresolved biome sentinel.
    if(level->contains(L"Biomes")){auto biomes=requiredArray(level,L"Biomes",256);std::copy_n(biomes.data,256,result->biomeValues.begin());}
    result->extra.reset(static_cast<CompoundTag*>(root->take(L"Level")));
    for(auto name:fields)result->extra->remove(name);
    result->legacyRootExtra=std::move(root);
    return result;
}
void ChunkRecord::writeLegacy(DataOutputStream* dos){
    if(!dos || !extra || !lowerBlocks || !upperBlocks || !lowerData || !upperData || !lowerSkyLight || !upperSkyLight || !lowerBlockLight || !upperBlockLight)throw IoError("Incomplete legacy chunk record");
    std::vector<unsigned char> blocks(65536),data(32768),sky(32768),light(32768);
    byteArray blockData(blocks.data(),blocks.size()),dataData(data.data(),data.size()),skyData(sky.data(),sky.size()),lightData(light.data(),light.size());
    lowerBlocks->getData(blockData,0);upperBlocks->getData(blockData,32768);
    lowerData->getData(dataData,0);upperData->getData(dataData,16384);
    lowerSkyLight->getData(skyData,0);upperSkyLight->getData(skyData,16384);
    lowerBlockLight->getData(lightData,0);upperBlockLight->getData(lightData,16384);
    auto level=std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(extra->copy()));
    // Original OldChunkStorage NBT field order and LevelChunk section ordering.
    // Saving a formerly 128-high chunk writes both current-height sections.
    for(auto name:fields)level->remove(name);
    level->putInt(L"xPos",x);level->putInt(L"zPos",z);level->putLong(L"LastUpdate",lastUpdate);
    level->putByteArray(L"Blocks",blockData);level->putByteArray(L"Data",dataData);
    level->putByteArray(L"SkyLight",skyData);level->putByteArray(L"BlockLight",lightData);
    level->putByteArray(L"HeightMap",heightmap);level->putShort(L"TerrainPopulatedFlags",static_cast<short>(terrainPopulated));level->putByteArray(L"Biomes",biomes);
    auto root=legacyRootExtra?std::unique_ptr<CompoundTag>(static_cast<CompoundTag*>(legacyRootExtra->copy())):std::make_unique<CompoundTag>();
    root->put(L"Level",level.get());level.release();NbtIo::write(root.get(),dos);
}
}
