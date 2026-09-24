#include "WorldGenLevel.h"
#include "GenerationRegion.h"
#include <iostream>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    console::GenerationRegion region;
    for(int x=-1;x<=1;++x)for(int z=-1;z<=1;++z)region.insert(x,z,std::make_unique<console::ChunkStorage>());
    Level level(1);level.setBlockAccess(region);
    level.setTileNoUpdate(0,63,0,12);
    require(Tile::cactus->canSurvive(&level,0,64,0),"Cactus rejected sand");
    level.setTileNoUpdate(1,64,0,20);
    require(!Tile::cactus->canSurvive(&level,0,64,0),"Cactus ignored solid glass material");
    level.setTileNoUpdate(1,64,0,0);level.setTileNoUpdate(1,63,0,8);
    require(Tile::reeds->canSurvive(&level,0,64,0),"Reeds rejected flowing water beside sand");
    level.setTileNoUpdate(1,63,0,79);
    require(!Tile::reeds->canSurvive(&level,0,64,0),"Ice incorrectly hydrated reeds");
    level.setTileNoUpdate(0,63,0,9);
    require(Tile::waterLily->mayPlace(&level,0,64,0),"Lily rejected still water");
    level.setTileAndDataNoUpdate(0,63,0,8,0);
    require(!Tile::waterLily->mayPlace(&level,0,64,0) && Tile::waterLily->canSurvive(&level,0,64,0),"Lily placement and survival rules were conflated");
    level.setTileAndDataNoUpdate(0,63,0,9,1);
    require(!Tile::waterLily->canSurvive(&level,0,64,0),"Lily survived non-source water");
    level.setTileNoUpdate(0,63,0,2);level.setTileNoUpdate(0,70,0,1);
    bool stale=false;try{Tile::tiles[37]->canSurvive(&level,0,64,0);}catch(const std::logic_error&){stale=true;}
    require(stale,"Uninitialized light was accepted by plant survival");
    region.acceptPreparedLight();
    require(!Tile::tiles[37]->canSurvive(&level,0,64,0),"Flower survived in darkness below a roof");
    require(Tile::tiles[40]->canSurvive(&level,0,64,0),"Mushroom rejected a dark solid surface");
    auto& chunk=*region.chunks.at({0,0});chunk.blockLight.set(0,64,0,8);
    require(Tile::tiles[37]->canSurvive(&level,0,64,0),"Flower ignored block light");
    chunk.blockLight.set(0,64,0,13);
    require(!Tile::tiles[40]->canSurvive(&level,0,64,0),"Mushroom survived at light level 13");
    level.setTileNoUpdate(0,63,0,110);
    require(Tile::tiles[40]->canSurvive(&level,0,64,0),"Mycelium did not bypass mushroom light requirement");
    level.setTileAndDataNoUpdate(0,63,0,44,0);
    require(!Tile::pumpkin->mayPlace(&level,0,64,0),"Pumpkin accepted bottom slab");
    level.setTileAndDataNoUpdate(0,63,0,44,8);
    require(Tile::pumpkin->mayPlace(&level,0,64,0),"Pumpkin rejected top slab");
    level.setTileNoUpdate(1,64,0,20);
    require(Tile::vine->mayPlace(&level,0,64,0,Facing::WEST),"Vine rejected cube-shaped glass");
    level.setTileNoUpdate(1,64,0,44);
    require(!Tile::vine->mayPlace(&level,0,64,0,Facing::WEST),"Vine attached to non-cube slab");
    std::cout<<"Passed plant soil, hydration, source-water, lighting, slab and attachment rules\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
