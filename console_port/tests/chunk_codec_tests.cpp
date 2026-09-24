#include "CompressedTileStorage.h"
#include "ByteArrayInputStream.h"
#include "ByteArrayOutputStream.h"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}catch(const std::out_of_range&){return;}throw std::runtime_error("Expected rejection");}
static std::vector<unsigned char> serialize(CompressedTileStorage& storage){ByteArrayOutputStream stream;DataOutputStream out(&stream);storage.write(&out);return {stream.buf.data,stream.buf.data+stream.size()};}
static void readBytes(CompressedTileStorage& storage,const std::vector<unsigned char>& bytes){
    ByteArrayInputStream input(byteArray(const_cast<unsigned char*>(bytes.data()),bytes.size()));
    struct Detach{ByteArrayInputStream& input;~Detach(){input.reset();}} detach{input};DataInputStream reader(&input);storage.read(&reader);
}
int main(){try{
    CompressedTileStorage::staticCtor();CompressedTileStorage vacant;
    require(serialize(vacant)==std::vector<unsigned char>({0,0,0,0}) && vacant.get(0,0,0)==0 && vacant.getHighestNonEmptyY()==-1,"Absent section sentinel");
    vacant.set(15,127,15,254);require(vacant.get(15,127,15)==254,"Lazy empty storage and maximum supported tile");
    std::vector<unsigned char> before=serialize(vacant);rejects([&]{vacant.set(16,0,0,1);});rejects([&]{vacant.set(0,128,0,1);});rejects([&]{vacant.set(0,0,0,255);});require(before==serialize(vacant),"Invalid mutations must preserve storage");
    for(std::size_t length=0;length<before.size();++length){std::vector<unsigned char> truncated(before.begin(),before.begin()+length);rejects([&]{readBytes(vacant,truncated);});}
    require(vacant.get(15,127,15)==254,"Failed reads must retain the previous palette");
    rejects([&]{readBytes(vacant,{255,255,255,255});});rejects([&]{readBytes(vacant,{0,0,0,1,0});});rejects([&]{readBytes(vacant,{0,1,0,0});});
    CompressedTileStorage empty(true);auto wire=serialize(empty);require(wire.size()==1028 && wire[0]==0 && wire[1]==0 && wire[2]==4 && wire[3]==0 && wire[4]==7 && wire[5]==0,"Wire size is big endian and palette indices are little endian");
    auto bad=wire;bad[4]=3;bad[5]=0;rejects([&]{readBytes(vacant,bad);}); // Raw block offset with no payload.
    bad=wire;bad[4]=7;bad[5]=255;rejects([&]{readBytes(vacant,bad);});
    readBytes(vacant,{0,0,0,0});require(vacant.get(15,127,15)==0 && serialize(vacant).size()==4,"Zero-size read clears old storage");
    std::vector<unsigned char> blocks(32768),out(32770,77);
    for(int i=0;i<32768;++i)blocks[i]=static_cast<unsigned char>((i*73+i/128)%255);
    CompressedTileStorage full(byteArray(blocks.data(),blocks.size()),0);full.compress();full.getData(byteArray(out.data(),out.size()),1);
    require(out.front()==77 && out.back()==77 && std::equal(blocks.begin(),blocks.end(),out.begin()+1),"Full-domain packing and offset boundaries");
    CompressedTileStorage replaced(true);replaced.setData(byteArray(blocks.data(),blocks.size()),0);
    replaced.getData(byteArray(out.data(),out.size()),1);require(std::equal(blocks.begin(),blocks.end(),out.begin()+1),"Bulk replacement of compressed storage");
    auto overlapping=serialize(replaced);
    // Every random-pattern block has data. Force the second block to alias the first.
    overlapping[6]=overlapping[4];overlapping[7]=overlapping[5];rejects([&]{readBytes(vacant,overlapping);});
    CompressedTileStorage reloaded;readBytes(reloaded,serialize(full));for(int i=0;i<32768;++i)require(reloaded.get(i/2048,i%128,(i/128)%16)==blocks[i],"Every block coordinate must round trip");
    CompressedTileStorage growing(true);int counts[5];
    for(int i=0;i<64;++i){growing.set((i>>4)&3,i&3,(i>>2)&3,i+1);for(int j=0;j<=i;++j)require(growing.get((j>>4)&3,j&3,(j>>2)&3)==j+1,"Palette upgrade must retain all earlier values");}
    growing.compress();growing.getAllocatedSize(counts,counts+1,counts+2,counts+3,counts+4);require(counts[4]==1 && counts[0]==511,"64-color block uses raw storage, air blocks use uniform indices");
    for(int i=0;i<64;++i)growing.set((i>>4)&3,i&3,(i>>2)&3,0);growing.compress();require(growing.getHighestNonEmptyY()==-1 && growing.isRenderChunkEmpty(0),"Recompression collapses cleared block to air");
    unsigned char region[]={4,5,6,7,8,9,10,11};require(growing.testSetDataRegion(byteArray(region,8),0,0,0,2,2,2,0),"Region change detection");
    require(growing.setDataRegion(byteArray(region,8),0,0,0,2,2,2,0,nullptr,nullptr,0)==8,"Region write count");
    require(!growing.testSetDataRegion(byteArray(region,8),0,0,0,2,2,2,0),"Unchanged region detection");
    unsigned char regionOut[8]{};growing.getDataRegion(byteArray(regionOut,8),0,0,0,2,2,2,0);require(std::equal(region,region+8,regionOut),"Region coordinate order");
    rejects([&]{growing.setDataRegion(byteArray(region,8),0,0,0,3,2,2,0,nullptr,nullptr,0);});rejects([&]{growing.get(0,-1,0);});rejects([&]{growing.compress(512);});
    require(growing.getDataRegion(byteArray(),0,0,0,0,0,0,0)==0,"Empty null-buffer regions are valid");
    std::atomic_bool failed=false;std::vector<std::thread> workers;
    for(int worker=0;worker<4;++worker)workers.emplace_back([&,worker]{for(int i=0;i<50;++i){growing.set(worker,100,worker,20+worker);if(growing.get(worker,100,worker)!=20+worker)failed=true;growing.compress();CompressedTileStorage::tick();}});
    for(auto& worker:workers)worker.join();require(!failed,"Concurrent read/write/compress and deferred frees");
    CompressedTileStorage::tick();CompressedTileStorage::tick();CompressedTileStorage::tick();
    std::cout<<"Original chunk palette, wire format, validation and concurrency passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
