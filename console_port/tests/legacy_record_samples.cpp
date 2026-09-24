#include "ChunkRecord.h"
#include <iostream>
#ifdef CONSOLE_LEGACY_RECORD_REFERENCE
void originalWriteLegacy(console::ChunkRecord*,DataOutputStream*);
#endif
int main(){
    for(unsigned fixture=0;fixture<6;++fixture){
        console::ChunkRecord record;record.x=-40+fixture;record.z=17-fixture;record.lastUpdate=(1ll<<45)+fixture;record.terrainPopulated=fixture*47;
        for(unsigned i=0;i<256;++i){record.heightValues[i]=i+fixture;record.biomeValues[i]=(i+fixture)%23;}
        for(int i=0;i<80;++i){int x=i%16,y=(i*7)%128,z=(i*3)%16;record.lowerBlocks->set(x,y,z,(i+fixture)%254);record.upperBlocks->set(x,y,z,(i+20+fixture)%254);record.lowerData->set(x,y,z,(i+fixture)%16);record.upperSkyLight->set(x,y,z,(i+fixture)%16);record.lowerBlockLight->set(x,y,z,(i*3+fixture)%16);}
        ByteArrayOutputStream bytes;DataOutputStream output(&bytes);
#ifdef CONSOLE_LEGACY_RECORD_REFERENCE
        originalWriteLegacy(&record,&output);
#else
        record.writeLegacy(&output);
#endif
        std::uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<bytes.size();++i)hash=(hash^bytes.buf.data[i])*1099511628211ull;
        std::cout<<fixture<<' '<<bytes.size()<<' '<<hash<<'\n';
    }
}
