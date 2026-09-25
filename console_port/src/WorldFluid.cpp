// Pending tile ticks: ServerLevel::addToTickNextTick / tickPendingTicks over
// the ScheduledTickQueue, and the chunk's saved TileTicks. The liquids
// themselves are the original LiquidTile methods (ported/tick).
#include "WorldState.h"
#include "TileTickHost.h"
#include <algorithm>
#include <limits>

namespace console {
void World::scheduleFluid(int x,int y,int z,int delay){
    if(!inside(x,y,z))return;
    const int id=get(x,y,z);
    if(id!=8 && id!=10)return;
    tileTicks().addToTickNextTick(x-width/2,y,z-depth/2,id,delay);
}
// A flowing liquid with no saved tick would never move again (world
// generation leaves them without one), so every visible chunk's flowing
// liquids are scheduled when it becomes visible.
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
void World::tickPendingTicks(){
    if(state->pending)return;
    if(state->lightDirty)state->ensureLighting(seed);
    tileTicks().tickPendingTicks(false);
}
void World::saveTileTicks(ChunkRecord& record,bool remove){
    if(!record.extra)record.extra=std::make_unique<CompoundTag>();
    auto list=std::make_unique<TagList>();
    // What the record still holds is either the ticks of tiles the port does
    // not run or a chunk whose ticks were never loaded: both stay as saved.
    if(auto* existing=dynamic_cast<TagList*>(record.extra->get(L"TileTicks")))
        for(int i=0;i<existing->size();++i)if(auto* tag=dynamic_cast<CompoundTag*>(existing->get(i))){
            std::unique_ptr<Tag> copy(tag->copy());list->add(copy.get());copy.release();
        }
    for(const auto& td:tileTicks().fetchTicksInChunk(record.x,record.z,remove)){
        auto tag=std::make_unique<CompoundTag>();
        tag->putInt(L"i",td.tileId);tag->putInt(L"x",td.x);tag->putInt(L"y",td.y);tag->putInt(L"z",td.z);
        const auto delay=std::clamp(td.m_delay-time(),std::int64_t(std::numeric_limits<int>::min()),
                                    std::int64_t(std::numeric_limits<int>::max()));
        tag->putInt(L"t",int(delay));list->add(tag.get());tag.release();
    }
    record.extra->put(L"TileTicks",list.get());list.release();
}
void World::loadTileTicks(ChunkRecord& record){
    if(!record.extra)return;
    auto* list=dynamic_cast<TagList*>(record.extra->get(L"TileTicks"));if(!list)return;
    std::vector<SavedTileTick> ticks;
    auto kept=std::make_unique<TagList>();
    for(int i=0;i<list->size();++i){
        auto* tag=dynamic_cast<CompoundTag*>(list->get(i));if(!tag)continue;
        const int id=tag->getInt(L"i"),x=tag->getInt(L"x"),y=tag->getInt(L"y"),z=tag->getInt(L"z");
        const bool here=x>=record.x*16 && x<record.x*16+16 && z>=record.z*16 && z<record.z*16+16 && y>=0 && y<height;
        if(here && sim::tickPorted(id))ticks.push_back({x,y,z,id,tag->getInt(L"t")});
        else{std::unique_ptr<Tag> copy(tag->copy());kept->add(copy.get());copy.release();}
    }
    if(ticks.empty())return;
    // The queue now owns these; the record keeps only the others.
    tileTicks().forceAddTileTicks(ticks);
    record.extra->put(L"TileTicks",kept.get());kept.release();
}
}
