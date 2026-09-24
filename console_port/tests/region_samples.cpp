#ifdef CONSOLE_REGION_REFERENCE
#include "OriginalRegion.h"
#else
#include "ConsoleRegionFile.h"
#endif
#include <iostream>
int main(){
#ifdef CONSOLE_REGION_REFERENCE
    OriginalRegion region;
#else
    std::vector<unsigned char> headers(8192);
    auto region=console::ConsoleRegionFile::read(headers,SAVE_FILE_PLATFORM_WIN64);
#endif
    // Fragment, grow, shrink, overwrite, and fill previous holes. Deterministic
    // incompressible inputs make these exercise multiple sector allocations.
    std::uint32_t rng=0x12564389;
    for(unsigned turn=0;turn<100;++turn){
        unsigned size=turn%5==0?100:turn%5==1?16000:turn%5==2?4300:turn%5==3?8000:27000;
        std::vector<unsigned char> payload(size);for(auto& b:payload){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;b=static_cast<unsigned char>(rng);}
        unsigned slot=(turn*13)%11;region.put(slot,slot*3%32,payload,123456789+turn);
        auto bytes=region.serialize();std::uint64_t hash=14695981039346656037ull;for(auto b:bytes)hash=(hash^b)*1099511628211ull;
        std::cout<<turn<<' '<<bytes.size()<<' '<<region.takeSizeDelta()<<' '<<hash<<'\n';
    }
}
