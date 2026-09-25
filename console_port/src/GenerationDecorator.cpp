#include "GenerationDecorator.h"
#include "WorldGenLevel.h"
#include "OreFeature.h"
#include "SandFeature.h"
#include "ClayFeature.h"
#include "TreeFeature.h"
#include "BasicTree.h"
#include "BirchFeature.h"
#include "PineFeature.h"
#include "SpruceFeature.h"
#include "SwampTreeFeature.h"
#include "MegaTreeFeature.h"
#include "GroundBushFeature.h"
#include "HugeMushroomFeature.h"
#include "FlowerFeature.h"
#include "TallGrassFeature.h"
#include "DeadBushFeature.h"
#include "ReedsFeature.h"
#include "CactusFeature.h"
#include "WaterlilyFeature.h"
#include "PumpkinFeature.h"
#include "VinesFeature.h"
#include "LakeFeature.h"
#include "SpringFeature.h"
#include "DesertWellFeature.h"
#include <bit>
#include <cstdint>
#include <memory>

namespace console {
namespace {
// Defaults and per-biome constructor overrides from BiomeDecorator and the
// supplied Biome subclasses. Only the tree count is needed before foliage.
int treesPerChunk(int id){
    switch(id){
    case 1:case 2:case 16:case 17:return -999; // plains, deserts, beaches
    case 14:case 15:return -100;                // mushroom island
    case 4:case 5:case 18:case 19:return 10;    // forest and taiga
    case 6:return 2;                            // swamp
    case 21:case 22:return 50;                  // jungle
    default:return 0;
    }
}
struct FoliageCounts {
    int flowers=2,grass=1,deadBush=0,mushrooms=0,reeds=0,cactus=0;
    int waterlilies=0,hugeMushrooms=0;
};
FoliageCounts foliageFor(int id){
    FoliageCounts counts;
    switch(id){
    case 1:counts.flowers=4;counts.grass=10;break; // PlainsBiome
    case 2:case 17:counts.deadBush=2;counts.reeds=50;counts.cactus=10;break;
    case 4:case 18:counts.grass=2;break;
    case 6:counts.flowers=-999;counts.deadBush=1;counts.mushrooms=8;
           counts.reeds=10;counts.waterlilies=4;break;
    case 14:case 15:counts.flowers=-100;counts.grass=-100;
           counts.mushrooms=1;counts.hugeMushrooms=1;break;
    case 21:case 22:counts.flowers=4;counts.grass=25;break;
    default:break;
    }
    return counts;
}
std::unique_ptr<Feature> treeFor(int biome,Random& random){
    if(biome==5 || biome==19){
        if(random.nextInt(3)==0)return std::make_unique<PineFeature>();
        return std::make_unique<SpruceFeature>(false);
    }
    if(biome==6)return std::make_unique<SwampTreeFeature>();
    if(biome==4 || biome==18){
        if(random.nextInt(5)==0)return std::make_unique<BirchFeature>(false);
        if(random.nextInt(10)==0)return std::make_unique<BasicTree>(false);
        return std::make_unique<TreeFeature>(false);
    }
    if(biome==21 || biome==22){
        if(random.nextInt(10)==0)return std::make_unique<BasicTree>(false);
        if(random.nextInt(2)==0)return std::make_unique<GroundBushFeature>(3,0);
        if(random.nextInt(3)==0)return std::make_unique<MegaTreeFeature>(false,10+random.nextInt(20),3,3);
        return std::make_unique<TreeFeature>(false,4+random.nextInt(7),3,3,true);
    }
    if(random.nextInt(10)==0)return std::make_unique<BasicTree>(false);
    return std::make_unique<TreeFeature>(false);
}
int topSolidBlock(Level& level,int x,int z){
    // Level::getTopSolidBlock searches down from the highest loaded section.
    for(int y=Level::maxBuildHeight-1;y>0;--y){
        int tile=level.getTile(x,y,z);
        if(tile && tile!=18 && Tile::materialFor(tile)->blocksMotion())return y+1;
    }
    return -1;
}
void decorateOres(Level& level,Random& random,int xo,int zo,DecorationStats& stats){
    OreFeature dirt(3,32),gravel(13,32),coal(16,16),iron(15,8),gold(14,8);
    OreFeature redstone(73,7),diamond(56,7),lapis(21,6);
    auto depthSpan=[&](int count,Feature& feature,int y0,int y1){
        for(int i=0;i<count;++i){
            int x=xo+random.nextInt(16);
            int y=y0+random.nextInt(y1-y0);
            int z=zo+random.nextInt(16);
            feature.place(&level,&random,x,y,z);
            ++stats.oreAttempts;
        }
    };
    depthSpan(20,dirt,0,Level::genDepth);
    depthSpan(10,gravel,0,Level::genDepth);
    depthSpan(20,coal,0,Level::genDepth);
    depthSpan(20,iron,0,Level::genDepth/2);
    depthSpan(2,gold,0,Level::genDepth/4);
    depthSpan(8,redstone,0,Level::genDepth/8);
    depthSpan(1,diamond,0,Level::genDepth/8);
    for(int i=0;i<1;++i){
        int x=xo+random.nextInt(16);
        int y=random.nextInt(Level::genDepth/8)+random.nextInt(Level::genDepth/8);
        int z=zo+random.nextInt(16);
        lapis.place(&level,&random,x,y,z);
        ++stats.oreAttempts;
    }
}
void decorateFoliage(Level& level,Random& random,int biome,int xo,int zo,DecorationStats& stats){
    const auto counts=foliageFor(biome);
    HugeMushroomFeature hugeMushroom;
    FlowerFeature yellowFlower(37),roseFlower(38),brownMushroom(39),redMushroom(40);
    ReedsFeature reeds;CactusFeature cactus;WaterlilyFeature waterlily;
    for(int i=0;i<counts.hugeMushrooms;++i){
        int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        hugeMushroom.place(&level,&random,x,level.getHeightmap(x,z),z);
    }
    for(int i=0;i<counts.flowers;++i){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        yellowFlower.place(&level,&random,x,y,z);++stats.flowerAttempts;
        if(random.nextInt(4)==0){
            x=xo+random.nextInt(16)+8;y=random.nextInt(Level::genDepth);z=zo+random.nextInt(16)+8;
            roseFlower.place(&level,&random,x,y,z);++stats.flowerAttempts;
        }
    }
    for(int i=0;i<counts.grass;++i){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        const int type=(biome==21 || biome==22) && random.nextInt(4)==0?2:1;
        TallGrassFeature grass(31,type);grass.place(&level,&random,x,y,z);++stats.grassAttempts;
    }
    DeadBushFeature deadBush(32);
    for(int i=0;i<counts.deadBush;++i){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        deadBush.place(&level,&random,x,y,z);
    }
    for(int i=0;i<counts.waterlilies;++i){
        int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        int y=random.nextInt(Level::genDepth);
        while(y>0 && level.getTile(x,y-1,z)==0)--y;
        static_cast<Feature&>(waterlily).place(&level,&random,x,y,z);
    }
    for(int i=0;i<counts.mushrooms;++i){
        if(random.nextInt(4)==0){
            int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
            brownMushroom.place(&level,&random,x,level.getHeightmap(x,z),z);
        }
        if(random.nextInt(8)==0){
            int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
            redMushroom.place(&level,&random,x,y,z);
        }
    }
    if(random.nextInt(4)==0){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        brownMushroom.place(&level,&random,x,y,z);
    }
    if(random.nextInt(8)==0){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        redMushroom.place(&level,&random,x,y,z);
    }
    for(int i=0;i<counts.reeds+10;++i){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        reeds.place(&level,&random,x,y,z);++stats.reedAttempts;
    }
    if(random.nextInt(32)==0){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        PumpkinFeature pumpkin;pumpkin.place(&level,&random,x,y,z);
    }
    for(int i=0;i<counts.cactus;++i){
        int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        cactus.place(&level,&random,x,y,z);++stats.cactusAttempts;
    }
    // BiomeDecorator places cave springs after foliage. The original feature
    // checks its stone enclosure; its immediate liquid tick is not yet wired
    // to a full flowing-fluid simulation in the generation-only Level.
    SpringFeature waterSpring(8),lavaSpring(10);
    for(int i=0;i<50;++i){
        int x=xo+random.nextInt(16)+8;
        int y=random.nextInt(random.nextInt(Level::genDepth-8)+8);
        int z=zo+random.nextInt(16)+8;
        waterSpring.place(&level,&random,x,y,z);++stats.springAttempts;
    }
    for(int i=0;i<20;++i){
        int x=xo+random.nextInt(16)+8;
        int y=random.nextInt(random.nextInt(random.nextInt(Level::genDepth-16)+8)+8);
        int z=zo+random.nextInt(16)+8;
        lavaSpring.place(&level,&random,x,y,z);++stats.springAttempts;
    }
    // JungleBiome::decorate adds these after its BiomeDecorator pass. Spring
    // placement and RNG calls therefore precede vines.
    if(biome==21 || biome==22){
        VinesFeature vines;
        for(int i=0;i<50;++i){
            int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
            vines.place(&level,&random,x,Level::genDepth/2,z);
        }
    }
}
}

bool decorateGeneratedChunk(Level& level,int chunkX,int chunkZ,DecorationStats* output,
                            const std::function<void(int,int)>& afterBiome,
                            std::vector<DungeonTile>* generatedTiles){
    if(chunkX<-1000000 || chunkX>1000000 || chunkZ<-1000000 || chunkZ>1000000)return false;
    // Original features write into neighboring chunks (x/z offset 8..23),
    // sometimes farther for 32-block ore veins or jungle tree branches.
    for(int x=chunkX-1;x<=chunkX+2;++x)
        for(int z=chunkZ-1;z<=chunkZ+2;++z)
            if(!level.dimension || !level.hasChunkAt(x*16,0,z*16))return false;
    DecorationStats stats;
    int xo=chunkX*16,zo=chunkZ*16;
    int biome=level.getBiome(xo+16,zo+16)->id;
    // RandomLevelSource::postProcess uses a separate RNG from getChunk.
    Random random(level.getSeed());
    const std::int64_t xScale=random.nextLong()/2*2+1;
    const std::int64_t zScale=random.nextLong()/2*2+1;
    const auto offset=std::uint64_t(chunkX)*std::uint64_t(xScale)+std::uint64_t(chunkZ)*std::uint64_t(zScale);
    random.setSeed(std::bit_cast<std::int64_t>(offset^std::uint64_t(level.getSeed())));
    // Structure postprocessing still needs its native port. With no village
    // result yet, run the original lake calls before dungeons and biome work.
    if(random.nextInt(4)==0){
        const int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth),z=zo+random.nextInt(16)+8;
        LakeFeature(9).place(&level,&random,x,y,z);
    }
    if(random.nextInt(8)==0){
        const int x=xo+random.nextInt(16)+8;
        const int y=random.nextInt(random.nextInt(Level::genDepth-8)+8);
        const int z=zo+random.nextInt(16)+8;
        if(y<level.seaLevel || random.nextInt(10)==0)LakeFeature(11).place(&level,&random,x,y,z);
    }
    std::vector<DungeonTile> discardedTiles;
    auto& tileSink=generatedTiles?*generatedTiles:discardedTiles;
    for(int i=0;i<8;++i){
        const int x=xo+random.nextInt(16)+8,y=random.nextInt(Level::genDepth);
        const int z=zo+random.nextInt(16)+8;
        ++stats.dungeonAttempts;
        if(decorateMonsterRoom(level,random,x,y,z,tileSink))++stats.dungeonsPlaced;
    }
    decorateOres(level,random,xo,zo,stats);
    SandFeature sand(7,12);ClayFeature clay(4);
    for(int i=0;i<3;++i){int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        sand.place(&level,&random,x,topSolidBlock(level,x,z),z);++stats.shoreAttempts;}
    {int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        clay.place(&level,&random,x,topSolidBlock(level,x,z),z);++stats.shoreAttempts;}
    {int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        // The supplied BiomeDecorator calls sandFeature again here.
        sand.place(&level,&random,x,topSolidBlock(level,x,z),z);++stats.shoreAttempts;}
    int forests=treesPerChunk(biome);
    if(random.nextInt(10)==0)++forests;
    for(int i=0;i<forests;++i){
        int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        auto tree=treeFor(biome,random);
        tree->init(1,1,1);
        ++stats.treeAttempts;
        if(tree->place(&level,&random,x,level.getHeightmap(x,z),z))++stats.treesPlaced;
    }
    decorateFoliage(level,random,biome,xo,zo,stats);
    // DesertBiome and ExtremeHillsBiome override decorate after the shared
    // BiomeDecorator. Keep their feature calls and RNG order in that position.
    if((biome==2 || biome==17) && random.nextInt(1000)==0){
        const int x=xo+random.nextInt(16)+8,z=zo+random.nextInt(16)+8;
        DesertWellFeature well;
        if(well.place(&level,&random,x,level.getHeightmap(x,z)+1,z))++stats.desertWells;
    }
    if(biome==3 || biome==20){
        const int count=3+random.nextInt(6);
        for(int i=0;i<count;++i){
            const int x=xo+random.nextInt(16),y=random.nextInt(Level::genDepth/4-4)+4;
            const int z=zo+random.nextInt(16);
            if(level.getTile(x,y,z)==Tile::rock_Id && level.setTileNoUpdate(x,y,z,129))++stats.emeraldOre;
        }
    }
    // RandomLevelSource::postProcess places rule schematics on the source
    // chunk, then applies this shifted 16x16 snow/ice pass.
    if(afterBiome)afterBiome(chunkX,chunkZ);
    BiomeArray climate;
    level.getBiomeSource()->getBiomeBlock(climate,xo+8,zo+8,16,16,false);
    std::unique_ptr<Biome*[]> climateOwner(climate.data);
    for(int dx=0;dx<16;++dx)for(int dz=0;dz<16;++dz){
        // The source checks the same biome temperature in both predicates.
        // Querying the generation layer for all 256 biomes at once avoids
        // rebuilding its layer graph separately for every warm column.
        if(climate[dx+dz*16]->getTemperature()>0.15f)continue;
        const int x=xo+8+dx,z=zo+8+dz,y=level.getTopRainBlock(x,z);
        if(level.shouldFreezeIgnoreNeighbors(x,y-1,z)){
            if(level.setTileNoUpdate(x,y-1,z,Tile::ice_Id))++stats.frozenCells;
        }
        if(level.shouldSnow(x,y,z)){
            if(level.setTile(x,y,z,78))++stats.snowLayers;
        }
    }
    if(output)*output=stats;
    return true;
}
bool placeSaplingTree(Level& level,int kind,int height,Random& random,int x,int y,int z){
    std::unique_ptr<Feature> feature;
    switch(kind){
    case 0:feature=std::make_unique<TreeFeature>(false);break;
    case 1:feature=std::make_unique<BasicTree>(false);break;
    case 2:feature=std::make_unique<SpruceFeature>(false);break;
    case 3:feature=std::make_unique<BirchFeature>(false);break;
    // TreeTile::JUNGLE_TRUNK, LeafTile::JUNGLE_LEAF
    case 4:feature=std::make_unique<TreeFeature>(false,height,3,3,false);break;
    case 5:feature=std::make_unique<MegaTreeFeature>(false,height,3,3);break;
    // Mushroom::growTree
    case 6:feature=std::make_unique<HugeMushroomFeature>(0);break;
    case 7:feature=std::make_unique<HugeMushroomFeature>(1);break;
    default:throw std::invalid_argument("Unknown sapling tree");
    }
    return feature->place(&level,&random,x,y,z);
}
}
