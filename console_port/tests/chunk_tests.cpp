#include "ChunkGenerator.h"
#include <future>
#include <iostream>
#include <stdexcept>
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    try {
        console::ChunkGenerator generator(8675309);
        auto origin=generator.generate(0,0);
        generator.generate(-1,1);generator.generate(26,-27);
        check(origin.blocks==generator.generate(0,0).blocks,"Chunk depends on generation order");
        auto work=[&]{return generator.generate(0,0).blocks;};
        auto a=std::async(std::launch::async,work),b=std::async(std::launch::async,work);
        check(a.get()==origin.blocks && b.get()==origin.blocks,"Concurrent chunk generation differs");
        auto edge=generator.generate(-27,0,console::GenerationStage::Density);
        for(int z=0;z<16;++z)for(int y=0;y<63;++y)
            check(edge.blocks[z*128+y]==(y<=53?1:9),"Console finite-world edge profile differs");
        auto surface=generator.generate(0,0,console::GenerationStage::Surface);
        for(int i=0;i<256;++i)for(int y=0;y<2;++y)check(surface.blocks[i*128+y]==7,"Console bedrock floor missing");
        bool carved=false;
        for(int x=-2;x<=2 && !carved;++x){auto raw=generator.generate(x,0,console::GenerationStage::Surface);auto caves=generator.generate(x,0);carved=raw.blocks!=caves.blocks;}
        check(carved,"Carver stage never changed terrain");
        std::cout<<"Passed chunk determinism/order/concurrency, finite-world coast, bedrock and carver checks\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
