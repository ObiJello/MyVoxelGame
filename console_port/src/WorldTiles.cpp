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
// randValue = randValue * 3 + addend, with the console's int wrap.
int nextRand(int value,int addend){return static_cast<int>(static_cast<unsigned>(value)*3u+static_cast<unsigned>(addend));}
std::unique_ptr<CompoundTag> dropStack(int id,int count,int damage){
    auto tag=std::make_unique<CompoundTag>();
    tag->putShort(L"id",id);tag->putByte(L"Count",count);tag->putShort(L"Damage",damage);
    return tag;
}
}

// The live sim::Level over the client world. Coordinates are client
// coordinates; the region is read wherever a resident chunk exists (the
// source reads loaded neighbours), and written only inside the visible area.
class WorldTickLevel final:public sim::Level {
    World& world_;
    World::State& s_;
    sim::Dimension overworld_;
    int skyDarken_=0;
    int lx(int x)const{return x-World::width/2;}
    int lz(int z)const{return z-World::depth/2;}
    bool resident(int x,int z)const{
        return s_.region.hasChunk(Mth::intFloorDiv(lx(x),16),Mth::intFloorDiv(lz(z),16));
    }
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
        int light=s_.region.getLight(LightLayer::Sky,lx(x),y,lz(z))-dampen;
        return std::max(light,s_.region.getLight(LightLayer::Block,lx(x),y,lz(z)));
    }
public:
    WorldTickLevel(World& world,World::State& state):world_(world),s_(state){
        sim::initializeTiles();
        random=&s_.tickRandom;
        dimension=&overworld_;
        skyDarken_=consoleOldSkyDarken(world.dayTime(),world.rainLevel(),world.thunderLevel());
    }
    int getTile(int x,int y,int z)override{
        if(y<0 || y>=maxBuildHeight || !resident(x,z))return 0;
        return s_.region.getTile(lx(x),y,lz(z));
    }
    int getData(int x,int y,int z)override{
        if(y<0 || y>=maxBuildHeight || !resident(x,z))return 0;
        return s_.region.getData(lx(x),y,lz(z));
    }
    bool setTileAndData(int x,int y,int z,int tile,int data)override{
        if(!world_.inside(x,y,z) || !validBlock(static_cast<std::uint8_t>(tile)))return false;
        const bool changed=world_.set(x,y,z,static_cast<Block>(tile));
        return world_.setData(x,y,z,tile?data:0) || changed;
    }
    bool setTile(int x,int y,int z,int tile)override{return setTileAndData(x,y,z,tile,0);}
    bool setData(int x,int y,int z,int data)override{return world_.inside(x,y,z) && world_.setData(x,y,z,data);}
    // The port has no neighbour notifications yet, so NoUpdate stores the same way.
    bool setTileNoUpdate(int x,int y,int z,int tile)override{return setTile(x,y,z,tile);}
    bool setTileAndDataNoUpdate(int x,int y,int z,int tile,int data)override{return setTileAndData(x,y,z,tile,data);}
    bool setDataNoUpdate(int x,int y,int z,int data)override{return setData(x,y,z,data);}
    int getRawBrightness(int x,int y,int z)override{return raw(x,y,z,true,skyDarken_);}
    int getDaytimeRawBrightness(int x,int y,int z)override{return raw(x,y,z,false,0);}
    int getBrightness(LightLayer::variety layer,int x,int y,int z)override{
        if(y<0)y=0;
        if(y>=maxBuildHeight)y=maxBuildHeight-1;
        if(!resident(x,z))return int(layer);
        return s_.region.getLight(layer,lx(x),y,lz(z));
    }
    // LevelChunk::isSkyLit: at or above the heightmap.
    bool canSeeSky(int x,int y,int z)override{return resident(x,z) && y>=s_.region.getHeightmap(lx(x),lz(z));}
    int biome(int x,int z){
        if(!resident(x,z))return 1;
        const auto& chunk=*s_.region.chunks.at({Mth::intFloorDiv(lx(x),16),Mth::intFloorDiv(lz(z),16)});
        return chunk.biomes[(lz(z)&15)*16+(lx(x)&15)];
    }
    int topRainBlock(int x,int z){return resident(x,z)?s_.lightLevel->getTopRainBlock(lx(x),lz(z)):0;}
    bool isRainingAt(int x,int y,int z)override{
        if(!s_.metadata->isRaining())return false;
        if(!canSeeSky(x,y,z))return false;
        if(topRainBlock(x,z)>y)return false;
        const int id=biome(x,z);
        if(biomeHasSnow(id))return false;
        return biomeHasRain(id);
    }
    bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)override{
        if(y1<0 || y0>=maxBuildHeight)return false;
        for(int cx=Mth::intFloorDiv(lx(x0),16);cx<=Mth::intFloorDiv(lx(x1),16);++cx)
            for(int cz=Mth::intFloorDiv(lz(z0),16);cz<=Mth::intFloorDiv(lz(z1),16);++cz)
                if(!s_.region.hasChunk(cx,cz))return false;
        return true;
    }
    // Tile::spawnResources -> popResource: each drop at a random point inside
    // the tile.
    void spawnResources(int x,int y,int z,int tile,int data)override{
        if(!world_.inside(x,y,z))return;
        for(const auto& drop:consoleTileDrops(tile,data,0,*random)){
            const double xo=random->nextFloat()*.7+.15,yo=random->nextFloat()*.7+.15,zo=random->nextFloat()*.7+.15;
            world_.spawnDroppedItem({x+xo,y+yo,z+zo},{random->nextFloat()*.2-.1,.2,random->nextFloat()*.2-.1},
                                    dropStack(drop.id,drop.count,drop.damage),10);
        }
    }
    bool placeTree(TreeKind kind,int height,Random& treeRandom,int x,int y,int z)override{
        // The feature writes through the world's light Level, which relights.
        const bool placed=placeSaplingTree(*s_.lightLevel,static_cast<int>(kind),height,treeRandom,lx(x),y,lz(z));
        ++world_.revision;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
            s_.undecorated.erase({Mth::intFloorDiv(lx(x),16)+dx,Mth::intFloorDiv(lz(z),16)+dz});
        return placed;
    }
    // Tile::onRemove for the tile a change replaced (LevelChunk::setTileAndData).
    void removed(int x,int y,int z,int tile,int data){
        if(auto* t=sim::Tile::tiles[tile])t->onRemove(this,x,y,z,tile,data);
    }
    // ServerLevel::tickTiles and the update thread.
    void tickTiles();
    // Level::tickClientSideTiles without the cave sound itself.
    void tickClientSideTiles(int xo,int zo);
};

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
            const double dx=player.x-(x+.5),dy=player.y-(y+.5),dz=player.z-(z+.5);
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
        const int id=getTile(x,y,z);
        auto* tile=sim::Tile::tiles[id];
        if(!tile || !tile->isTicking())continue;
        // LiquidTileDynamic::tick is the port's flowing fluid step.
        if(id==sim::Tile::water_Id || id==sim::Tile::lava_Id)world_.flowFluid(x,y,z);
        else tile->tick(this,x,y,z,random);
    }
    s_.updateTiles.clear();

    // Level::buildAndPrepareChunksToPoll: the player's chunk, then rings out
    // to 9 (the console's interleaved order), in insertion order. Only chunks
    // inside the visible window tick.
    std::vector<std::pair<int,int>> poll;
    auto add=[&](int cx,int cz){
        const int left=cx*16,north=cz*16;
        if(left<world_.originX() || left+16>world_.originX()+World::width ||
           north<world_.originZ() || north+16>world_.originZ()+World::depth)return;
        if(std::find(poll.begin(),poll.end(),std::pair{cx,cz})==poll.end())poll.push_back({cx,cz});
    };
    if(s_.hasPlayerPosition){
        const int px=Mth::floor(s_.playerPosition.x/16),pz=Mth::floor(s_.playerPosition.z/16);
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
        if(random->nextInt(prob)==0 && s_.metadata->isRaining() && s_.metadata->isThundering()){
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
            if(s_.lightLevel->shouldFreeze(lx(x+xo),yy-1,lz(z+zo)))setTile(x+xo,yy-1,z+zo,sim::Tile::ice_Id);
            if(s_.metadata->isRaining() && s_.lightLevel->shouldSnow(lx(x+xo),yy,lz(z+zo)))setTile(x+xo,yy,z+zo,sim::Tile::topSnow_Id);
            if(s_.metadata->isRaining() && biomeHasRain(biome(x+xo,z+zo))){
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
        auto has=[&](int dx,int dz){return s_.region.hasChunk(Mth::intFloorDiv(lx((cx+dx)*16),16),Mth::intFloorDiv(lz((cz+dz)*16),16));};
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

bool World::useItemOn(int x,int y,int z,int face,int slot){
    if(state->pending || slot<0 || slot>=36 || !inside(x,y,z))return false;
    const auto item=carriedItems()[slot];
    if(!item.id)return false;
    sim::ItemInstance instance;
    instance.id=item.id;instance.count=item.count;instance.auxValue=item.damage;
    WorldTickLevel level(*this,*state);
    if(!sim::useItemOn(level,instance,x,y,z,face))return false;
    // ServerPlayerGameMode::useItemOn: creative keeps the stack.
    if(instance.count<item.count)consumeCarried(slot,item.count-instance.count);
    if(instance.damage>0)wearCarried(slot,instance.damage);
    return true;
}

void World::tileRemoved(int x,int y,int z,int tile,int data){
    if(state->pending || (tile!=17 && tile!=18))return;
    WorldTickLevel level(*this,*state);
    level.removed(x,y,z,tile,data);
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
