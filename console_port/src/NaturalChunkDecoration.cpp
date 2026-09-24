#include "NaturalChunkDecoration.h"
#include "NaturalDecorationAccess.h"
#include "GenerationDecorator.h"
#include "WorldGenLevel.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>

namespace console {
namespace {
std::unique_ptr<ChunkStorage> clone(const ChunkStorage& from){
    auto result=std::make_unique<ChunkStorage>();
    result->blocks=from.blocks;result->biomes=from.biomes;result->heightmap=from.heightmap;
    std::copy_n(from.metadata.data.data,from.metadata.data.length,result->metadata.data.data);
    return result;
}
}
std::unique_ptr<ChunkStorage> decorateNaturalColumn(
    std::int64_t seed,int worldChunks,BiomeScale scale,int chunkX,int chunkZ,
    const std::function<const ChunkStorage&(int,int)>& source,
    const std::function<bool(ChunkStorage&,int,int)>& afterBiome,
    std::vector<DungeonTile>* generatedTiles){
    GenerationRegion region;
    for(int x=chunkX-2;x<=chunkX+2;++x)
        for(int z=chunkZ-2;z<=chunkZ+2;++z)
            region.insert(x,z,clone(source(x,z)));
    NaturalDecorationAccess access(region);Level level(seed,worldChunks,scale);level.setBlockAccess(access);
    std::vector<DungeonTile> allTiles;
    for(int x=chunkX-1;x<=chunkX;++x)
        for(int z=chunkZ-1;z<=chunkZ;++z)
            if(!decorateGeneratedChunk(level,x,z,nullptr,[&](int sx,int sz){
                if(afterBiome){
                    auto& current=*region.chunks.at({sx,sz});
                    if(afterBiome(current,sx,sz))current.recalculateHeightmap();
                }
            },&allTiles))throw std::logic_error("Natural decoration neighborhood is incomplete");
    auto result=std::move(region.chunks.at({chunkX,chunkZ}));
    result->recalculateHeightmap();result->unsaved=true;
    if(generatedTiles){
        std::map<std::tuple<int,int,int>,DungeonTile> retained;
        for(auto& tile:allTiles){
            if(Mth::intFloorDiv(tile.x,16)!=chunkX || Mth::intFloorDiv(tile.z,16)!=chunkZ)continue;
            const int id=result->blocks[((tile.x&15)*16+(tile.z&15))*256+tile.y];
            if(id!=(tile.spawner?52:54))continue;
            retained[{tile.x,tile.y,tile.z}]=std::move(tile);
        }
        for(auto& [key,tile]:retained)generatedTiles->push_back(std::move(tile));
    }
    return result;
}
}
