#include "CompressedTileStorage.h"
#include "ByteArrayInputStream.h"
#include "ByteArrayOutputStream.h"
#include <iostream>
#include <vector>
#include <memory>
static std::uint64_t checksum(const unsigned char* data,unsigned count){std::uint64_t h=14695981039346656037ull;for(unsigned i=0;i<count;++i)h=(h^data[i])*1099511628211ull;return h;}
static void changed(int,int,int,void* param,int extra){*static_cast<int*>(param)+=extra;}
int main(){
    CompressedTileStorage::staticCtor();
    for(int types:{1,2,3,4,5,16,17,64}){
        std::vector<unsigned char> blocks(32768),roundtrip(32768);
        for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<128;++y)blocks[(x*16+z)*128+y]=((x&3)*16+(z&3)*4+(y&3))%types;
        CompressedTileStorage storage(byteArray(blocks.data(),blocks.size()),0);storage.compress();
        storage.getData(byteArray(roundtrip.data(),roundtrip.size()),0);if(blocks!=roundtrip)return 1;
        int counts[5];int size=storage.getAllocatedSize(counts,counts+1,counts+2,counts+3,counts+4);
        ByteArrayOutputStream bytes;DataOutputStream output(&bytes);storage.write(&output);
        auto payload=bytes.toByteArray();ByteArrayInputStream input(payload);DataInputStream reader(&input);CompressedTileStorage loaded;loaded.read(&reader);
        loaded.getData(byteArray(roundtrip.data(),roundtrip.size()),0);if(blocks!=roundtrip)return 2;
        std::cout<<types<<' '<<size<<' '<<checksum(bytes.buf.data,bytes.size());for(auto count:counts)std::cout<<' '<<count;std::cout<<'\n';
        for(int i=0;i<80;++i)storage.set(i%16,(i*17)%128,(i*3)%16,100+i%100);
        unsigned char region[24];for(int i=0;i<24;++i)region[i]=200+i;
        int callbacks=0;int copied=storage.setDataRegion(byteArray(region,24),2,4,5,4,7,9,0,changed,&callbacks,3);
        storage.compress();storage.getData(byteArray(roundtrip.data(),roundtrip.size()),0);
        CompressedTileStorage duplicate(&storage);if(!storage.isSameAs(&duplicate))return 3;
        std::cout<<copied<<' '<<callbacks<<' '<<checksum(roundtrip.data(),roundtrip.size())<<' '<<storage.getHighestNonEmptyY()<<'\n';
        CompressedTileStorage::tick();CompressedTileStorage::tick();CompressedTileStorage::tick();
    }
}
