#include "WorldState.h"
#include "FlowingFluidTick.h"
#include <algorithm>
#include <limits>
#include <tuple>

namespace console {
void World::scheduleFluid(int x,int y,int z,int delay){
    if(!inside(x,y,z))return;
    const int id=get(x,y,z);
    if(id!=8 && id!=10)return;
    auto key=std::tuple{x,y,z};
    const auto due=time()+delay;
    auto [it,added]=state->fluidTicks.try_emplace(key,due);
    if(!added && due<it->second)it->second=due;
}
void World::activateFluidChunks(){
    for(int cz=originZ()/16-4;cz<originZ()/16+4;++cz)
        for(int cx=originX()/16-4;cx<originX()/16+4;++cx)activateFluidChunk(cx,cz);
}
void World::activateFluidChunk(int chunkX,int chunkZ){
    const int left=chunkX*16+width/2,north=chunkZ*16+depth/2;
    if(left<originX() || left>=originX()+width ||
       north<originZ() || north>=originZ()+depth)return;
    for(int z=north;z<north+16;++z)
        for(int x=left;x<left+16;++x){
            const auto& chunk=state->chunk(x,z);
            const auto* column=chunk.blocks.data()+((x&15)*16+(z&15))*height;
            for(int y=0;y<height;++y)if(column[y]==8 || column[y]==10)
                scheduleFluid(x,y,z,column[y]==8?5:30);
        }
}
void World::tickFluids(){
    if(state->pending)return;
    struct Access final:FlowingFluidAccess {
        World& world;
        explicit Access(World& value):world(value){}
        bool inside(int x,int y,int z)const override{return world.inside(x,y,z);}
        int tile(int x,int y,int z)const override{return world.get(x,y,z);}
        int data(int x,int y,int z)const override{return world.getData(x,y,z);}
        bool blocksMotion(int x,int y,int z)const override{return solid(world.get(x,y,z));}
        void put(int x,int y,int z,int id,int data)override{
            if(!world.inside(x,y,z))return;
            const int old=world.get(x,y,z);
            if(old!=id)world.set(x,y,z,static_cast<Block>(id));
            if(id && world.getData(x,y,z)!=data)world.setData(x,y,z,data);
            if(id==8 || id==10)world.scheduleFluid(x,y,z,id==8?5:30);
            if(old!=id && id!=9 && id!=11)world.updateLiquidNeighbors(x,y,z);
        }
    } access(*this);
    // Source ServerLevel permits 1000 tile callbacks per tick. The desktop
    // adapter keeps a smaller budget while lighting and mesh updates run in
    // the same frame; due entries remain queued for later ticks.
    for(int processed=0;processed<64;++processed){
        auto due=state->fluidTicks.end();
        for(auto it=state->fluidTicks.begin();it!=state->fluidTicks.end();++it){
            const auto [x,y,z]=it->first;
            if(inside(x,y,z) && it->second<=time() &&
               (due==state->fluidTicks.end() || it->second<due->second))due=it;
        }
        if(due==state->fluidTicks.end())break;
        auto [x,y,z]=due->first;state->fluidTicks.erase(due);
        if(inside(x,y,z))tickFlowingFluid(access,state->fluidRandom,x,y,z);
    }
}
void World::saveFluidTicks(ChunkRecord& record,bool remove,bool keepSavedFluids){
    if(!record.extra)record.extra=std::make_unique<CompoundTag>();
    auto list=std::make_unique<TagList>();
    // Keep saved ticks for still-unported tile behaviors opaque and intact.
    if(auto* existing=dynamic_cast<TagList*>(record.extra->get(L"TileTicks")))
        for(int i=0;i<existing->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(existing->get(i)))
            if(keepSavedFluids || (tag->getInt(L"i")!=8 && tag->getInt(L"i")!=10)){
                std::unique_ptr<Tag> copy(tag->copy());list->add(copy.get());copy.release();
            }
    for(auto it=state->fluidTicks.begin();it!=state->fluidTicks.end();){
        const auto [x,y,z]=it->first;
        if((x-64>=record.x*16 && x-64<record.x*16+16) &&
           (z-64>=record.z*16 && z-64<record.z*16+16)){
            const int sourceX=x-width/2,sourceZ=z-depth/2;
            const int id=state->region.hasChunk(Mth::intFloorDiv(sourceX,16),Mth::intFloorDiv(sourceZ,16))
                ?state->region.getTile(sourceX,y,sourceZ):0;
            if(id==8 || id==10){
                auto tag=std::make_unique<CompoundTag>();
                tag->putInt(L"i",id);tag->putInt(L"x",x-64);tag->putInt(L"y",y);tag->putInt(L"z",z-64);
                const auto delay=std::clamp(it->second-time(),std::int64_t(std::numeric_limits<int>::min()),
                                            std::int64_t(std::numeric_limits<int>::max()));
                tag->putInt(L"t",int(delay));list->add(tag.get());tag.release();
            }
            if(remove){it=state->fluidTicks.erase(it);continue;}
        }
        ++it;
    }
    record.extra->put(L"TileTicks",list.get());list.release();
}
void World::loadFluidTicks(const ChunkRecord& record){
    if(!record.extra)return;
    auto* list=dynamic_cast<TagList*>(record.extra->get(L"TileTicks"));if(!list)return;
    for(int i=0;i<list->size();++i){
        auto* tag=dynamic_cast<CompoundTag*>(list->get(i));if(!tag)continue;
        const int id=tag->getInt(L"i"),x=tag->getInt(L"x")+64,y=tag->getInt(L"y"),z=tag->getInt(L"z")+64;
        if((id!=8 && id!=10) || !inside(x,y,z) || get(x,y,z)!=id)continue;
        state->fluidTicks.try_emplace(std::tuple{x,y,z},time()+tag->getInt(L"t"));
    }
}
}
