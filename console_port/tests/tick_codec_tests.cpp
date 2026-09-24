#include "ScheduledTickCodec.h"
#include "PS3WorldStorage.h"
#include "TickQueueFixture.h"
#include <algorithm>
#include <iostream>
#include <limits>
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}catch(const std::overflow_error&){return;}catch(const std::out_of_range&){return;}throw std::runtime_error("Expected invalid tick save rejection");}
static std::unique_ptr<CompoundTag> entry(int x,int y,int z,int id,int delay){auto tag=std::make_unique<CompoundTag>();tag->putInt(L"x",x);tag->putInt(L"y",y);tag->putInt(L"z",z);tag->putInt(L"i",id);tag->putInt(L"t",delay);return tag;}
static void append(TagList& list,std::unique_ptr<CompoundTag> tag){list.add(tag.get());tag.release();}
int main(){try{
    using namespace console;
    for(bool legacy:{false,true})for(int dimension:{-1,0,1}){
        TickQueueFixture source;ScheduledTickQueue queue(source);ChunkRecord record;record.x=-1;record.z=1;
        record.extra->putString(L"Unknown",L"retained");
        for(int i=0;i<3;++i){int x=-16+i,y=64,z=16;record.lowerBlocks->set(x&15,y,z&15,8);queue.forceAddTileTick(x,y,z,8,i==0?2:10);}
        queue.forceAddTileTick(0,64,16,8,3);queue.setTimeAndAdjustTileTicks(500);
        ScheduledTickCodec::saveChunk(record,queue);require(queue.size()==4,"Saving a chunk does not consume pending ticks");
        PS3WorldStorage fresh;auto archive=fresh.serialize();if(legacy)archive[9]=2;
        auto saved=PS3WorldStorage::read(archive);saved->putChunk(dimension,record);auto reopened=PS3WorldStorage::read(saved->serialize());auto loaded=reopened->chunk(dimension,-1,1);
        auto expected=ScheduledTickCodec::read(*loaded->extra,-1,1);require(expected.size()==3 && loaded->extra->getString(L"Unknown")==L"retained","Both record families preserve chunk ticks and unrelated NBT");
        std::stable_sort(expected.begin(),expected.end(),[](auto a,auto b){return a.delay<b.delay;});
        TickQueueFixture destination;destination.time=1000;destination.loaded=false;ScheduledTickQueue restored(destination);
        ScheduledTickCodec::loadChunk(*loaded,restored);ScheduledTickCodec::loadChunk(*loaded,restored);
        require(restored.size()==3 && destination.checks==0 && destination.executed.empty(),"Loading batches bypasses neighbor checks, suppresses duplicates and executes nothing");
        for(const auto& td:expected)destination.tiles[{td.x,td.y,td.z}]=loaded->lowerBlocks->get(td.x&15,td.y,td.z&15);
        destination.loaded=true;require(restored.tickPendingTicks(false) && destination.executed.empty(),"Loaded delays are relative to the new world clock");
        destination.time=1002;require(restored.tickPendingTicks(false) && destination.executed.size()==1,"First restored update runs at its original relative delay");
        destination.time=1010;require(!restored.tickPendingTicks(false) && destination.executed.size()==3,"All restored updates execute at their due times");
        for(std::size_t i=0;i<expected.size();++i)require(destination.executed[i]==std::tuple{expected[i].tileId,expected[i].x,expected[i].y,expected[i].z},"Equal-delay reload order follows the serialized tick list");
    }
    TickQueueFixture host;ScheduledTickQueue queue(host);ChunkRecord record;
    queue.forceAddTileTick(0,64,0,8,50);
    auto list=std::make_unique<TagList>();append(*list,entry(0,64,0,8,0));append(*list,entry(1,64,0,8,5));append(*list,entry(1,64,0,8,1));record.extra->put(L"TileTicks",list.release());
    ScheduledTickCodec::loadChunk(record,queue);require(queue.size()==2,"Existing and within-batch duplicates retain the first schedule");
    auto snap=queue.snapshotTicksInChunk(0,0);for(const auto& td:snap.ticks)require(td.m_delay==(td.x==0?150:105),"Batch loading retains the correct winning deadlines");
    auto malformed=std::make_unique<TagList>();append(*malformed,entry(2,64,0,8,5));auto missing=entry(3,64,0,8,5);missing->remove(L"t");append(*malformed,std::move(missing));record.extra->put(L"TileTicks",malformed.release());
    rejects([&]{ScheduledTickCodec::loadChunk(record,queue);});require(queue.size()==2,"A malformed later entry cannot partially load the batch");
    for(int invalid=0;invalid<5;++invalid){
        auto tags=std::make_unique<TagList>();auto tag=entry(2,64,0,8,1);
        if(invalid==0)tag->putInt(L"x",16);
        if(invalid==1)tag->putInt(L"i",255);
        if(invalid==2)tag->putShort(L"t",1);
        if(invalid==3)tag->putInt(L"z",INT_MAX);
        if(invalid==4)tags->add(new IntTag(L"",2));else append(*tags,std::move(tag));
        record.extra->put(L"TileTicks",tags.release());rejects([&]{ScheduledTickCodec::loadChunk(record,queue);});require(queue.size()==2,"Invalid typed tick payload leaves existing schedules intact");
    }
    record.extra->putInt(L"TileTicks",1);rejects([&]{ScheduledTickCodec::loadChunk(record,queue);});record.extra->remove(L"TileTicks");ScheduledTickCodec::loadChunk(record,queue);require(queue.size()==2,"Missing TileTicks is an empty batch");
    ScheduledTickCodec::saveChunk(record,queue);std::unique_ptr<Tag> before(record.extra->copy());
    std::vector<TickNextTickData> far;far.emplace_back(0,64,0,8);far.back().delay(1ll<<40);rejects([&]{ScheduledTickCodec::write(*record.extra,far,0);});require(record.extra->equals(before.get()),"Unrepresentable saved delay preserves previous NBT");
    far.back().delay(INT32_MIN);ScheduledTickCodec::write(*record.extra,far,0);require(ScheduledTickCodec::read(*record.extra,0,0)[0].delay==INT32_MIN,"Minimum signed save delay is retained");
    far.back().delay(INT32_MAX);ScheduledTickCodec::write(*record.extra,far,0);require(ScheduledTickCodec::read(*record.extra,0,0)[0].delay==INT32_MAX,"Maximum signed save delay is retained");
    far.back().delay(INT64_MIN);rejects([&]{ScheduledTickCodec::write(*record.extra,far,INT64_MAX);});
    TickQueueFixture end;end.time=INT64_MAX-1;ScheduledTickQueue overflow(end);overflow.forceAddTileTick(7,64,0,8,0);
    std::vector<SavedTileTick> batch{{0,64,0,8,0},{1,64,0,8,2}};rejects([&]{overflow.forceAddTileTicks(batch);});require(overflow.size()==1,"Deadline overflow cannot partially commit a valid batch prefix");
    batch={{0,64,0,0,INT_MAX}};overflow.forceAddTileTicks(batch);for(auto td:overflow.snapshotTicksInChunk(0,0).ticks)if(td.tileId==0)require(td.m_delay==0,"Original air tick load ignores relative delay");
    std::cout<<"Tick NBT source layout, all-dimension legacy/current reload, due-time dispatch and transactional failure checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
