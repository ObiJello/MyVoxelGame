#include "SparseDataStorage.h"
#include "SparseLightStorage.h"
#include "ByteArrayInputStream.h"
#include "ByteArrayOutputStream.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}catch(const std::out_of_range&){return;}throw std::runtime_error("Expected rejection");}
template<class T>static auto save(T& storage){ByteArrayOutputStream bytes;DataOutputStream output(&bytes);storage.write(&output);return std::vector<unsigned char>(bytes.buf.data,bytes.buf.data+bytes.size());}
template<class T>static void load(T& storage,const std::vector<unsigned char>& bytes){ByteArrayInputStream input(byteArray(const_cast<unsigned char*>(bytes.data()),bytes.size()));struct Detach{ByteArrayInputStream& input;~Detach(){input.reset();}} detach{input};DataInputStream reader(&input);storage.read(&reader);}
template<class T>static void exercise(T& storage){
    std::vector<unsigned char> packed(16384),out(16386,99);for(int i=0;i<16384;++i)packed[i]=static_cast<unsigned char>(i*157+i/64);
    storage.setData(byteArray(packed.data(),packed.size()),0);storage.compress();storage.getData(byteArray(out.data(),out.size()),1);
    require(out.front()==99 && out.back()==99 && std::equal(packed.begin(),packed.end(),out.begin()+1),"Plane reorder must preserve every nibble and buffer guards");
    for(int index=0;index<32768;++index)require(storage.get(index/2048,index%128,(index/128)%16)==((packed[index/2]>>((index&1)*4))&15),"Sparse coordinate nibble order");
    auto original=save(storage);T copy(&storage);storage.set(15,127,15,0);require(save(copy)==original,"Deep copied planes must be independent");
    rejects([&]{storage.set(0,0,0,16);});rejects([&]{storage.get(-1,0,0);});rejects([&]{storage.setData(byteArray(),0);});
    for(unsigned length:{0u,1u,3u,4u,127u,131u,static_cast<unsigned>(original.size()-1)}){std::vector<unsigned char> shortInput(original.begin(),original.begin()+length);rejects([&]{load(storage,shortInput);});}
    require(storage.get(15,127,15)==0,"Failed reads retain prior data");rejects([&]{load(storage,{255,255,255,255});});rejects([&]{load(storage,{0,0,0,129});});
    auto alias=original;alias[5]=alias[4];rejects([&]{load(storage,alias);});
    auto invalid=original;invalid[4]=255;rejects([&]{load(storage,invalid);});
    unsigned char region[8]{};require(storage.getDataRegion(byteArray(region,8),0,1,0,2,5,2,0)==8,"Odd Y regions retain original paired transfer length");
    for(int x=0;x<2;++x)for(int z=0;z<2;++z)for(int pair=0;pair<2;++pair)require(region[(x*2+z)*2+pair]==(storage.get(x,pair*2,z)|(storage.get(x,pair*2+1,z)<<4)),"Odd Y transfer rounds its origin down as in the console");
    require(storage.getDataRegion(byteArray(),0,0,0,0,0,0,0)==0,"Empty region accepts null array");
    std::atomic_bool bad=false;std::vector<std::thread> workers;for(int x=0;x<4;++x)workers.emplace_back([&,x]{for(int i=0;i<100;++i){storage.set(x,80,x,x+1);storage.compress();T::tick();if(storage.get(x,80,x)!=x+1)bad=true;}});for(auto& worker:workers)worker.join();require(!bad,"Concurrent nibble mutation and reclamation");
    T::tick();T::tick();T::tick();
}
int main(){try{
    SparseDataStorage::staticCtor();SparseLightStorage::staticCtor();SparseDataStorage data;require(data.get(0,0,0)==0 && data.compress()==0,"Empty data collapses all planes");
    SparseLightStorage sky(true);require(sky.get(0,127,0)==15 && sky.get(0,126,0)==0 && sky.compress()==0,"Original lower sky section initialization");
    SparseLightStorage upper(true,true);require(upper.get(15,0,15)==15 && upper.get(0,127,0)==15,"Upper sky section starts fully bright");upper.set(0,60,0,3);require(upper.get(1,60,0)==15,"Expanding uniform sky plane must prefill 15");upper.setAllBright();require(upper.compress()==0 && upper.get(0,60,0)==15,"Full brightness collapses data planes");
    std::vector<unsigned char> bright(16384,255);data.setData(byteArray(bright.data(),bright.size()),0);require(data.compress()==128,"Metadata stores uniform nonzero planes");upper.setData(byteArray(bright.data(),bright.size()),0);require(upper.compress()==0,"Light storage has a uniform-15 sentinel");
    auto wire=save(upper);require(wire.size()==132 && wire[0]==0 && wire[3]==0 && wire[4]==129,"Sparse wire format has a big-endian plane count and byte indices");rejects([&]{load(data,wire);});
    exercise(data);SparseLightStorage block(false);exercise(block);
    unsigned char region[]={0x21,0x43};int calls=0;data.setDataRegion(byteArray(region,2),0,0,0,1,4,1,0,[](int,int,int,void* p,int n){*static_cast<int*>(p)+=n;},&calls,1);require(data.get(0,0,0)==1 && data.get(0,3,0)==4 && calls<=4,"Metadata callback and paired writes");
    block.setDataRegion(byteArray(region,2),0,0,0,1,4,1,0);require(block.get(0,1,0)==2 && block.get(0,2,0)==3,"Light paired writes");
    std::cout<<"Sparse metadata/light packing, validation, defaults and concurrency passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
