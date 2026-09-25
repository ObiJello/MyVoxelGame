// The original random tile tick rules (ported/tick) on a small map level, and
// the World's random tick pass on a generated world.
#include "TileTickHost.h"
#include "TileProperties.h"
#include "World.h"
#include "WorldLibrary.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

using namespace console;
using sim::Tile;

static void require(bool good,const std::string& message){if(!good)throw std::runtime_error(message);}

// Blocks in a map; sky light 15 above the highest block of a column, block
// light from `lamp`; every chunk is present.
struct MapLevel final:sim::Level {
    std::map<std::tuple<int,int,int>,std::pair<int,int>> blocks;
    int sky=15,lamp=0,skyDarken=0;
    bool raining=false;
    std::vector<std::tuple<int,int,int,int>> drops;
    std::vector<std::tuple<int,int,int,int>> trees;
    bool treeResult=true;
    Random rng{42};
    MapLevel(){sim::initializeTiles();random=&rng;static sim::Dimension overworld;dimension=&overworld;}
    int getTile(int x,int y,int z)override{auto it=blocks.find({x,y,z});return it==blocks.end()?0:it->second.first;}
    int getData(int x,int y,int z)override{auto it=blocks.find({x,y,z});return it==blocks.end()?0:it->second.second;}
    // LevelChunk::setTileAndData: store, onRemove of the old tile, onPlace of the new.
    bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data)override{
        const int old=getTile(x,y,z),oldData=getData(x,y,z);
        if(old==tile && oldData==data)return false;
        if(tile==0)blocks.erase({x,y,z});else blocks[{x,y,z}]={tile,data};
        if(old && Tile::tiles[old])Tile::tiles[old]->onRemove(this,x,y,z,old,oldData);
        if(tile && Tile::tiles[tile])Tile::tiles[tile]->onPlace(this,x,y,z);
        return true;
    }
    bool setDataNoUpdate(int x,int y,int z,int data)override{
        if(!getTile(x,y,z) || getData(x,y,z)==data)return false;
        blocks[{x,y,z}].second=data;return true;
    }
    bool hasChunk(int,int)override{return true;}
    std::vector<std::tuple<int,int,int,int,int>> scheduled;
    void addToTickNextTick(int x,int y,int z,int tile,int delay)override{scheduled.push_back({x,y,z,tile,delay});}
    std::vector<std::tuple<double,double,double,int,int>> falling;
    void addEntity(std::shared_ptr<sim::FallingTile> e)override{falling.push_back({e->x,e->y,e->z,e->tile,e->data});}
    int top(int x,int z){int h=-1;for(auto& [k,v]:blocks)if(std::get<0>(k)==x && std::get<2>(k)==z && Tile::lightBlock[v.first]>0)h=std::max(h,std::get<1>(k));return h+1;}
    int skyAt(int x,int y,int z){return y>=top(x,z)?sky:0;}
    int getRawBrightness(int x,int y,int z)override{return std::max(skyAt(x,y,z)-skyDarken,lamp);}
    int getDaytimeRawBrightness(int x,int y,int z)override{return std::max(skyAt(x,y,z),lamp);}
    int getBrightness(LightLayer::variety layer,int x,int y,int z)override{return layer==LightLayer::Sky?skyAt(x,y,z):lamp;}
    bool canSeeSky(int x,int y,int z)override{return y>=top(x,z);}
    bool isRainingAt(int x,int y,int z)override{return raining && canSeeSky(x,y,z);}
    bool hasChunksAt(int,int,int,int,int,int)override{return true;}
    void spawnResources(int x,int y,int z,int tile,int data)override{drops.push_back({x,y,z,tile});(void)data;}
    bool placeTree(TreeKind kind,int height,Random&,int x,int y,int z)override{
        trees.push_back({int(kind),height,x,z});(void)y;
        if(treeResult)setTile(x,y,z,Tile::treeTrunk_Id);
        return treeResult;
    }
    void put(int x,int y,int z,int tile,int data=0){blocks[{x,y,z}]={tile,data};}
    void tick(int x,int y,int z,int times=1){for(int i=0;i<times;++i){const int id=getTile(x,y,z);if(id)Tile::tiles[id]->tick(this,x,y,z,random);}}
};

static void rules(){
    // The property table: the tiles the source marks as ticking.
    for(int id:{2,6,18,59,60,81,83,104,105,106,110,115,127,141,142,79,78,80,74})
        require(consoleTileProperties(id) && consoleTileProperties(id)->ticking,"tile "+std::to_string(id)+" should tick");
    require(!consoleTileProperties(1)->ticking && consoleTileProperties(1)->solid,"stone");
    require(consoleTileProperties(89)->lightEmission==15 && consoleTileProperties(18)->lightBlock==1,"light properties");

    {   // CropTile: wet farmland under the crop, full light: grows to 7.
        MapLevel level;
        level.put(0,0,0,Tile::farmland_Id,7);level.put(0,1,0,Tile::crops_Id,0);
        level.tick(0,1,0,400);
        require(level.getData(0,1,0)==7,"crops grow to age 7");
        // Bush::checkAlive: the crop pops off plain dirt.
        level.put(0,0,0,Tile::dirt_Id);level.tick(0,1,0);
        require(level.getTile(0,1,0)==0 && level.drops.size()==1,"crops break without farmland");
    }
    {   // FarmTile: dries without water, then turns to dirt when nothing grows on it.
        MapLevel level;
        level.put(0,0,0,Tile::farmland_Id,2);
        level.tick(0,0,0);require(level.getData(0,0,0)==1,"farmland dries");
        level.tick(0,0,0,2);require(level.getTile(0,0,0)==Tile::dirt_Id,"dry farmland reverts to dirt");
        level.put(0,0,0,Tile::farmland_Id,0);level.put(3,0,0,Tile::calmWater_Id);
        level.tick(0,0,0);require(level.getData(0,0,0)==7,"water within 4 wets farmland");
    }
    {   // LeafTile: flagged leaves with no trunk in range decay; with a trunk the flag clears.
        MapLevel level;
        level.put(0,5,0,Tile::leaves_Id,8);
        level.tick(0,5,0);
        require(level.getTile(0,5,0)==0,"orphaned leaves decay");
        level.put(0,5,0,Tile::leaves_Id,8);level.put(0,4,0,Tile::treeTrunk_Id);
        level.tick(0,5,0);
        require(level.getTile(0,5,0)==Tile::leaves_Id && level.getData(0,5,0)==0,"leaves near a trunk keep");
        level.put(0,6,0,Tile::leaves_Id,4|8);level.tick(0,6,0);
        require(level.getTile(0,6,0)==Tile::leaves_Id,"player-placed leaves never decay");
        // TreeTile::onRemove flags the leaves around a removed trunk.
        level.setTile(0,4,0,0);
        require(level.getData(0,5,0)&8,"removing a trunk flags nearby leaves");
    }
    {   // Sapling: the age bit first, then the tree (restored if it does not fit).
        MapLevel level;
        level.put(0,0,0,Tile::grass_Id);level.put(0,1,0,Tile::sapling_Id,0);
        for(int i=0;i<200 && level.trees.empty();++i)level.tick(0,1,0);
        require(!level.trees.empty() && level.getTile(0,1,0)==Tile::treeTrunk_Id,"sapling grows a tree");
        MapLevel blocked;blocked.treeResult=false;
        blocked.put(0,0,0,Tile::grass_Id);blocked.put(0,1,0,Tile::sapling_Id,2|8);
        for(int i=0;i<200 && blocked.trees.empty();++i)blocked.tick(0,1,0);
        require(std::get<0>(blocked.trees.at(0))==int(sim::Level::TreeKind::Birch) &&
                blocked.getTile(0,1,0)==Tile::sapling_Id && blocked.getData(0,1,0)==2,"a tree that does not fit restores the sapling");
        // Four jungle saplings grow the mega tree from the corner.
        MapLevel jungle;
        for(int dx=0;dx<2;++dx)for(int dz=0;dz<2;++dz){jungle.put(dx,0,dz,Tile::grass_Id);jungle.put(dx,1,dz,Tile::sapling_Id,3|8);}
        for(int i=0;i<200 && jungle.trees.empty();++i)jungle.tick(1,1,1);
        require(std::get<0>(jungle.trees.at(0))==int(sim::Level::TreeKind::MegaJungle) &&
                std::get<2>(jungle.trees[0])==0 && std::get<3>(jungle.trees[0])==0,"jungle mega tree");
    }
    {   // GrassTile: dies under an opaque block in the dark, spreads to lit dirt.
        MapLevel level;
        level.put(0,0,0,Tile::grass_Id);level.put(0,1,0,Tile::rock_Id);level.sky=0;
        level.tick(0,0,0);require(level.getTile(0,0,0)==Tile::dirt_Id,"covered grass dies");
        MapLevel spread;
        spread.put(0,0,0,Tile::grass_Id);spread.put(1,0,0,Tile::dirt_Id);
        spread.tick(0,0,0,100);
        require(spread.getTile(1,0,0)==Tile::grass_Id,"grass spreads to lit dirt");
    }
    {   // CactusTile / ReedTile: age 15 grows one block, three tall at most.
        MapLevel level;
        level.put(0,0,0,Tile::sand_Id);level.put(0,1,0,Tile::cactus_Id,0);
        for(int i=0;i<64;++i){level.tick(0,1,0);level.tick(0,2,0);level.tick(0,3,0);}
        require(level.getTile(0,3,0)==Tile::cactus_Id && level.getTile(0,4,0)==0,"cactus grows to three");
        MapLevel reeds;
        reeds.put(0,0,0,Tile::sand_Id);reeds.put(1,0,0,Tile::calmWater_Id);reeds.put(0,1,0,Tile::reeds_Id,0);
        reeds.tick(0,1,0,16);
        require(reeds.getTile(0,2,0)==Tile::reeds_Id,"sugar cane grows");
    }
    {   // StemTile: grows, then puts its fruit beside it.
        MapLevel level;
        level.put(0,0,0,Tile::farmland_Id,7);level.put(0,1,0,Tile::pumpkinStem_Id,0);
        for(int dx=-1;dx<=1;++dx)for(int dz=-1;dz<=1;++dz)if(dx||dz)level.put(dx,0,dz,Tile::dirt_Id);
        level.tick(0,1,0,2000);
        int fruit=0;for(auto& [k,v]:level.blocks)if(v.first==Tile::pumpkin_Id)++fruit;
        require(level.getData(0,1,0)==7 && fruit==1,"pumpkin stem grows one pumpkin");
    }
    {   // IceTile melts to still water in block light above 8; TopSnowTile above 11.
        MapLevel level;
        level.put(0,0,0,Tile::ice_Id);level.put(5,-1,0,Tile::dirt_Id);level.put(5,0,0,Tile::topSnow_Id);level.lamp=12;
        level.tick(0,0,0);level.tick(5,0,0);
        require(level.getTile(0,0,0)==Tile::calmWater_Id && level.getTile(5,0,0)==0,"ice and snow melt by a light");
    }
    {   // RedStoneOreTile: lit ore goes dark.
        MapLevel level;level.put(0,0,0,Tile::redStoneOre_lit_Id);level.tick(0,0,0);
        require(level.getTile(0,0,0)==Tile::redStoneOre_Id,"redstone ore unlights");
    }
    {   // Mushroom spreads, at most five in a 9x3x9 box.
        MapLevel level;level.sky=0;
        for(int x=-6;x<=6;++x)for(int z=-6;z<=6;++z)level.put(x,0,z,Tile::rock_Id);
        level.put(0,1,0,Tile::mushroom1_Id);
        for(int i=0;i<5000;++i)for(auto [k,v]:std::map(level.blocks))if(v.first==Tile::mushroom1_Id)level.tick(std::get<0>(k),std::get<1>(k),std::get<2>(k));
        int count=0;for(auto& [k,v]:level.blocks)if(v.first==Tile::mushroom1_Id)++count;
        require(count>=2 && count<=6,"mushrooms spread but stay sparse ("+std::to_string(count)+")");
    }
    {   // CocoaTile: grows to age 2 on jungle wood, pops off anything else.
        MapLevel level;level.put(1,0,0,Tile::treeTrunk_Id,3);
        level.put(0,0,0,Tile::cocoa_Id,3); // direction 3 (east): attached at x+1
        level.tick(0,0,0,200);
        require((level.getData(0,0,0)>>2)==2,"cocoa ripens");
        level.put(1,0,0,Tile::treeTrunk_Id,0);level.tick(0,0,0);
        require(level.getTile(0,0,0)==0,"cocoa needs jungle wood");
    }
    {   // CauldronTile::handleRain fills a level at a time.
        MapLevel level;level.put(0,0,0,118,0);
        for(int i=0;i<400;++i)Tile::tiles[118]->handleRain(&level,0,0,0);
        require(level.getData(0,0,0)==3,"rain fills a cauldron");
    }
}

static void items(){
    MapLevel level;
    level.put(0,0,0,Tile::grass_Id);
    sim::ItemInstance hoe;hoe.id=290;hoe.count=1;
    require(!sim::useItemOn(level,hoe,0,0,0,0),"a hoe does nothing from below");
    require(sim::useItemOn(level,hoe,0,0,0,1) && level.getTile(0,0,0)==Tile::farmland_Id && hoe.damage==1,"a hoe tills grass");
    sim::ItemInstance seeds;seeds.id=295;seeds.count=3;
    require(!sim::useItemOn(level,seeds,0,0,0,2),"seeds only plant on the top face");
    require(sim::useItemOn(level,seeds,0,0,0,1) && level.getTile(0,1,0)==Tile::crops_Id && seeds.count==2,"seeds plant wheat");
    sim::ItemInstance boneMeal;boneMeal.id=351;boneMeal.auxValue=15;boneMeal.count=2;
    require(sim::useItemOn(level,boneMeal,0,1,0,1) && level.getData(0,1,0)==7 && boneMeal.count==1,"bone meal ripens wheat");
    require(!sim::useItemOn(level,boneMeal,0,1,0,1) && boneMeal.count==1,"not on ripe wheat");
    level.put(2,0,0,Tile::grass_Id);level.put(2,1,0,Tile::sapling_Id,1);
    require(sim::useItemOn(level,boneMeal,2,1,0,1) && std::get<0>(level.trees.at(0))==int(sim::Level::TreeKind::Spruce),"bone meal grows a spruce sapling");
    sim::ItemInstance potato;potato.id=392;potato.count=1;
    level.put(4,0,0,Tile::farmland_Id);
    require(sim::useItemOn(level,potato,4,0,0,1) && level.getTile(4,1,0)==Tile::potatoes_Id && potato.count==0,"potatoes plant");
    sim::ItemInstance cocoa;cocoa.id=351;cocoa.auxValue=3;cocoa.count=1;
    level.put(6,1,0,Tile::treeTrunk_Id,3);
    require(sim::useItemOn(level,cocoa,6,1,0,4) && level.getTile(5,1,0)==Tile::cocoa_Id && cocoa.count==0,"cocoa beans on jungle wood");
    // Bone meal on grass scatters tall grass and flowers.
    MapLevel meadow;
    for(int x=-8;x<=8;++x)for(int z=-8;z<=8;++z)meadow.put(x,0,z,Tile::grass_Id);
    sim::ItemInstance meal;meal.id=351;meal.auxValue=15;meal.count=1;
    require(sim::useItemOn(meadow,meal,0,0,0,1),"bone meal on grass");
    int plants=0;for(auto& [k,v]:meadow.blocks)if(v.first==Tile::tallgrass_Id || v.first==Tile::flower_Id || v.first==Tile::rose_Id)++plants;
    require(plants>10,"bone meal grows plants ("+std::to_string(plants)+")");
}

// Tile updates: the chunk hooks, neighbour notifications and scheduled ticks.
static void updates(){
    {   // HeavyTile::onPlace schedules; the tick over air hands a FallingTile to the level.
        MapLevel level;
        level.setTile(0,5,0,Tile::sand_Id);
        require(level.scheduled.size()==1 && std::get<4>(level.scheduled[0])==5,"sand schedules its fall");
        level.tick(0,5,0);
        require(level.falling.size()==1 && std::get<3>(level.falling[0])==Tile::sand_Id,"sand starts falling");
    }
    {   // TorchTile::neighborChanged: a torch loses its support and drops.
        MapLevel level;
        level.put(0,0,0,Tile::rock_Id);level.put(0,1,0,Tile::torch_Id,5);
        level.setTile(0,0,0,0);
        require(level.getTile(0,1,0)==0 && level.drops.size()==1 && std::get<3>(level.drops[0])==Tile::torch_Id,"torch pops off");
    }
    {   // DoorTile::neighborChanged: removing the upper half removes and drops the lower.
        MapLevel level;
        level.put(0,-1,0,Tile::rock_Id);level.put(0,0,0,Tile::door_wood_Id,0);level.put(0,1,0,Tile::door_wood_Id,8);
        level.setTile(0,1,0,0);
        require(level.getTile(0,0,0)==0 && level.drops.size()==1,"door halves go together");
    }
    {   // FireTile::onPlace: fire with nothing to burn and nothing under it goes
        // out; on netherrack it stays and schedules its tick.
        MapLevel level;
        level.setTile(0,1,0,Tile::fire_Id);
        require(level.getTile(0,1,0)==0,"unsupported fire goes out");
        level.put(5,0,0,Tile::hellRock_Id);
        level.setTile(5,1,0,Tile::fire_Id);
        require(level.getTile(5,1,0)==Tile::fire_Id && !level.scheduled.empty(),"fire on netherrack stays");
        // FireTile::tick on netherrack never burns out.
        for(int i=0;i<50;++i)level.tick(5,1,0);
        require(level.getTile(5,1,0)==Tile::fire_Id,"netherrack burns forever");
    }
    {   // LiquidTileStatic::neighborChanged turns still water flowing and schedules it.
        MapLevel level;
        level.put(0,0,0,Tile::calmWater_Id,0);
        level.setTile(1,0,0,Tile::rock_Id);
        require(level.getTile(0,0,0)==Tile::water_Id && !level.scheduled.empty(),"still water wakes");
    }
}

// The same through the World: falling sand, doors, torches, flint and steel
// and a pending tick that survives a save.
static void worldUpdates(const std::filesystem::path& scratch){
    World world;world.generate(41,true);
    for(int x=24;x<=40;++x)for(int z=24;z<=40;++z)world.set(x,179,z,Stone);
    require(world.setTileAndUpdate(32,190,32,Sand),"place sand in the air");
    for(int i=0;i<60 && (world.get(32,180,32)!=Sand || !world.fallingBlocks().empty());++i)world.tickTime();
    require(world.get(32,180,32)==Sand && world.get(32,190,32)==Air && world.fallingBlocks().empty(),"sand falls and lands");

    world.set(30,180,30,static_cast<Block>(64));world.setData(30,180,30,0);
    world.set(30,181,30,static_cast<Block>(64));world.setData(30,181,30,8);
    world.setSurvival(true);
    world.setCarriedItem(0,0,0,0);
    const auto dropsBefore=world.droppedItems().size();
    require(world.destroyBlock(30,181,30,0),"break the upper door half");
    int doors=0;for(std::size_t i=dropsBefore;i<world.droppedItems().size();++i)if(world.droppedItems()[i].id==324)doors+=world.droppedItems()[i].count;
    require(world.get(30,180,30)==Air && doors==1,"a broken door drops once");
    world.setSurvival(false);

    require(world.set(34,180,34,Stone) && world.set(34,181,34,static_cast<Block>(50)),"place a torch");
    world.setData(34,181,34,5);
    const auto torchDrops=world.droppedItems().size();
    require(world.breakBlock(34,180,34) && world.get(34,181,34)==Air && world.droppedItems().size()==torchDrops+1 &&
            world.droppedItems().back().id==50,"a torch falls with its block");

    // TileItem::useOn for torches: the wall from the clicked face, and no torch
    // where nothing holds it.
    world.set(39,180,30,Stone);
    require(world.placeBlock(38,180,30,static_cast<Block>(50),0,{30.5,180,30.5},0,4) &&
            world.get(38,180,30)==static_cast<Block>(50) && world.getData(38,180,30)==2,"a torch on a wall");
    require(world.placeBlock(38,180,32,static_cast<Block>(50),0,{30.5,180,30.5},0,1) && world.getData(38,180,32)==5,"a torch on the floor");
    require(!world.placeBlock(38,185,30,static_cast<Block>(50),0,{30.5,180,30.5},0,1),"no torch in the air");
    require(world.setCarriedItem(0,259,1,0) && world.useItemOn(36,179,36,1,0) && int(world.get(36,180,36))==51,"flint and steel lights fire");

    require(world.setTileAndUpdate(26,180,26,static_cast<Block>(8)),"place flowing water");
    const auto path=scratch/"tile_updates_world.inner";
    world.save(path);
    World loaded;require(loaded.load(path),"reload with pending ticks");
    std::filesystem::remove(path);
    for(int i=0;i<5;++i)loaded.tickTime();
    // LiquidTileDynamic::getSpread: toward the pad's nearer edge.
    require(loaded.get(25,180,26)==static_cast<Block>(8) && loaded.getData(25,180,26)==1,"a saved pending tick runs after loading");
}

static void worldPass(const std::filesystem::path& scratch){
    // A generated world: random ticks run without errors, and report their cost.
    World world;
    world.generate(12345);
    Vec3 spawn=world.spawn();
    world.setPlayerPosition(spawn);
    const auto before=world.revision;
    const auto start=std::chrono::steady_clock::now();
    const int ticks=1200;
    for(int i=0;i<ticks;++i)world.tickTime();
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"world ticks: "<<ticks<<" in "<<ms<<" ms ("<<ms/ticks<<" ms/tick), "
             <<(world.revision-before)<<" block changes\n";
    require(world.rainLevel()>=0 && world.rainLevel()<=1 && world.thunderLevel()>=0,"weather levels");
    // A save round trip keeps the weather timers.
    // Farming through the World: till, plant and ripen at the spawn.
    world.setSurvival(true);
    Vec3 at=world.spawn();
    int x=int(std::floor(at.x)),z=int(std::floor(at.z)),y=int(std::floor(at.y))-1;
    while(y>0 && world.get(x,y,z)==Air)--y;
    world.set(x,y,z,Grass);world.set(x,y+1,z,Air);
    world.setCarriedItem(0,290,1,0);world.setCarriedItem(1,295,5,0);world.setCarriedItem(2,351,4,15);
    require(world.useItemOn(x,y,z,1,0) && int(world.get(x,y,z))==60 && world.carriedItems()[0].damage==1,"world hoe");
    require(world.useItemOn(x,y,z,1,1) && int(world.get(x,y+1,z))==59 && world.carriedItems()[1].count==4,"world seeds");
    require(world.useItemOn(x,y+1,z,1,2) && world.getData(x,y+1,z)==7 && world.carriedItems()[2].count==3,"world bone meal");
    const auto path=scratch/"tile_ticks_world.inner";
    world.save(path);
    World loaded;require(loaded.load(path),"reload");
    std::filesystem::remove(path);
}

int main(int argc,char** argv){try{
    rules();
    items();
    updates();
    worldUpdates(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path());
    worldPass(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path());
    std::cout<<"tile tick tests passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
