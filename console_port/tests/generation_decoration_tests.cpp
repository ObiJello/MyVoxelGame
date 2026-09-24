#include "GenerationDecorator.h"
#include "GenerationRegion.h"
#include "NaturalDecorationAccess.h"
#include "ChunkGenerator.h"
#include "WorldGenLevel.h"
#include "Mth.h"
#include "IntCache.h"
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::uint64_t digest(const console::GenerationRegion& region){
    std::uint64_t hash=14695981039346656037ull;
    for(const auto& [coord,chunk]:region.chunks){
        for(auto value:chunk->blocks)hash=(hash^value)*1099511628211ull;
        for(unsigned i=0;i<chunk->metadata.data.length;++i)
            hash=(hash^chunk->metadata.data[i])*1099511628211ull;
    }
    return hash;
}
void populate(console::GenerationRegion& region,bool complete){
    for(int cx=complete?-2:-1;cx<=(complete?2:1);++cx)
        for(int cz=complete?-2:-1;cz<=2;++cz){
            auto chunk=std::make_unique<console::ChunkStorage>();
            for(int x=0;x<16;++x)for(int z=0;z<16;++z){
                int column=(x*16+z)*256;
                for(int y=0;y<60;++y)chunk->blocks[column+y]=1;
                for(int y=60;y<63;++y)chunk->blocks[column+y]=3;
                chunk->blocks[column+63]=2;
            }
            chunk->recalculateHeightmap();region.insert(cx,cz,std::move(chunk));
        }
}
std::uint64_t run(int biome,console::DecorationStats& stats,int& logs){
    console::GenerationRegion region;populate(region,true);
    console::NaturalDecorationAccess access(region);
    Level level(1227750481513469519ll);level.getBiomeSource()->setFixedBiome(biome);level.setBlockAccess(access);
    require(console::decorateGeneratedChunk(level,0,0,&stats),"Prepared decoration region rejected");
    logs=0;
    for(const auto& [coord,chunk]:region.chunks)
        for(auto tile:chunk->blocks)logs+=tile==17;
    return digest(region);
}
std::uint64_t runOnGeneratedTerrain(int centerX,int centerZ,console::DecorationStats& stats){
    constexpr std::int64_t seed=1227750481513469519ll;
    console::ChunkGenerator generator(seed);
    console::GenerationRegion region;
    for(int cx=centerX-2;cx<=centerX+2;++cx)for(int cz=centerZ-2;cz<=centerZ+2;++cz)
        region.insert(cx,cz,std::make_unique<console::ChunkStorage>(generator.generate(cx,cz)));
    auto before=digest(region);
    console::NaturalDecorationAccess access(region);
    Level level(seed);level.getBiomeSource()->setFixedBiome(4);level.setBlockAccess(access);
    require(console::decorateGeneratedChunk(level,centerX,centerZ,&stats),"Real generated terrain rejected by decorator");
    auto after=digest(region);
    require(after!=before,"Decoration did not change real generated terrain");
    return after;
}
}
int main(){
    Mth::init();IntCache::CreateNewThreadStorage();
    console::DecorationStats first,repeat,plains;
    int logs=0,repeatLogs=0,plainsLogs=0;
    auto forestHash=run(4,first,logs);
    require(first.oreAttempts==82 && first.shoreAttempts==5,"Original pre-tree feature counts changed");
    require(first.dungeonAttempts==8,"Original dungeon attempt count changed");
    require(first.treeAttempts>=10 && first.treeAttempts<=11,"Forest tree count changed");
    require(first.treesPlaced>0 && logs>0,"Forest decoration created no trees");
    require(first.flowerAttempts>=2 && first.flowerAttempts<=4 && first.grassAttempts==2 &&
            first.reedAttempts==10 && first.springAttempts==70,"Forest foliage and spring stage counts changed");
    require(run(4,repeat,repeatLogs)==forestHash && repeatLogs==logs,
            "Decoration depends on generation order or process state");
    run(1,plains,plainsLogs);
    require(plains.treeAttempts==0 && plains.flowerAttempts>=4 && plains.flowerAttempts<=8 &&
            plains.grassAttempts==10,"Original plains tree and foliage counts changed");
    console::GenerationRegion cold;populate(cold,true);
    require(cold.setTileAndData(12,63,12,9,0),"Prepare still-water cold biome fixture");
    console::NaturalDecorationAccess coldAccess(cold);
    Level coldLevel(1227750481513469519ll);coldLevel.getBiomeSource()->setFixedBiome(12);coldLevel.setBlockAccess(coldAccess);
    require(coldLevel.getTopRainBlock(12,12)==64 && coldLevel.shouldFreezeIgnoreNeighbors(12,63,12),
            "Cold still-water surface is eligible to freeze");
    require(coldLevel.shouldSnow(18,64,18),"Cold grass surface is eligible for snow");
    console::DecorationStats coldStats;
    require(console::decorateGeneratedChunk(coldLevel,0,0,&coldStats),"Cold biome decoration region rejected");
    require(coldStats.frozenCells>0 && coldStats.snowLayers>0,
            "Source post-process did not freeze water and add snow");
    console::GenerationRegion ordered;populate(ordered,true);
    console::NaturalDecorationAccess orderedAccess(ordered);
    Level orderedLevel(1227750481513469519ll);orderedLevel.getBiomeSource()->setFixedBiome(12);
    orderedLevel.setBlockAccess(orderedAccess);
    int hookCalls=0;
    require(console::decorateGeneratedChunk(orderedLevel,0,0,nullptr,[&](int cx,int cz){
        require(cx==0 && cz==0,"Rule callback received the wrong source chunk");
        ++hookCalls;
        orderedLevel.setTileNoUpdate(12,63,12,9);
    }),"Source-order fixture rejected");
    require(hookCalls==1 && ordered.getTile(12,63,12)==79,
            "Cold post-process must freeze schematic water placed after biome decoration");
    Level climateLevel(1227750481513469519ll);
    BiomeArray climate;
    climateLevel.getBiomeSource()->getBiomeBlock(climate,-48,112,16,16,false);
    for(int dx=0;dx<16;++dx)for(int dz=0;dz<16;++dz)
        require(climate[dx+dz*16]->id==climateLevel.getBiome(-48+dx,112+dz)->id,
                "Batched snow climate differs from source point lookup");
    delete[] climate.data;
    console::DecorationStats hills;
    int hillLogs=0;
    run(3,hills,hillLogs);
    require(hills.emeraldOre>=3 && hills.emeraldOre<=8,
            "Extreme Hills source override did not place its emerald veins");
    console::GenerationRegion dungeon;populate(dungeon,true);
    Random dimensions(71337),roomRandom(71337);
    const int xr=dimensions.nextInt(2)+2;dimensions.nextInt(2);
    require(dungeon.setTileAndData(12+xr+1,30,12,0,0) &&
            dungeon.setTileAndData(12+xr+1,31,12,0,0),"Prepare a two-high cave entrance");
    console::NaturalDecorationAccess dungeonAccess(dungeon);
    Level dungeonLevel(71337);dungeonLevel.setBlockAccess(dungeonAccess);
    std::vector<console::DungeonTile> roomTiles;
    require(console::decorateMonsterRoom(dungeonLevel,roomRandom,12,30,12,roomTiles),
            "Source dungeon room did not accept one cave opening");
    require(dungeon.getTile(12,30,12)==52 &&
            (dungeon.getTile(12,29,12)==48 || dungeon.getTile(12,29,12)==4),
            "Dungeon spawner and source floor were not placed");
    int spawners=0;
    for(const auto& tile:roomTiles)if(tile.spawner){
        ++spawners;
        require(tile.x==12 && tile.y==30 && tile.z==12 && !tile.entityId.empty(),
                "Dungeon spawner lost its source coordinates or mob type");
    }
    require(spawners==1,"Dungeon must create one spawner tile record");
    console::GenerationRegion incomplete;populate(incomplete,false);
    Level level(7);level.getBiomeSource()->setFixedBiome(4);level.setBlockAccess(incomplete);
    auto before=digest(incomplete);
    require(!console::decorateGeneratedChunk(level,0,0),"Incomplete neighbor region accepted");
    require(digest(incomplete)==before,"Incomplete region decoration wrote blocks");
    console::DecorationStats generatedFirst,generatedRepeat;
    auto generatedHash=runOnGeneratedTerrain(-14,8,generatedFirst);
    require(runOnGeneratedTerrain(-14,8,generatedRepeat)==generatedHash &&
            generatedFirst.oreAttempts==generatedRepeat.oreAttempts &&
            generatedFirst.treeAttempts==generatedRepeat.treeAttempts,
            "Real terrain decoration is not deterministic");
    IntCache::ReleaseThreadStorage();
}
