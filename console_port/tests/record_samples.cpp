#include "ChunkRecord.h"
#include <iostream>
static std::uint64_t hashBytes(const unsigned char* bytes,unsigned size){std::uint64_t h=14695981039346656037ull;for(unsigned i=0;i<size;++i)h=(h^bytes[i])*1099511628211ull;return h;}
int main(){
    for(int fixture=0;fixture<4;++fixture){
        console::ChunkRecord record;record.x=-27+fixture;record.z=26-fixture;record.lastUpdate=9876543210ll+fixture;record.terrainPopulated=fixture*30;
        for(int i=0;i<256;++i){record.heightValues[i]=(i+fixture)%256;record.biomeValues[i]=(i+fixture)%23;}
        for(int i=0;i<32;++i){int x=i%16,y=(i*7)%128,z=(i*3)%16;record.lowerBlocks->set(x,y,z,1+i%200);record.upperBlocks->set(x,y,z,24+i%100);record.lowerData->set(x,y,z,i%16);record.upperData->set(x,y,z,15-i%16);record.lowerSkyLight->set(x,y,z,i%16);record.upperBlockLight->set(x,y,z,i%16);}
        auto* ticks=new ListTag<CompoundTag>();auto* tick=new CompoundTag;tick->putInt(L"i",12);tick->putInt(L"x",record.x*16);tick->putInt(L"y",70);tick->putInt(L"z",record.z*16);tick->putInt(L"t",3);ticks->add(tick);record.extra->put(L"TileTicks",ticks);
        record.extra->put(L"Entities",new ListTag<CompoundTag>());record.extra->put(L"TileEntities",new ListTag<CompoundTag>());
        ByteArrayOutputStream bytes;DataOutputStream out(&bytes);record.write(&out);std::cout<<fixture<<' '<<bytes.size()<<' '<<hashBytes(bytes.buf.data,bytes.size())<<'\n';
        CompressedTileStorage::tick();SparseDataStorage::tick();SparseLightStorage::tick();
    }
    for(int i=0;i<3;++i){CompressedTileStorage::tick();SparseDataStorage::tick();SparseLightStorage::tick();}
}
