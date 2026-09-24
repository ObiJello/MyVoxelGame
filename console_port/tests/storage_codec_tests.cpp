#include "ChunkStorageCodec.h"
#include "PS3WorldStorage.h"
#include "DimensionChunkGenerator.h"
#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include <iostream>
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
static void same(const console::ChunkStorage& a,const console::ChunkStorage& b){
    require(a.blocks==b.blocks && a.heightmap==b.heightmap && a.biomes==b.biomes,"Flat blocks, heights and biomes round trip");
    for(auto pair:{std::pair{&a.metadata,&b.metadata},std::pair{&a.skyLight,&b.skyLight},std::pair{&a.blockLight,&b.blockLight}})
        require(std::memcmp(pair.first->data.data,pair.second->data.data,32768)==0,"Every packed nibble round trips");
}
static void upperLightRoundTrip(bool ceiling){
    using namespace console;
    GenerationRegion region;
    for(int x=-2;x<=2;++x)for(int z=-2;z<=2;++z)
        region.insert(x,z,std::make_unique<ChunkStorage>());
    Level level(42);level.dimension->hasCeiling=ceiling;level.setBlockAccess(region);
    auto& chunk=*region.chunks.at({0,0});
    chunk.set(7,127,9,89);chunk.set(15,128,9,89);
    chunk.set(7,200,9,89);chunk.set(8,200,9,1);chunk.set(15,255,9,89);
    auto check=[&]{
        for(auto [x,y]:{std::pair{7,127},std::pair{15,128},std::pair{7,200},std::pair{15,255}})
            require(level.getBrightness(LightLayer::Block,x,y,9)==15,"Initial emission scan covers both sections and the build ceiling");
        require(level.getBrightness(LightLayer::Block,7,126,9)==14 && level.getBrightness(LightLayer::Block,15,129,9)==14,"Section-boundary light falls off into adjacent Y values");
        require(level.getBrightness(LightLayer::Block,16,128,9)==14 && level.getBrightness(LightLayer::Block,16,255,9)==14,"Upper-section emission crosses chunk boundaries");
        require(level.getBrightness(LightLayer::Block,8,200,9)==0 && level.getBrightness(LightLayer::Block,9,200,9)==11,"Opaque obstruction blocks direct light while indirect light travels around it");
        if(ceiling)require(level.getBrightness(LightLayer::Sky,7,201,9)==0,"Ceiling dimensions do not acquire skylight during block-light rebuild");
    };
    region.initializeLight(level);check();
    ChunkRecord context;auto snapshot=ChunkStorageCodec::capture(*region.chunks.at({0,0}),context);
    PS3WorldStorage world;world.putChunk(ceiling?-1:0,*snapshot);
    auto loaded=PS3WorldStorage::read(world.serialize());
    auto record=loaded->chunk(ceiling?-1:0,0,0);
    auto restored=ChunkStorageCodec::restore(*record);
    require(restored.needsLightRebuild,"Original wrapped ceiling height requests a host light rebuild");
    same(*region.chunks.at({0,0}),*restored.storage);
    restored.storage->blockLight.setAll(0);
    region.insert(0,0,std::move(restored.storage));region.initializeLight(level);check();
    level.setTileNoUpdate(7,200,9,0);
    require(level.getBrightness(LightLayer::Block,7,200,9)==0 && level.getBrightness(LightLayer::Block,9,200,9)==0,"Removing a restored upper-section source clears its propagated light");
}
int main(){try{
    using namespace console;
    ChunkStorage input;constexpr unsigned char ids[]={1,3,9,18,89,121,115};
    for(int x=0;x<16;++x)for(int z=0;z<16;++z){int h=(x*17+z*7)%254+1;input.biomes[z*16+x]=(x+z*3)%23;
        for(int y=0;y<256;++y){if(y<h)input.blocks[(x*16+z)*256+y]=ids[(x+y+z)%7];input.metadata.set(x,y,z,(x+3*y+z)%16);input.skyLight.set(x,y,z,(x*3+y+5*z)%16);input.blockLight.set(x,y,z,(x+y*7+z)%16);}
    }
    input.recalculateHeightmap();ChunkRecord context;context.x=-17;context.z=23;context.lastUpdate=1ll<<47;context.terrainPopulated=42;context.extra->putString(L"Unknown",L"keep");context.legacyRootExtra=std::make_unique<CompoundTag>();context.legacyRootExtra->putInt(L"OuterUnknown",123);
    auto record=ChunkStorageCodec::capture(input,context);
    require(record->x==-17 && record->z==23 && record->lastUpdate==(1ll<<47) && record->terrainPopulated==42 && record->extra->getString(L"Unknown")==L"keep" && record->legacyRootExtra->getInt(L"OuterUnknown")==123,"Snapshot retains record metadata and opaque NBT");
    for(int x=0;x<16;++x)for(int z=0;z<16;++z){require(record->heightValues[z*16+x]==input.heightmap[x*16+z],"Height map is transposed into original wire order");for(int y=0;y<256;++y){auto& blocks=y<128?record->lowerBlocks:record->upperBlocks;auto& data=y<128?record->lowerData:record->upperData;auto& sky=y<128?record->lowerSkyLight:record->upperSkyLight;auto& light=y<128?record->lowerBlockLight:record->upperBlockLight;require(blocks->get(x,y%128,z)==input.blocks[(x*16+z)*256+y] && data->get(x,y%128,z)==input.metadata.get(x,y,z) && sky->get(x,y%128,z)==input.skyLight.get(x,y,z) && light->get(x,y%128,z)==input.blockLight.get(x,y,z),"All section coordinates and nibbles match the flat input");}}
    record->extra->putString(L"Unknown",L"changed");require(context.extra->getString(L"Unknown")==L"keep","Record NBT is copied without ownership aliases");
    auto restored=ChunkStorageCodec::restore(*record);same(input,*restored.storage);require(!restored.needsLightRebuild && !restored.storage->unsaved,"Consistent saved heights do not require repair");
    record->heightValues[0]^=1;auto corrected=ChunkStorageCodec::restore(*record);require(corrected.needsLightRebuild && corrected.storage->hasGapsToCheck && corrected.storage->columnFlags[0]==0x11,"Stale saved heightmap requests lighting repair");same(input,*corrected.storage);
    input.blocks[(2*16+3)*256+255]=1;input.recalculateHeightmap();record=ChunkStorageCodec::capture(input,context);require(record->heightValues[3*16+2]==0,"Original byte height encoding wraps 256");corrected=ChunkStorageCodec::restore(*record);require(corrected.storage->heightmap[2*16+3]==256 && corrected.needsLightRebuild,"Host recovers full height and explicitly requests relighting");
    input.heightmap[0]^=1;rejects([&]{ChunkStorageCodec::capture(input,context);});input.recalculateHeightmap();
    auto oldLength=input.metadata.data.length;input.metadata.data.length=1;rejects([&]{ChunkStorageCodec::capture(input,context);});input.metadata.data.length=oldLength;
    record->lowerBlocks->set(0,0,0,200);rejects([&]{ChunkStorageCodec::restore(*record);});require(record->lowerBlocks->get(0,0,0)==200,"Unsupported host block leaves the raw record intact");
    // Real terrain from all three dimension generators traverses storage ->
    // legacy or current chunk record -> region -> archive and back.
    ChunkGenerator overworld(8675309);DimensionChunkGenerator nether(TerrainDimension::Nether,8675309),end(TerrainDimension::End,8675309);
    for(int dimension:{-1,0,1})for(bool legacy:{false,true}){
        auto generated=dimension==-1?nether.generate(-1,0):dimension==1?end.generate(-1,0):overworld.generate(-1,0);ChunkStorage source(generated);
        for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<256;++y){source.metadata.set(x,y,z,(x+y+z)%16);source.skyLight.set(x,y,z,dimension==-1?0:(y>=source.heightmap[x*16+z]?15:0));source.blockLight.set(x,y,z,(x*3+y+z)%16);}
        ChunkRecord base;base.x=-1;base.z=0;base.extra->putInt(L"OpaqueEntityMarker",7);auto snap=ChunkStorageCodec::capture(source,base);PS3WorldStorage fresh;auto archiveBytes=fresh.serialize();if(legacy)archiveBytes[9]=2;auto world=PS3WorldStorage::read(archiveBytes);world->putChunk(dimension,*snap);auto loadedWorld=PS3WorldStorage::read(world->serialize());auto loadedRecord=loadedWorld->chunk(dimension,-1,0);auto loaded=ChunkStorageCodec::restore(*loadedRecord);same(source,*loaded.storage);require(loadedRecord->extra->getInt(L"OpaqueEntityMarker")==7,"Opaque entity fields survive actual terrain save path");
    }
    // Preserve light actually produced by the original propagation code, not
    // just patterns. A five-by-five halo is required by the generation adapter.
    GenerationRegion region;for(int x=-2;x<=2;++x)for(int z=-2;z<=2;++z){auto chunk=std::make_unique<ChunkStorage>();for(int i=0;i<256;++i)std::fill_n(chunk->blocks.data()+i*256,4,1);chunk->recalculateHeightmap();region.insert(x,z,std::move(chunk));}
    Level level(42);level.setBlockAccess(region);region.chunks.at({0,0})->set(7,20,9,89);region.initializeLight(level);auto& lit=*region.chunks.at({0,0});require(lit.blockLight.get(7,20,9)==15 && lit.blockLight.get(8,20,9)==14,"Original emission propagation prepared the fixture");auto litRecord=ChunkStorageCodec::capture(lit,context);same(lit,*ChunkStorageCodec::restore(*litRecord).storage);
    upperLightRoundTrip(false);upperLightRoundTrip(true);
    std::cout<<"Flat/compressed snapshots, full-height recovery, opaque fields, all-dimension save integration and upper-section relighting passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
