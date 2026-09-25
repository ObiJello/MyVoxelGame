// The original random tile tick rules (ported/tick) on a small map level, and
// the World's random tick pass on a generated world.
#include "TileTickHost.h"
#include "TileProperties.h"
#include "TileSurvival.h"
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
    void spawnResources(int x,int y,int z,int tile,int data,float)override{drops.push_back({x,y,z,tile});(void)data;}
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
    // Destroy times from the registration chains agree with the survival table.
    for(int id=1;id<256;++id)if(const auto* p=consoleTileProperties(id))if(const auto* s=consoleSurvivalTile(id))
        require(std::abs(p->destroyTime-s->destroyTime)<1e-6f,"destroy time of tile "+std::to_string(id));
    require(consoleTileProperties(49)->explosionResistance==6000 && consoleTileProperties(1)->explosionResistance==30,"explosion resistance");
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

// Redstone through the World: levers, dust, lamps, torches, repeaters,
// pressure plates and doors, with the World's own scheduled ticks.
static void redstone(){
    World world;world.generate(46,true);
    for(int x=20;x<=44;++x)for(int z=20;z<=44;++z)world.set(x,179,z,Stone);
    const console::Vec3 feet{22.5,180,22.5};
    auto B=[](int id){return static_cast<Block>(id);};
    // A lever on the floor, three dust and a lamp.
    require(world.placeBlock(26,180,30,B(69),0,feet,0,1) && world.get(26,180,30)==B(69),"place a lever");
    for(int x=27;x<=29;++x)require(world.placeBlock(x,180,30,B(55),0,feet,0,1),"place dust");
    require(!world.placeBlock(27,185,30,B(55),0,feet,0,1),"no dust in the air");
    require(world.placeBlock(30,180,30,B(123),0,feet,0),"place a lamp");
    require(world.usable(26,180,30) && world.useBlock(26,180,30),"pull the lever");
    require(world.getData(27,180,30)==15 && world.getData(29,180,30)==13,"dust carries the signal, one less each step");
    require(world.get(30,180,30)==B(124),"the lamp lights");
    world.useBlock(26,180,30);
    require(world.getData(28,180,30)==0,"dust goes dark");
    for(int i=0;i<3;++i)world.tickTime();
    require(world.get(30,180,30)==B(124),"a lamp stays lit for four ticks");
    for(int i=0;i<2;++i)world.tickTime();
    require(world.get(30,180,30)==B(123),"then goes out");

    // A redstone torch on the side of a block turns off when the block is powered.
    world.set(32,180,34,Stone);
    require(world.placeBlock(33,180,34,B(76),0,feet,0,5) && world.getData(33,180,34)==1,"a redstone torch on a wall");
    require(world.placeBlock(32,181,34,B(69),0,feet,0,1),"a lever on the block");
    world.useBlock(32,181,34);
    world.tickTime();
    require(world.get(33,180,34)==B(76),"the torch waits two ticks");
    world.tickTime();world.tickTime();
    require(world.get(33,180,34)==B(75),"the powered block turns the torch off");
    world.useBlock(32,181,34);
    for(int i=0;i<3;++i)world.tickTime();
    require(world.get(33,180,34)==B(76),"and back on");

    // A repeater facing south (data 0) takes its input from the south side and
    // powers the north side after its delay.
    require(world.set(36,180,32,B(93)),"place a repeater");world.setData(36,180,32,0);
    require(world.placeBlock(36,180,33,B(69),0,feet,0,1) && world.placeBlock(36,180,31,B(123),0,feet,0),"lever and lamp");
    world.useBlock(36,180,33);
    world.tickTime();
    require(world.get(36,180,31)==B(123),"the repeater waits");
    world.tickTime();world.tickTime();
    require(world.get(36,180,32)==B(94) && world.get(36,180,31)==B(124),"the repeater passes the signal on");
    require(world.usable(36,180,32) && world.useBlock(36,180,32) && (world.getData(36,180,32)>>2)==1,"right click sets a longer delay");

    // A wooden pressure plate under the player.
    require(world.placeBlock(40,180,36,B(72),0,feet,0,1),"place a pressure plate");
    world.setPlayerPosition({40.5,180,36.5});
    world.tickTime();
    require(world.getData(40,180,36)==1,"the player presses the plate");
    world.setPlayerPosition({30.5,180,40.5});
    for(int i=0;i<25;++i)world.tickTime();
    require(world.getData(40,180,36)==0,"the plate lifts when the player steps off");

    // DoorTile::use opens both halves' composite state from either half.
    world.set(24,180,40,B(64));world.setData(24,180,40,0);
    world.set(24,181,40,B(64));world.setData(24,181,40,8);
    require(world.useBlock(24,181,40) && (world.getData(24,180,40)&4),"use a door's top half");
    require(world.useBlock(24,180,40) && !(world.getData(24,180,40)&4),"and close it from below");
}

// Pistons through the World: tile events, moving pieces and pushing.
static void pistons(){
    World world;world.generate(47,true);
    for(int x=20;x<=44;++x)for(int z=20;z<=44;++z)world.set(x,179,z,Stone);
    auto B=[](int id){return static_cast<Block>(id);};
    const console::Vec3 feet{22.5,180,40.5};
    auto run=[&](int ticks){for(int i=0;i<ticks;++i)world.tickTime();};
    // A piston facing east (Facing 5) with a lever behind it and stone in front.
    require(world.set(30,180,30,B(33)),"place a piston");world.setData(30,180,30,5);
    world.set(31,180,30,Stone);
    require(world.placeBlock(29,180,30,B(69),0,feet,0,1),"a lever behind the piston");
    world.useBlock(29,180,30);
    run(4);
    require(world.get(30,180,30)==B(33) && world.getData(30,180,30)==13,"the piston extends");
    require(world.get(31,180,30)==B(34) && world.get(32,180,30)==Stone,"the head pushes the stone along");
    world.useBlock(29,180,30);
    run(4);
    require(world.get(30,180,30)==B(33) && world.getData(30,180,30)==5 && world.get(31,180,30)==Air &&
            world.get(32,180,30)==Stone,"a plain piston retracts and leaves the stone");
    // A sticky piston pulls it back.
    require(world.set(30,180,34,B(29)),"place a sticky piston");world.setData(30,180,34,5);
    world.set(31,180,34,Stone);
    world.placeBlock(29,180,34,B(69),0,feet,0,1);
    world.useBlock(29,180,34);run(4);
    require(world.get(32,180,34)==Stone,"the sticky piston pushes");
    world.useBlock(29,180,34);run(4);
    require(world.get(31,180,34)==Stone && world.get(32,180,34)==Air,"and pulls the stone back");
    // Obsidian does not move (PistonBaseTile::isPushable).
    require(world.set(30,180,38,B(33)),"another piston");world.setData(30,180,38,5);
    world.set(31,180,38,Obsidian);
    world.placeBlock(29,180,38,B(69),0,feet,0,1);
    world.useBlock(29,180,38);run(4);
    require(world.getData(30,180,38)==5 && world.get(31,180,38)==Obsidian,"obsidian stops a piston");
    // The head pushes the player standing in front of it.
    require(world.set(34,180,42,B(33)),"a piston by the player");world.setData(34,180,42,5);
    world.placeBlock(33,180,42,B(69),0,feet,0,1);
    world.setPlayerPosition({35.5,180,42.5});
    world.takePlayerPush();
    world.useBlock(33,180,42);run(4);
    require(world.get(35,180,42)==B(34) && world.takePlayerPush().x>.5,"the piston pushes the player");
    // Placing a piston faces it from the player (PistonBaseTile::getNewFacing).
    require(world.placeBlock(40,180,26,B(33),0,{40.5,180,20.5},0,1),"place a piston by hand");
    require(world.getData(40,180,26)<6,"a placed piston faces a direction");
}

// Entity::move over MapLevel: full cubes for solid tiles, half slabs.
struct BoxLevel final:sim::Level {
    MapLevel& map;
    explicit BoxLevel(MapLevel& m):map(m){random=m.random;dimension=m.dimension;}
    int getTile(int x,int y,int z)override{return map.getTile(x,y,z);}
    int getData(int x,int y,int z)override{return map.getData(x,y,z);}
    bool setTileAndDataNoUpdate(int x,int y,int z,int t,int d)override{return map.setTileAndDataNoUpdate(x,y,z,t,d);}
    bool setDataNoUpdate(int x,int y,int z,int d)override{return map.setDataNoUpdate(x,y,z,d);}
    bool hasChunk(int,int)override{return true;}
    int getRawBrightness(int x,int y,int z)override{return map.getRawBrightness(x,y,z);}
    int getDaytimeRawBrightness(int x,int y,int z)override{return map.getDaytimeRawBrightness(x,y,z);}
    int getBrightness(LightLayer::variety l,int x,int y,int z)override{return map.getBrightness(l,x,y,z);}
    bool canSeeSky(int x,int y,int z)override{return map.canSeeSky(x,y,z);}
    bool isRainingAt(int,int,int)override{return false;}
    bool hasChunksAt(int,int,int,int,int,int)override{return true;}
    void spawnResources(int,int,int,int,int,float)override{}
    bool placeTree(TreeKind,int,Random&,int,int,int)override{return false;}
    void addTileAABBs(Tile* tile,int x,int y,int z,AABB* box,sim::AABBList* boxes,std::shared_ptr<sim::Entity>)override{
        const double top=tile->id==Tile::stoneSlabHalf_Id?.5:1;
        if(!Tile::solid[tile->id] && tile->id!=Tile::stoneSlabHalf_Id)return;
        AABB* shape=AABB::newTemp(x,y,z,x+1,y+top,z+1);
        if(!box || shape->intersects(box))boxes->push_back(shape);
    }
};
struct TestEntity final:sim::Entity { using Entity::Entity; };

static void entities(){
    MapLevel map;BoxLevel level(map);
    for(int x=-4;x<=6;++x)for(int z=-4;z<=4;++z)map.put(x,0,z,Tile::rock_Id);
    // A falling box lands on the floor (Entity::move's y clip, onGround).
    auto e=std::make_shared<TestEntity>(&level);
    e->setPos(0.5,5,0.5);
    for(int i=0;i<60;++i){e->yd-=0.08;e->move(e->xd,e->yd,e->zd);e->yd*=0.98;}
    require(e->onGround && std::abs(e->y-1)<1e-9 && e->fallDistance==0,"an entity falls onto the floor");
    // It stops against a wall (the x clip).
    map.put(2,1,0,Tile::rock_Id);map.put(2,2,0,Tile::rock_Id);
    e->move(2,0,0);
    require(e->horizontalCollision && std::abs(e->x-(2-0.3))<1e-6,"a wall stops it at its half width");
    // With a foot size of half a block it steps up onto a slab.
    map.put(2,1,0,0);map.put(2,2,0,0);map.put(2,1,0,Tile::stoneSlabHalf_Id);
    e->setPos(1.2,1,0.5);e->onGround=true;e->footSize=0.5f;
    e->move(1,0,0);
    // The box is on the slab; y lags by ySlideOffset (the smooth step).
    require(std::abs(e->bb->y0-1.5)<1e-6 && e->x>2 && e->ySlideOffset>0.5f,"it steps up half a block");
    // In water, Entity::baseTick notices (updateInWaterState -> checkAndHandleWater).
    map.put(-2,1,0,Tile::water_Id);map.put(-2,2,0,Tile::water_Id);
    e->setPos(-1.5,1,0.5);e->fallDistance=3;
    e->baseTick();
    require(e->isInWater() && !e->isInLava() && e->fallDistance==0,"it is in water, which stops a fall");
    e->setPos(4.5,1,0.5);
    require(!e->updateInWaterState(),"and out of it");
    // Entity::isFree and isInWall.
    require(e->isFree(0.0,0.0,0.0) && !e->isFree(0.0,-1.0,0.0),"standing in the open is free, the floor is not");
    e->setPos(0.5,0.2,0.5);
    require(e->isInWall(),"inside the floor is in a wall");
}

// A PathfinderMob with the new AI (navigation, move and look controls).
struct Walker final:sim::PathfinderMob {
    explicit Walker(sim::Level* level):PathfinderMob(level){health=getMaxHealth();}
    int getMaxHealth()override{return 10;}
    bool useNewAi()override{return true;}
    int getHealthNow(){return health;}
};

static void mobs(){
    MapLevel map;BoxLevel level(map);
    for(int x=-6;x<=12;++x)for(int z=-8;z<=8;++z)map.put(x,0,z,Tile::rock_Id);
    auto mob=std::make_shared<Walker>(&level);
    mob->moveTo(0.5,3,0.5,0,0);
    for(int i=0;i<40;++i)mob->tick();
    require(mob->onGround && std::abs(mob->y-1)<1e-6,"a mob falls onto the floor (Mob::travel)");
    // A hit knocks it back and away from the attacker, then the
    // invulnerability window absorbs a weaker second hit (Mob::hurt).
    auto attacker=std::make_shared<Walker>(&level);
    attacker->moveTo(-1.5,1,0.5,0,0);
    require(mob->hurt(sim::DamageSource::mobAttack(attacker),3),"a mob can be hurt");
    require(mob->getHealthNow()==7 && mob->xd>0 && mob->yd>0,"the hit costs health and knocks it away");
    require(!mob->hurt(sim::DamageSource::mobAttack(attacker),2) && mob->getHealthNow()==7,"a weaker hit in the window does nothing");
    require(mob->getLastHurtByMob()==attacker,"it remembers who hurt it");
    // Path finding around a wall (PathNavigation, PathFinder, MoveControl).
    for(int z=-4;z<=4;++z){map.put(4,1,z,Tile::rock_Id);map.put(4,2,z,Tile::rock_Id);}
    auto walker=std::make_shared<Walker>(&level);
    walker->moveTo(0.5,1,0.5,0,0);
    for(int i=0;i<3;++i)walker->tick(); // PathNavigation::canUpdatePath: only from the ground
    require(walker->getNavigation()->moveTo(8.5,1,0.5,0.3f),"a path around the wall");
    for(int i=0;i<600 && !walker->getNavigation()->isDone();++i)walker->tick();
    require(walker->distanceTo(8.5,1,0.5)<1.5,"the mob walks round the wall to the target");
    // Health 0: the death animation, then it is removed (Mob::tickDeath).
    mob->invulnerableTime=0;
    mob->hurt(sim::DamageSource::genericSource,20);
    require(!mob->isAlive(),"a fatal hit kills it");
    for(int i=0;i<25;++i)mob->tick();
    require(mob->removed,"a dead mob is removed after its death animation");
}

// Level::tickEntities for a BoxLevel: each entity's tick, then the removed go.
static void tickAll(BoxLevel& level,int ticks){
    for(int i=0;i<ticks;++i){
        const auto current=level.entities;
        for(const auto& e:current)if(!e->removed)e->tick();
        std::erase_if(level.entities,[](const auto& e){return e->removed;});
    }
}
template<class T> static std::shared_ptr<T> spawn(BoxLevel& level,double x,double y,double z){
    auto mob=std::make_shared<T>(&level);
    mob->moveTo(x,y,z,0,0);
    level.entities.push_back(mob);
    return mob;
}
template<class T> static int countOf(BoxLevel& level){
    int n=0;for(const auto& e:level.entities)if(std::dynamic_pointer_cast<T>(e) && !e->removed)++n;
    return n;
}

static void animals(){
    MapLevel map;BoxLevel level(map);
    for(int x=-16;x<=16;++x)for(int z=-16;z<=16;++z)map.put(x,0,z,Tile::grass_Id);
    // Pigs wander (RandomStrollGoal) while a player is within 32 blocks
    // (Mob::checkDespawn keeps their idle time down).
    auto player=std::make_shared<sim::Player>();player->level=&level;
    player->moveTo(10.5,1,10.5,0,0);
    level.players.push_back(player);
    // RandomStrollGoal rolls 1 in 120 each time the goals are chosen.
    auto pig=spawn<sim::Pig>(level,0.5,1,0.5);
    for(int i=0;i<3000 && pig->distanceTo(0.5,1,0.5)<=1;++i)tickAll(level,1);
    require(pig->distanceTo(0.5,1,0.5)>1 && pig->onGround,"a pig wanders off");
    // Hit, it panics (PanicGoal): it runs to random spots nearby for as
    // long as it remembers the hit (Mob::lastHurtByMobTime, 60 ticks).
    player->moveTo(pig->x-1,1,pig->z,0,0);
    const double hitX=pig->x,hitZ=pig->z;
    require(pig->hurt(sim::DamageSource::playerAttack(player),1),"the pig is hit");
    double furthest=0;
    for(int i=0;i<60;++i){tickAll(level,1);furthest=std::max(furthest,std::hypot(pig->x-hitX,pig->z-hitZ));}
    require(furthest>2.5,"it panics and runs");
    // A cow follows wheat (TemptGoal).
    level.entities.clear();
    auto cow=spawn<sim::Cow>(level,-6.5,1,0.5);
    player->moveTo(0.5,1,0.5,0,0);
    player->inventory->items[0]=std::make_shared<sim::ItemInstance>(sim::Item::wheat_Id,5,0);
    tickAll(level,200);
    require(cow->distanceTo(player->x,player->y,player->z)<3.5,"a cow follows wheat");
    // Fed wheat, two cows fall in love (Animal::interact) and breed a calf
    // (BreedGoal); the parents' age is set and the calf is a baby.
    auto mate=spawn<sim::Cow>(level,cow->x+1,1,cow->z);
    tickAll(level,5);
    require(cow->interact(player) && mate->interact(player),"feeding wheat puts cows in love");
    require(player->inventory->items[0]->count==3,"feeding uses the wheat");
    player->inventory->items[0]=nullptr;
    tickAll(level,200);
    require(countOf<sim::Cow>(level)==3,"two cows in love breed a calf");
    std::shared_ptr<sim::Cow> calf;
    for(const auto& e:level.entities)if(auto c=std::dynamic_pointer_cast<sim::Cow>(e);c && c!=cow && c!=mate)calf=c;
    require(calf && calf->isBaby() && cow->getAge()>0,"the calf is a baby and the parents wait");
    // A sheared lamb eats grass (EatTileGoal): the grass turns to dirt and
    // the wool grows back (Sheep::ate).
    level.entities.clear();level.players.clear();
    auto lamb=spawn<sim::Sheep>(level,4.5,1,4.5);
    lamb->setAge(-24000);lamb->setSheared(true);
    bool ate=false;
    for(int i=0;i<4000 && !ate;++i){tickAll(level,1);ate=!lamb->isSheared();}
    int dirt=0;for(int x=-16;x<=16;++x)for(int z=-16;z<=16;++z)dirt+=map.getTile(x,0,z)==Tile::dirt_Id;
    require(ate && dirt>=1,"a lamb eats grass, which turns to dirt, and grows its wool back");
    // A chicken lays an egg within its egg time (Chicken::aiStep).
    level.entities.clear();
    auto chicken=spawn<sim::Chicken>(level,-4.5,1,-4.5);
    tickAll(level,12500);
    bool egg=false;
    for(const auto& e:level.entities)if(auto item=std::dynamic_pointer_cast<sim::ItemEntity>(e))egg|=item->getItem()->id==sim::Item::egg_Id;
    require(egg,"a chicken lays an egg");
}

static void tnt(){
    World world;world.generate(53,true);
    for(int x=16;x<=48;++x)for(int z=16;z<=48;++z){
        world.set(x,179,z,Stone);
        for(int y=180;y<186;++y)world.set(x,y,z,Air);
    }
    auto B=[](int id){return static_cast<Block>(id);};
    auto run=[&](int ticks){for(int i=0;i<ticks;++i)world.tickTime();};
    const console::Vec3 feet{40.5,180,40.5};
    world.setPlayerPosition(feet);
    // Flint and steel lights TNT (TntTile::use): the tile becomes a primed
    // entity that falls, flashes and explodes after 80 ticks.
    world.setSurvival(true);
    world.addCarriedItem(259,1);
    int flint=-1;
    for(int i=0;i<36;++i)if(world.carriedItems()[i].id==259)flint=i;
    require(flint>=0,"flint and steel is carried");
    require(world.set(24,180,24,B(46)),"place TNT");
    world.set(24,180,26,Obsidian);
    require(world.useItemOn(24,180,24,1,flint),"flint and steel lights TNT");
    require(world.get(24,180,24)==Air && world.primedTnt().size()==1,"lit TNT becomes a primed entity");
    require(world.carriedItems()[flint].damage==0,"lighting TNT does not wear the flint and steel");
    require(std::abs(world.primedTnt()[0].position.y-180.5)<.3,"the entity starts at the block centre");
    run(40);
    require(world.primedTnt().size()==1 && world.primedTnt()[0].life<45,"the fuse burns down");
    run(45);
    require(world.primedTnt().empty(),"the TNT explodes after its fuse");
    require(world.get(24,179,24)==Air && world.get(23,180,24)==Air,"the explosion breaks the floor around it");
    require(world.get(24,180,26)==Obsidian,"obsidian survives an explosion");
    // Redstone lights TNT too (TntTile::neighborChanged), and an explosion
    // lights the TNT next to it with a short fuse (TntTile::wasExploded).
    require(world.set(36,180,20,B(46)) && world.set(36,180,22,B(46)),"two TNT blocks");
    require(world.placeBlock(35,180,20,B(69),0,{30.5,180,20.5},0,1),"a lever by the TNT");
    world.useBlock(35,180,20);
    run(1);
    require(world.get(36,180,20)==Air && world.primedTnt().size()==1,"a powered lever lights TNT");
    run(81);
    require(world.get(36,180,22)==Air && world.primedTnt().size()==1 && world.primedTnt()[0].life<31,
            "an explosion lights the TNT beside it with a short fuse");
    run(40);
    require(world.primedTnt().empty(),"the chained TNT explodes");
    // A player beside an explosion is hurt and thrown back.
    const int before=world.playerHealth();
    world.setPlayerPosition({42.5,180,40.5});
    world.takePlayerKnockback();
    require(world.set(40,180,40,B(46)),"TNT by the player");
    require(world.useItemOn(40,180,40,1,flint),"light it");
    run(81);
    require(world.playerHealth()<before,"the explosion hurts the player");
    require(world.takePlayerKnockback().x>0,"and pushes them away");
}

static void dispensers(){
    World world;world.generate(59,true);
    for(int x=16;x<=48;++x)for(int z=16;z<=48;++z){
        world.set(x,179,z,Stone);
        for(int y=180;y<186;++y)world.set(x,y,z,Air);
    }
    auto B=[](int id){return static_cast<Block>(id);};
    auto run=[&](int ticks){for(int i=0;i<ticks;++i)world.tickTime();};
    world.setPlayerPosition({40.5,180,40.5});
    world.setSurvival(true);
    auto carriedSlot=[&](int id){
        const auto items=world.carriedItems();
        for(int i=0;i<36;++i)if(items[i].id==id)return i;
        return -1;
    };
    auto load=[&](int x,int y,int z,int id,int count,int damage=0){
        world.addCarriedItem(id,count,damage);
        require(world.transferDispenserItem(x,y,z,carriedSlot(id),false),"fill the dispenser");
    };
    auto countIn=[&](int x,int y,int z,int id){
        int n=0;for(const auto& item:world.dispenserItems(x,y,z))if(item.id==id)n+=item.count;
        return n;
    };
    // Placing faces it from the player (DispenserTile::setPlacedBy); the
    // texture's front is on that side.
    require(world.placeBlock(24,180,24,B(23),0,{24.5,180,30.5},0,1),"place a dispenser");
    require(world.getData(24,180,24)>=2 && world.getData(24,180,24)<=5,"a placed dispenser faces a side");
    world.setData(24,180,24,5);
    require(textureTile(B(23),3,5)==46 && textureTile(B(23),2,5)==45 && textureTile(B(23),0,5)==62,
            "the dispenser's front faces east");
    require(world.canOpenDispenser(24,180,24) && world.dispenserItems(24,180,24).size()==9,"a dispenser has nine slots");
    // Cobblestone is thrown out in front when the dispenser is powered.
    load(24,180,24,4,3);
    require(countIn(24,180,24,4)==3,"the dispenser holds the stack");
    const auto dropsBefore=world.droppedItems().size();
    require(world.placeBlock(23,180,24,B(69),0,{30.5,180,30.5},0,1),"a lever behind it");
    world.useBlock(23,180,24);
    run(5);
    require(countIn(24,180,24,4)==2,"a signal dispenses one item");
    require(world.droppedItems().size()==dropsBefore+1,"the item is thrown out");
    const auto& thrown=world.droppedItems().back();
    require(thrown.id==4 && thrown.position.x>24.5 && thrown.velocity.x>0,"it flies out of the front");
    // It fires on the rising edge only.
    run(10);
    require(countIn(24,180,24,4)==2,"a steady signal fires once");
    world.useBlock(23,180,24);run(2);world.useBlock(23,180,24);run(5);
    require(countIn(24,180,24,4)==1,"a new signal fires again");
    // A water bucket empties in front; the empty bucket takes the source back.
    require(world.set(30,180,30,B(23)),"a second dispenser");world.setData(30,180,30,5);
    load(30,180,30,326,1);
    require(world.placeBlock(29,180,30,B(69),0,{30.5,180,36.5},0,1),"its lever");
    world.useBlock(29,180,30);run(5);
    require(world.get(31,180,30)==B(8) && world.getData(31,180,30)==0 && countIn(30,180,30,325)==1,
            "the water bucket empties a source in front");
    world.useBlock(29,180,30);run(2);world.useBlock(29,180,30);run(5);
    require(countIn(30,180,30,326)==1 && !(world.get(31,180,30)==B(8) && world.getData(31,180,30)==0),
            "the empty bucket picks the source up");
    // Arrows need projectiles, which are not ported: the dispenser clicks and keeps them.
    require(world.set(36,180,30,B(23)),"a third dispenser");world.setData(36,180,30,5);
    load(36,180,30,262,4);
    world.takeLevelEvents();
    require(world.placeBlock(35,180,30,B(69),0,{30.5,180,36.5},0,1),"its lever");
    world.useBlock(35,180,30);run(5);
    bool failed=false;for(const auto& e:world.takeLevelEvents())failed|=e[0]==1001;
    require(countIn(36,180,30,262)==4 && failed,"arrows stay in the dispenser with a failed click");
    // A spawn egg spawns its mob in front.
    require(world.set(42,180,24,B(23)),"a fourth dispenser");world.setData(42,180,24,5);
    load(42,180,24,383,1,90);
    const auto mobs=world.entities().size();
    require(world.placeBlock(41,180,24,B(69),0,{30.5,180,36.5},0,1),"its lever");
    world.useBlock(41,180,24);run(5);
    require(world.entities().size()==mobs+1 && world.entities().back().id==L"Pig" && countIn(42,180,24,383)==0,
            "a spawn egg spawns a pig in front");
    // Breaking a dispenser drops what it holds (DispenserTile::onRemove).
    const auto before=world.droppedItems().size();
    require(world.breakBlock(24,180,24),"break the first dispenser");
    int cobble=0;for(std::size_t i=before;i<world.droppedItems().size();++i)
        if(world.droppedItems()[i].id==4)cobble+=world.droppedItems()[i].count;
    require(cobble==1,"its cobblestone drops");
    require(world.set(24,180,24,B(23)) && countIn(24,180,24,4)==0,"a new dispenser there is empty");
}

// The source's animals in the World: breeding, shearing, milking, a hit,
// and a calf surviving a save.
static void worldAnimals(const std::filesystem::path& scratch){
    World world;world.generate(61,true);
    for(int x=16;x<=48;++x)for(int z=16;z<=48;++z){
        world.set(x,179,z,Grass);
        for(int y=180;y<186;++y)world.set(x,y,z,Air);
    }
    world.setSurvival(true);
    world.setPlayerPosition({32.5,180,20.5});
    auto aimAt=[&](const SimulatedEntity& e){return console::Vec3{e.position.x,e.position.y+.5,e.position.z-2};};
    auto find=[&](const wchar_t* id,int nth=0)->const SimulatedEntity*{
        int n=0;for(const auto& e:world.entities())if(e.id==id && e.health>0 && n++==nth)return &e;
        return nullptr;
    };
    auto carry=[&](int id,int count){
        world.addCarriedItem(id,count);
        for(int i=0;i<36;++i)if(world.carriedItems()[i].id==id){
            if(i>=9)world.swapCarriedSlots(i,0);
            return i<9?i:0;
        }
        return -1;
    };
    require(world.spawnCreativeEgg(92,{30.5,180,30.5}) && world.spawnCreativeEgg(92,{31.5,180,30.5}),"two cows");
    for(int i=0;i<3;++i)world.tickTime();
    require(find(L"Cow") && find(L"Cow")->ai,"a cow runs the source's mob");
    // Wheat puts both in love (Animal::interact) and they breed.
    int wheat=carry(296,4);
    for(int n=0;n<2;++n){
        const auto* cow=find(L"Cow",n);
        require(world.useEntity(aimAt(*cow),{0,0,1},wheat),"feed a cow");
    }
    require(world.carriedItems()[wheat].count==2,"feeding uses wheat");
    int calves=0;
    for(int i=0;i<300 && !calves;++i){world.tickTime();for(const auto& e:world.entities())calves+=e.id==L"Cow" && e.baby;}
    require(calves==1,"the cows breed a calf");
    // Milk: an empty bucket becomes a milk bucket (Cow::interact).
    world.consumeCarried(wheat,2);
    int bucket=carry(325,1);
    require(world.useEntity(aimAt(*find(L"Cow")),{0,0,1},bucket),"milk a cow");
    bool milk=false;for(const auto& item:world.carriedItems())milk|=item.id==335;
    require(milk,"the bucket fills with milk");
    // Shears: wool drops and the sheep is bare (Sheep::interact).
    require(world.spawnCreativeEgg(91,{36.5,180,36.5}),"a sheep");
    world.tickTime();
    const auto dropsBefore=world.droppedItems().size();
    int shears=carry(359,1);
    const auto* sheep=find(L"Sheep");
    require(sheep && !sheep->baby,"an adult sheep");
    require(world.useEntity(aimAt(*sheep),{0,0,1},shears),"shear a sheep");
    bool wool=false;for(std::size_t i=dropsBefore;i<world.droppedItems().size();++i)wool|=world.droppedItems()[i].id==35;
    require(wool && find(L"Sheep")->sheared && world.carriedItems()[shears].damage==1,"the sheep drops wool and the shears wear");
    // A saddle goes on a pig and red dye on a sheep (Player::interact ->
    // SaddleItem/DyePowderItem::interactEnemy).
    require(world.spawnCreativeEgg(90,{40.5,180,30.5}) && world.spawnCreativeEgg(91,{44.5,180,30.5}),"a pig and a sheep");
    world.tickTime();
    int saddle=carry(329,1);
    require(world.useEntity(aimAt(*find(L"Pig")),{0,0,1},saddle) && find(L"Pig")->saddled,"a saddle on a pig");
    world.addCarriedItem(351,1,1);
    int dye=-1;for(int i=0;i<36;++i)if(world.carriedItems()[i].id==351){if(i>=9)world.swapCarriedSlots(i,1);dye=i<9?i:1;break;}
    const SimulatedEntity* woolly=nullptr;
    for(const auto& e:world.entities())if(e.id==L"Sheep" && !e.sheared)woolly=&e;
    require(woolly && world.useEntity(aimAt(*woolly),{0,0,1},dye),"dye a sheep");
    bool red=false;for(const auto& e:world.entities())red|=e.id==L"Sheep" && e.woolColor==14;
    require(red,"the sheep's wool is red");
    // A hit knocks a cow back (Mob::hurt).
    auto cowHealth=[&]{int total=0;for(const auto& e:world.entities())if(e.id==L"Cow")total+=e.health;return total;};
    const int health=cowHealth();
    require(world.attackEntity(aimAt(*find(L"Cow")),{0,0,1},0),"hit a cow");
    require(cowHealth()<health,"the hit hurts it");
    // A save keeps the calf a calf.
    const auto path=scratch/"console_animals_test.pck";
    world.save(path);
    World reload;require(reload.load(path),"reload the animals");
    reload.setPlayerPosition({32.5,180,20.5});
    reload.tickTime();
    int babies=0;for(const auto& e:reload.entities())babies+=e.id==L"Cow" && e.baby && e.ai;
    require(babies==1,"the calf is still a calf after a reload");
    std::filesystem::remove(path);
}

static void worldPass(const std::filesystem::path& scratch){
    // A generated world: random ticks run without errors, and report their cost.
    World world;
    world.generate(12345);
    console::Vec3 spawn=world.spawn();
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
    console::Vec3 at=world.spawn();
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
    redstone();
    pistons();
    entities();
    mobs();
    animals();
    tnt();
    dispensers();
    worldUpdates(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path());
    worldAnimals(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path());
    worldPass(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path());
    std::cout<<"tile tick tests passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
