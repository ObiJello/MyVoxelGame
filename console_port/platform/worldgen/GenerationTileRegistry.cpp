#include "net.minecraft.world.level.tile.h"
#include "WorldGenLevel.h"
#include <memory>
namespace {
struct GenerationLiquidTile final:Tile {
    using Tile::Tile;
    void tick(Level* level,int x,int y,int z,Random* random)override{
        level->tickGenerationLiquid(id,x,y,z,*random);
    }
};
}
Tile* Tile::tiles[256]{};
Tile *Tile::reeds=nullptr,*Tile::cactus=nullptr,*Tile::waterLily=nullptr,
     *Tile::pumpkin=nullptr,*Tile::vine=nullptr;
void initializeGenerationTiles(){
    static std::once_flag initialized;
    std::call_once(initialized,[]{
        static std::array<std::unique_ptr<Tile>,256> owners;
        for(int id=1;id<256;++id){
            if(!Tile::supported[id])continue;
            std::unique_ptr<Tile> tile;
            switch(id){
            case 8:case 10:tile=std::make_unique<GenerationLiquidTile>(id);break;
            case 6:case 31:case 37:case 38:tile=std::make_unique<Bush>(id);break;
            case 32:tile=std::make_unique<DeadBushTile>(id);break;
            case 39:case 40:tile=std::make_unique<Mushroom>(id);break;
            case 44:tile=std::make_unique<HalfSlabTile>(id);break;
            case 81:tile=std::make_unique<CactusTile>(id);break;
            case 83:tile=std::make_unique<ReedTile>(id);break;
            case 86:tile=std::make_unique<PumpkinTile>(id);break;
            case 106:tile=std::make_unique<VineTile>(id);break;
            case 111:tile=std::make_unique<WaterlilyTile>(id);break;
            default:tile=std::make_unique<Tile>(id);break;
            }
            Tile::tiles[id]=tile.get();owners[id]=std::move(tile);
        }
        Tile::reeds=Tile::tiles[83];Tile::cactus=Tile::tiles[81];Tile::waterLily=Tile::tiles[111];
        Tile::pumpkin=Tile::tiles[86];Tile::vine=Tile::tiles[106];
    });
}
