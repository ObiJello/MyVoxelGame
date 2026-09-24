#include "BiomeGenerator.h"
#include "LevelType.h"
#include <future>
#include <iostream>
#include <stdexcept>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    try{
        console::BiomeGenerator generator(8675309);
        const auto full=generator.area(-40,-30,83,71);
        for(int z=0;z<71;z+=13)for(int x=0;x<83;x+=17){
            int w=std::min(17,83-x),d=std::min(13,71-z);auto part=generator.area(x-40,z-30,w,d);
            for(int pz=0;pz<d;++pz)for(int px=0;px<w;++px)
                require(full[x+px+(z+pz)*83]==part[px+pz*w],"Biome tile seams depend on query partition");
        }
        auto worker=[&]{for(int i=0;i<8;++i)require(generator.area(-40,-30,83,71)==full,"Concurrent layer graph changed output");};
        auto a=std::async(std::launch::async,worker),b=std::async(std::launch::async,worker);a.get();b.get();
        for(int i=0;i<10;++i){console::BiomeGenerator repeat(8675309);require(repeat.area(-40,-30,83,71)==full,"Layer lifetime changes deterministic output");}
        for(auto size:{0,-1,2049}){bool rejected=false;try{generator.area(0,0,size,1);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"Invalid biome dimensions accepted");}
        require(LevelType::getLevelType(L"missing")==nullptr,"Missing level type should not dereference null slots");
        require(!LevelType::lvl_flat->hasReplacement(),"Level type replacement flag uninitialized");
        require(LevelType::lvl_normal->getReplacementForVersion(0)==LevelType::lvl_normal_1_1,"Legacy generator selection mismatch");
        std::cout<<"Passed biome query seams, repeatability, concurrent queries, dimensions and level types\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
