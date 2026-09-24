// Original field layout: OldChunkStorage.cpp save/load TileTicks sections.
#include "ScheduledTickCodec.h"
#include "ChunkRecord.h"
#include <limits>
namespace console {
namespace {
int relativeDelay(std::int64_t due,std::int64_t time){
    const bool forward=due>=time;
    const std::uint64_t magnitude=forward?std::uint64_t(due)-std::uint64_t(time):std::uint64_t(time)-std::uint64_t(due);
    const auto limit=forward?std::uint64_t(INT32_MAX):std::uint64_t(INT32_MAX)+1;
    if(magnitude>limit)throw IoError("Pending tick delay does not fit the console save's signed 32-bit field");
    return static_cast<int>(forward?std::int64_t(magnitude):-std::int64_t(magnitude));
}
void validTick(int x,int y,int z,int id){
    for(int coordinate:{x,y,z})if(coordinate<INT32_MIN+8 || coordinate>INT32_MAX-8)throw IoError("Saved tick neighborhood overflows coordinates");
    if(id<0 || id>254)throw IoError("Invalid saved tile tick ID");
}
int requiredInt(CompoundTag& tag,const wchar_t* name){
    if(!tag.contains(name))throw IoError("Missing saved tile tick field");
    return tag.getInt(name);
}
}
void ScheduledTickCodec::write(CompoundTag& extra,std::span<const TickNextTickData> ticks,std::int64_t time){
    auto list=std::make_unique<ListTag<CompoundTag>>();
    for(const auto& td:ticks){
        validTick(td.x,td.y,td.z,td.tileId);
        const int delay=relativeDelay(td.m_delay,time);
        auto tag=std::make_unique<CompoundTag>();
        tag->putInt(L"i",td.tileId);tag->putInt(L"x",td.x);tag->putInt(L"y",td.y);tag->putInt(L"z",td.z);tag->putInt(L"t",delay);
        list->add(tag.get());tag.release();
    }
    extra.put(L"TileTicks",list.get());list.release();
}
std::vector<SavedTileTick> ScheduledTickCodec::read(CompoundTag& extra,int chunkX,int chunkZ){
    std::vector<SavedTileTick> result;
    if(!extra.contains(L"TileTicks"))return result;
    auto* list=extra.getList(L"TileTicks");result.reserve(list->size());
    const std::int64_t west=std::int64_t(chunkX)*16,north=std::int64_t(chunkZ)*16;
    for(int i=0;i<list->size();++i){
        auto* tag=dynamic_cast<CompoundTag*>(list->get(i));if(!tag)throw IoError("TileTicks must contain compounds");
        SavedTileTick tick{requiredInt(*tag,L"x"),requiredInt(*tag,L"y"),requiredInt(*tag,L"z"),requiredInt(*tag,L"i"),requiredInt(*tag,L"t")};
        validTick(tick.x,tick.y,tick.z,tick.tileId);
        if(tick.x<west || tick.x>=west+16 || tick.z<north || tick.z>=north+16)throw IoError("Saved tile tick belongs to another chunk");
        result.push_back(tick);
    }
    return result;
}
void ScheduledTickCodec::saveChunk(ChunkRecord& record,ScheduledTickQueue& queue){
    if(!record.extra)throw IoError("Chunk has no extra NBT storage");
    auto snapshot=queue.snapshotTicksInChunk(record.x,record.z);write(*record.extra,snapshot.ticks,snapshot.time);
}
void ScheduledTickCodec::loadChunk(ChunkRecord& record,ScheduledTickQueue& queue){
    if(!record.extra)throw IoError("Chunk has no extra NBT storage");
    auto batch=read(*record.extra,record.x,record.z);queue.forceAddTileTicks(batch);
}
}
