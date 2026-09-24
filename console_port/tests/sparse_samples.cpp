#include "SparseDataStorage.h"
#include "SparseLightStorage.h"
#include "ByteArrayOutputStream.h"
#include "ByteArrayInputStream.h"
#include <iostream>
#include <vector>
static std::uint64_t hashBytes(const unsigned char* bytes,unsigned size){std::uint64_t h=14695981039346656037ull;for(unsigned i=0;i<size;++i)h=(h^bytes[i])*1099511628211ull;return h;}
template<class T>static void sample(T& storage,int mode){
    std::vector<unsigned char> data(16384),result(16384);
    for(int i=0;i<16384;++i)data[i]=mode==0?0:mode==1?255:mode==2?((i%64)<32?0:255):static_cast<unsigned char>((i*37+i/64)%256);
    storage.setData(byteArray(data.data(),data.size()),0);storage.getData(byteArray(result.data(),result.size()),0);if(result!=data)throw std::runtime_error("Sparse array mismatch");
    for(int i=0;i<32;++i)storage.set(i%16,(i*7)%128,(i*3)%16,i%16);
    int count=storage.compress();ByteArrayOutputStream bytes;DataOutputStream output(&bytes);storage.write(&output);
    T copy(&storage);auto payload=bytes.toByteArray();ByteArrayInputStream input(payload);DataInputStream reader(&input);copy.read(&reader);
    storage.getData(byteArray(data.data(),data.size()),0);copy.getData(byteArray(result.data(),result.size()),0);if(data!=result)throw std::runtime_error("Sparse read mismatch");
    std::cout<<mode<<' '<<count<<' '<<hashBytes(bytes.buf.data,bytes.size())<<' '<<hashBytes(data.data(),data.size())<<'\n';
    unsigned char region[12]{};copy.getDataRegion(byteArray(region,12),1,3,2,3,7,5,0);std::cout<<hashBytes(region,12)<<'\n';
    T::tick();T::tick();T::tick();
}
int main(){try{SparseDataStorage::staticCtor();SparseLightStorage::staticCtor();for(int mode=0;mode<4;++mode){SparseDataStorage data;sample(data,mode);SparseLightStorage light(false);sample(light,mode);}
    SparseLightStorage sky(true,true);sky.set(0,0,0,0);sky.setAllBright();std::cout<<sky.compress()<<' '<<sky.get(0,0,0)<<'\n';
    SparseLightStorage::tick();SparseLightStorage::tick();SparseLightStorage::tick();
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
