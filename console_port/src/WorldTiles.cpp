// ServerLevel::tickTiles, the update thread's tile selection (ServerLevel::
// runUpdate) and Level::tickWeather over the client world. The tile rules
// themselves are the original methods in ported/tick/TileTickRules.cpp.
#include "WorldState.h"
#include "TileTickHost.h"
#include "ConsoleLightmap.h"
#include "SurvivalRules.h"
#include "Biome.h"
#include "GenerationDecorator.h"
#include "CompoundTag.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace console {
namespace {
constexpr int TICKS_PER_DAY=24000;
// ServerLevel::MAX_UPDATES, Level::MAX_GRASS_TICKS / MAX_LAVA_TICKS.
constexpr int MAX_UPDATES=256,MAX_GRASS_TICKS=100,MAX_LAVA_TICKS=100;

// Biome::hasSnow / hasRain: setNoRain() biomes (desert, hell, sky, desert
// hills) never rain; the rest snow below temperature 0.15.
bool biomeRains(int id){return id!=2 && id!=8 && id!=9 && id!=17;}
bool biomeHasSnow(int id){return biomeRains(id) && Biome::biomes[id] && Biome::biomes[id]->getTemperature()<0.15f;}
bool biomeHasRain(int id){return !biomeHasSnow(id) && biomeRains(id);}
// Level::hasChunksAt over the resident region (level coordinates).
bool regionHasChunksAt(const GenerationRegion& region,int x0,int y0,int z0,int x1,int y1,int z1){
    if(y1<0 || y0>=World::height)return false;
    for(int cx=Mth::intFloorDiv(x0,16);cx<=Mth::intFloorDiv(x1,16);++cx)
        for(int cz=Mth::intFloorDiv(z0,16);cz<=Mth::intFloorDiv(z1,16);++cz)
            if(!region.hasChunk(cx,cz))return false;
    return true;
}
// ServerLevel::MAX_FALLING_TILE.
constexpr std::size_t MAX_FALLING_TILE=20;
// randValue = randValue * 3 + addend, with the console's int wrap.
int nextRand(int value,int addend){return static_cast<int>(static_cast<unsigned>(value)*3u+static_cast<unsigned>(addend));}
std::unique_ptr<CompoundTag> dropStack(int id,int count,int damage){
    auto tag=std::make_unique<CompoundTag>();
    tag->putShort(L"id",id);tag->putByte(L"Count",count);tag->putShort(L"Damage",damage);
    return tag;
}
}

// The live sim::Level over the client world, in level coordinates (client
// coordinates less half the window, which keeps chunks aligned). The region is
// read wherever a resident chunk exists (the source reads loaded neighbours),
// and written only inside the visible window.
class WorldTickLevel final:public sim::Level {
    static constexpr int half=World::width/2;
    World& world_;
    World::State& s_;
    sim::Dimension overworld_;
    sim::CurrentLevel current_{this};
    int skyDarken_=0;
    bool resident(int x,int z)const{return s_.region.hasChunk(Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16));}
    int raw(int x,int y,int z,bool propagate,int dampen){
        if(propagate){
            switch(getTile(x,y,z)){
            case sim::Tile::stoneSlabHalf_Id:case sim::Tile::woodSlabHalf_Id:case sim::Tile::farmland_Id:
            case sim::Tile::stairs_stone_Id:case sim::Tile::stairs_wood_Id:{
                int br=raw(x,y+1,z,false,dampen);
                br=std::max(br,raw(x+1,y,z,false,dampen));br=std::max(br,raw(x-1,y,z,false,dampen));
                br=std::max(br,raw(x,y,z+1,false,dampen));br=std::max(br,raw(x,y,z-1,false,dampen));
                return br;
            }
            default:break;
            }
        }
        if(y<0)return 0;
        if(y>=maxBuildHeight)y=maxBuildHeight-1;
        if(!resident(x,z))return 0; // EmptyLevelChunk
        // LevelChunk::getRawBrightness
        int light=s_.region.getLight(LightLayer::Sky,x,y,z)-dampen;
        return std::max(light,s_.region.getLight(LightLayer::Block,x,y,z));
    }
    // Entity::move's vertical clip for a falling tile's box (the source
    // ignores cubes it already overlaps): the furthest the feet can go.
    double clipFall(const Vec3& position,double dy)const{
        const auto at=[&](double d){return world_.collides({position.x,position.y-.49+d,position.z},.98,.98);};
        if(dy>=0 || at(0) || !at(dy))return dy;
        double free=0,blocked=dy;
        for(int i=0;i<40;++i){const double mid=(free+blocked)/2;(at(mid)?blocked:free)=mid;}
        return free;
    }
public:
    WorldTickLevel(World& world,World::State& state):world_(world),s_(state){
        sim::initializeTiles();
        random=&s_.tickRandom;
        dimension=&overworld_;
        chunkSourceXZSize=s_.metadata->getXZSize();
        skyDarken_=consoleOldSkyDarken(world.dayTime(),world.rainLevel(),world.thunderLevel());
    }
    int getTile(int x,int y,int z)override{
        if(y<0 || y>=maxBuildHeight || !resident(x,z))return 0;
        return s_.region.getTile(x,y,z);
    }
    int getData(int x,int y,int z)override{
        if(y<0 || y>=maxBuildHeight || !resident(x,z))return 0;
        return s_.region.getData(x,y,z);
    }
    // LevelChunk::setTileAndData: store, then the replaced tile's onRemove
    // and the new tile's onPlace.
    bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data)override{
        if(y<0 || y>=maxBuildHeight || !world_.inside(x+half,y,z+half) || !validBlock(static_cast<std::uint8_t>(tile)))return false;
        const int old=getTile(x,y,z),oldData=getData(x,y,z);
        if(old==tile && oldData==data)return false;
        world_.set(x+half,y,z+half,static_cast<Block>(tile));
        world_.setData(x+half,y,z+half,data);
        placed(x,y,z,old,oldData,tile);
        return true;
    }
    // The chunk hooks of a change already stored.
    void placed(int x,int y,int z,int old,int oldData,int tile){
        if(old!=0 && sim::Tile::tiles[old])sim::Tile::tiles[old]->onRemove(this,x,y,z,old,oldData);
        if(tile!=0 && sim::Tile::tiles[tile])sim::Tile::tiles[tile]->onPlace(this,x,y,z);
    }
    bool setDataNoUpdate(int x,int y,int z,int data)override{
        return world_.inside(x+half,y,z+half) && world_.setData(x+half,y,z+half,data);
    }
    // Chunk coordinates are the same in level and client space less 4.
    bool hasChunk(int chunkX,int chunkZ)override{return s_.region.hasChunk(chunkX,chunkZ);}
    int getRawBrightness(int x,int y,int z)override{return raw(x,y,z,true,skyDarken_);}
    int getDaytimeRawBrightness(int x,int y,int z)override{return raw(x,y,z,false,0);}
    int getBrightness(LightLayer::variety layer,int x,int y,int z)override{
        if(y<0)y=0;
        if(y>=maxBuildHeight)y=maxBuildHeight-1;
        if(!resident(x,z))return int(layer);
        return s_.region.getLight(layer,x,y,z);
    }
    // LevelChunk::isSkyLit: at or above the heightmap.
    bool canSeeSky(int x,int y,int z)override{return resident(x,z) && y>=s_.region.getHeightmap(x,z);}
    int biome(int x,int z){
        if(!resident(x,z))return 1;
        const auto& chunk=*s_.region.chunks.at({Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16)});
        return chunk.biomes[(z&15)*16+(x&15)];
    }
    int topRainBlock(int x,int z){return resident(x,z)?s_.lightLevel->getTopRainBlock(x,z):0;}
    // Level::isRaining / isThundering: the rain level above 0.2, the thunder
    // level above 0.9.
    bool isRaining()override{return world_.rainLevel()>.2f;}
    bool isThundering(){return world_.thunderLevel()>.9f;}
    bool isRainingAt(int x,int y,int z)override{
        if(!isRaining())return false;
        if(!canSeeSky(x,y,z))return false;
        if(topRainBlock(x,z)>y)return false;
        const int id=biome(x,z);
        if(biomeHasSnow(id))return false;
        return biomeHasRain(id);
    }
    // Biome::isHumid: downfall above 0.85.
    bool isHumidAt(int x,int,int z)override{
        const auto* b=Biome::biomes[biome(x,z)];
        return b && b->downfall>.85f;
    }
    bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)override{
        return regionHasChunksAt(s_.region,x0,y0,z0,x1,y1,z1);
    }
    void addToTickNextTick(int x,int y,int z,int tile,int delay)override{
        world_.tileTicks().addToTickNextTick(x,y,z,tile,delay);
    }
    // ServerLevel::newFallingTileAllowed and addEntity(FallingTile).
    bool newFallingTileAllowed()override{return s_.fallingBlocks.size()<MAX_FALLING_TILE;}
    void addEntity(std::shared_ptr<sim::FallingTile> e)override{
        FallingBlock block;
        block.position={e->x+half,e->y,e->z+half};
        block.tile=e->tile;block.data=e->data;
        s_.fallingBlocks.push_back(block);
    }
    // PlayerList::isTrackingTile: the chunk is in the player's view.
    bool isTrackingTile(int x,int y,int z)override{return world_.inside(x+half,y,z+half);}
    // Tile::spawnResources -> popResource: each drop at a random point inside
    // the tile.
    void spawnResources(int x,int y,int z,int tile,int data)override{
        if(!world_.inside(x+half,y,z+half))return;
        for(const auto& drop:consoleTileDrops(tile,data,0,*random)){
            const double xo=random->nextFloat()*.7+.15,yo=random->nextFloat()*.7+.15,zo=random->nextFloat()*.7+.15;
            world_.spawnDroppedItem({x+half+xo,y+yo,z+half+zo},{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},
                                    dropStack(drop.id,drop.count,drop.damage),10);
        }
    }
    bool placeTree(TreeKind kind,int height,Random& treeRandom,int x,int y,int z)override{
        // The feature writes through the world's light Level, which relights.
        const bool placed=placeSaplingTree(*s_.lightLevel,static_cast<int>(kind),height,treeRandom,x,y,z);
        ++world_.revision;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
            s_.undecorated.erase({Mth::intFloorDiv(x,16)+dx,Mth::intFloorDiv(z,16)+dz});
        return placed;
    }
    // FallingTile::tick; false once the entity is removed.
    bool tickFalling(FallingBlock& block);
    // ServerLevel::tickTiles and the update thread.
    void tickTiles();
    // Level::tickClientSideTiles without the cave sound itself.
    void tickClientSideTiles(int xo,int zo);
};

bool WorldTickLevel::tickFalling(FallingBlock& block){
    if(block.tile==0)return false;
    ++block.time;
    auto& p=block.position;
    const int xt=Mth::floor(p.x)-half,zt=Mth::floor(p.z)-half;
    // time 1 takes the tile out of the world (before the move here: the
    // source's move ignores the tile's own cube, which it overlaps).
    if(block.time==1){
        if(getTile(xt,Mth::floor(p.y),zt)==block.tile)setTile(xt,Mth::floor(p.y),zt,0);
        else return false;
    }
    block.velocity.y-=0.04f;
    const double dy=clipFall(p,block.velocity.y);
    const bool onGround=dy!=block.velocity.y && block.velocity.y<0;
    p.y+=dy;
    if(dy!=block.velocity.y)block.velocity.y=0;
    block.velocity.x*=0.98f;block.velocity.y*=0.98f;block.velocity.z*=0.98f;
    const int yt=Mth::floor(p.y);
    if(onGround){
        block.velocity.x*=0.7f;block.velocity.z*=0.7f;block.velocity.y*=-0.5f;
        if(getTile(xt,yt,zt)!=sim::Tile::pistonMovingPiece_Id){
            if(mayPlace(block.tile,xt,yt,zt,true,1,nullptr) && !sim::HeavyTile::isFree(this,xt,yt-1,zt) &&
               setTileAndData(xt,yt,zt,block.tile,block.data)){
                if(auto* heavy=dynamic_cast<sim::HeavyTile*>(sim::Tile::tiles[block.tile]))
                    heavy->onLand(this,xt,yt,zt,block.data);
            }else{
                // spawnAtLocation(ItemInstance(tile, 1, getSpawnResourcesAuxValue(data))):
                // AnvilTile keeps its damage (data >> 2).
                const int aux=block.tile==sim::Tile::anvil_Id?block.data>>2:0;
                world_.spawnDroppedItem(p,{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},dropStack(block.tile,1,aux),10);
            }
            return false;
        }
    }else if((block.time>20*5 && (yt<1 || yt>maxBuildHeight)) || block.time>20*30){
        world_.spawnDroppedItem(p,{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},dropStack(block.tile,1,0),10);
        return false;
    }
    return true;
}

void WorldTickLevel::tickClientSideTiles(int xo,int zo){
    if(s_.delayUntilNextMoodSound==0){
        s_.randValue=nextRand(s_.randValue,s_.addend);
        const int val=s_.randValue>>2;
        int x=val&15,z=(val>>8)&15;
        const int y=(val>>16)&genDepthMinusOne;
        const int id=getTile(xo+x,y,zo+z);
        x+=xo;z+=zo;
        if(id==0 && getDaytimeRawBrightness(x,y,z)<=random->nextInt(8) && getBrightness(LightLayer::Sky,x,y,z)<=0){
            // getNearestPlayer within 8 and more than 2 away: the ambient
            // cave sound (audio is not ported) and a new delay.
            const auto& player=s_.playerPosition;
            const double dx=player.x-half-(x+.5),dy=player.y-(y+.5),dz=player.z-half-(z+.5);
            const double d2=dx*dx+dy*dy+dz*dz;
            if(s_.hasPlayerPosition && d2<=8*8 && d2>2*2)
                s_.delayUntilNextMoodSound=random->nextInt(20*60*10)+20*60*5;
        }
    }
}

void WorldTickLevel::tickTiles(){
    // The tiles the update thread chose after the previous tick.
    for(const auto& [x,y,z]:s_.updateTiles){
        if(!hasChunksAt(x,y,z,x,y,z))continue;
        auto* tile=sim::Tile::tiles[getTile(x,y,z)];
        if(tile && tile->isTicking())tile->tick(this,x,y,z,random);
    }
    s_.updateTiles.clear();

    // Level::buildAndPrepareChunksToPoll: the player's chunk, then rings out
    // to 9 (the console's interleaved order), in insertion order. Only chunks
    // inside the visible window tick.
    std::vector<std::pair<int,int>> poll;
    auto add=[&](int cx,int cz){
        const int left=cx*16+half,north=cz*16+half;
        if(left<world_.originX() || left+16>world_.originX()+World::width ||
           north<world_.originZ() || north+16>world_.originZ()+World::depth)return;
        if(std::find(poll.begin(),poll.end(),std::pair{cx,cz})==poll.end())poll.push_back({cx,cz});
    };
    if(s_.hasPlayerPosition){
        const int px=Mth::floor((s_.playerPosition.x-half)/16),pz=Mth::floor((s_.playerPosition.z-half)/16);
        add(px,pz);
        for(int r=1;r<=9;++r)for(int l=0;l<r*2;++l){
            add(px-r+l,pz-r);add(px+r,pz-r+l);add(px+r-l,pz+r);add(px-r,pz+r-l);
        }
    }
    if(s_.delayUntilNextMoodSound>0)--s_.delayUntilNextMoodSound;

    const int prob=100000;
    for(const auto& [cx,cz]:poll){
        const int xo=cx*16,zo=cz*16;
        tickClientSideTiles(xo,zo);
        if(random->nextInt(prob)==0 && isRaining() && isThundering()){
            s_.randValue=nextRand(s_.randValue,s_.addend);
            const int val=s_.randValue>>2;
            const int x=xo+(val&15),z=zo+((val>>8)&15);
            const int y=topRainBlock(x,z);
            // LightningBolt: the flash. The bolt entity (fire, damage) is not ported.
            if(isRainingAt(x,y,z))s_.lightningTime=2;
        }
        if(random->nextInt(16)==0){
            s_.randValue=nextRand(s_.randValue,s_.addend);
            const int val=s_.randValue>>2;
            const int x=val&15,z=(val>>8)&15;
            const int yy=topRainBlock(x+xo,z+zo);
            if(s_.lightLevel->shouldFreeze(x+xo,yy-1,z+zo))setTile(x+xo,yy-1,z+zo,sim::Tile::ice_Id);
            if(isRaining() && s_.lightLevel->shouldSnow(x+xo,yy,z+zo))setTile(x+xo,yy,z+zo,sim::Tile::topSnow_Id);
            if(isRaining() && biomeHasRain(biome(x+xo,z+zo))){
                const int tile=getTile(x+xo,yy-1,z+zo);
                if(tile!=0 && sim::Tile::tiles[tile])sim::Tile::tiles[tile]->handleRain(this,x+xo,yy-1,z+zo);
            }
        }
        // checkLight: the port relights on every change; keep the draws.
        random->nextInt(16);random->nextInt(128);random->nextInt(16);
    }

    // ServerLevel::runUpdate, run here after the tick instead of on a thread:
    // 80 samples per polled chunk from a copy of randValue, kept away from
    // edges without a loaded neighbour, grass and lava limited, 256 at most.
    int threadRand=s_.randValue,grassTicks=0,lavaTicks=0;
    for(const auto& [cx,cz]:poll){
        auto has=[&](int dx,int dz){return s_.region.hasChunk(cx+dx,cz+dz);};
        int minx=0,maxx=15,minz=0,maxz=15;
        if(!has(0,0))continue;
        if(!has(1,0))maxx=11;
        if(!has(0,1))maxz=11;
        if(!has(-1,0))minx=4;
        if(!has(0,-1))minz=4;
        if(!has(1,1)){maxx=11;maxz=11;}
        if(!has(1,-1)){maxx=11;minz=4;}
        if(!has(-1,-1)){minx=4;minz=4;}
        if(!has(-1,1)){minx=4;maxz=11;}
        for(int j=0;j<80;++j){
            threadRand=nextRand(threadRand,s_.addend);
            const int val=threadRand>>2;
            const int x=val&15;
            if(x<minx || x>maxx)continue;
            const int z=(val>>8)&15;
            if(z<minz || z>maxz)continue;
            const int y=(val>>16)&(maxBuildHeight-1);
            const int id=getTile(cx*16+x,y,cz*16+z);
            if(int(s_.updateTiles.size())>=MAX_UPDATES)break;
            if((id==sim::Tile::grass_Id && grassTicks>=MAX_GRASS_TICKS) ||
               (id==sim::Tile::calmLava_Id && lavaTicks>=MAX_LAVA_TICKS))continue;
            auto* tile=sim::Tile::tiles[id];
            if(tile && tile->isTicking() && tile->shouldTileTick(this,cx*16+x,y,cz*16+z)){
                if(id==sim::Tile::grass_Id)++grassTicks;
                else if(id==sim::Tile::calmLava_Id)++lavaTicks;
                s_.updateTiles.push_back({cx*16+x,y,cz*16+z});
            }
        }
    }
}

void World::tickTiles(){
    if(state->pending || !state->hasPlayerPosition)return;
    if(state->lightDirty)state->ensureLighting(seed);
    if(state->delayUntilNextMoodSound<0)state->delayUntilNextMoodSound=state->tickRandom.nextInt(20*60*10);
    WorldTickLevel level(*this,*state);
    level.tickTiles();
}

void World::tickFallingBlocks(){
    if(state->pending || state->fallingBlocks.empty())return;
    WorldTickLevel level(*this,*state);
    auto& blocks=state->fallingBlocks;
    for(std::size_t i=0;i<blocks.size();){
        // Entities outside the window wait (their chunks are not ticked).
        const auto& p=blocks[i].position;
        if(!inside(Mth::floor(p.x),std::clamp(Mth::floor(p.y),0,height-1),Mth::floor(p.z))){++i;continue;}
        FallingBlock block=blocks[i];
        const bool alive=level.tickFalling(block);
        if(alive)blocks[i++]=block;
        else blocks.erase(blocks.begin()+std::ptrdiff_t(i));
    }
}
const std::vector<FallingBlock>& World::fallingBlocks()const{return state->fallingBlocks;}

bool World::useItemOn(int x,int y,int z,int face,int slot){
    if(state->pending || slot<0 || slot>=36 || !inside(x,y,z))return false;
    const auto item=carriedItems()[slot];
    if(!item.id)return false;
    sim::ItemInstance instance;
    instance.id=item.id;instance.count=item.count;instance.auxValue=item.damage;
    WorldTickLevel level(*this,*state);
    if(!sim::useItemOn(level,instance,x-width/2,y,z-depth/2,face))return false;
    // ServerPlayerGameMode::useItemOn: creative keeps the stack.
    if(instance.count<item.count)consumeCarried(slot,item.count-instance.count);
    if(instance.damage>0)wearCarried(slot,instance.damage);
    return true;
}

// Edits are allowed while chunks stream (the region stays valid), as the raw
// set/setData are; only the ticks wait.
bool World::setTileAndUpdate(int x,int y,int z,Block tile,int data){
    if(!inside(x,y,z))return false;
    WorldTickLevel level(*this,*state);
    return level.setTileAndData(x-width/2,y,z-depth/2,tile,data);
}

bool World::setDataAndUpdate(int x,int y,int z,int data){
    if(!inside(x,y,z))return false;
    WorldTickLevel level(*this,*state);
    const bool changed=level.setDataNoUpdate(x-width/2,y,z-depth/2,data);
    if(changed)level.tileUpdated(x-width/2,y,z-depth/2,get(x,y,z));
    return changed;
}

void World::tileStored(int x,int y,int z,int oldTile,int oldData){
    if(!inside(x,y,z))return;
    WorldTickLevel level(*this,*state);
    const int tile=get(x,y,z);
    level.placed(x-width/2,y,z-depth/2,oldTile,oldData,tile);
    level.tileUpdated(x-width/2,y,z-depth/2,tile);
}

ScheduledTickQueue& World::tileTicks(){
    state->tileTickHost.world=this;
    return state->tileTicks;
}

// ServerLevel's pending tile ticks over the region, in level coordinates.
std::int64_t World::State::TileTickHost::getTime()const{return state.metadata->getTime();}
void World::State::TileTickHost::setTime(std::int64_t time)noexcept{state.metadata->setTime(time);}
bool World::State::TileTickHost::hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1){
    return regionHasChunksAt(state.region,x0,y0,z0,x1,y1,z1);
}
int World::State::TileTickHost::getTile(int x,int y,int z){
    if(y<0 || y>=height || !state.region.hasChunk(Mth::intFloorDiv(x,16),Mth::intFloorDiv(z,16)))return 0;
    return state.region.getTile(x,y,z);
}
void World::State::TileTickHost::tickTile(int id,int x,int y,int z){
    if(!sim::tickPorted(id))return;
    // A chunk in the streaming halo is loaded but not ticked: the tick waits
    // for the window (its writes would be refused outside it).
    if(!world->inside(x+width/2,y,z+depth/2)){state.tileTicks.forceAddTileTick(x,y,z,id,20);return;}
    WorldTickLevel level(*world,state);
    sim::Tile::tiles[id]->tick(&level,x,y,z,level.random);
}

// Level::prepareWeather and Level::tickWeather.
void World::tickWeather(){
    auto& data=*state->metadata;
    auto& random=state->tickRandom;
    if(!state->weatherPrepared){
        state->weatherPrepared=true;
        if(data.isRaining()){state->rainLevel=1;if(data.isThundering())state->thunderLevel=1;}
    }
    if(state->lightningTime>0)--state->lightningTime;

    int thunderTime=data.getThunderTime();
    if(thunderTime<=0){
        if(data.isThundering())data.setThunderTime(random.nextInt(20*60*10)+20*60*3);
        else data.setThunderTime(random.nextInt(TICKS_PER_DAY*7)+TICKS_PER_DAY/2);
    }else{
        --thunderTime;
        data.setThunderTime(thunderTime);
        if(thunderTime<=0)data.setThundering(!data.isThundering());
    }

    int rainTime=data.getRainTime();
    if(rainTime<=0){
        if(data.isRaining())data.setRainTime(random.nextInt(TICKS_PER_DAY/2)+TICKS_PER_DAY/2);
        else data.setRainTime(random.nextInt(TICKS_PER_DAY*7)+TICKS_PER_DAY/2);
    }else{
        --rainTime;
        data.setRainTime(rainTime);
        if(rainTime<=0)data.setRaining(!data.isRaining());
    }

    state->oRainLevel=state->rainLevel;
    state->rainLevel+=data.isRaining()?0.01f:-0.01f;
    state->rainLevel=std::clamp(state->rainLevel,0.f,1.f);
    state->oThunderLevel=state->thunderLevel;
    state->thunderLevel+=data.isThundering()?0.01f:-0.01f;
    state->thunderLevel=std::clamp(state->thunderLevel,0.f,1.f);
}
}
