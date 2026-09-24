#include "NbtIo.h"
#include "TickNextTickData.h"
#include <iostream>
#ifdef CONSOLE_TICK_CODEC_REFERENCE
void originalWriteTileTicks(CompoundTag*,std::vector<TickNextTickData>*,std::int64_t);
#else
#include "ScheduledTickCodec.h"
#endif
int main(){
    for(std::int64_t time:{-(1ll<<40),0ll,1ll<<48})for(int count:{0,1,8,31}){
        std::vector<TickNextTickData> ticks;
        for(int i=0;i<count;++i){TickNextTickData td(-16+i%16,i*7,-1,i%2?8:55);td.delay(time+(i%3==0?-31:i*77));ticks.push_back(td);}
        CompoundTag root;root.putString(L"Unknown",L"preserved");
#ifdef CONSOLE_TICK_CODEC_REFERENCE
        originalWriteTileTicks(&root,&ticks,time);
#else
        console::ScheduledTickCodec::write(root,ticks,time);
#endif
        ByteArrayOutputStream bytes;DataOutputStream output(&bytes);NbtIo::write(&root,&output);
        std::uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<bytes.size();++i)hash=(hash^bytes.buf[i])*1099511628211ull;
        std::cout<<time<<' '<<count<<' '<<bytes.size()<<' '<<hash<<'\n';
    }
}
