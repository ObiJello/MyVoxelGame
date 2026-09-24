#include "SpringFeature.h"
#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include "Mth.h"
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}

int main(){
    Mth::init();
    for(auto seed:{0LL,1LL,-1LL,8675309LL}){
        for(int fixture=0;fixture<9;++fixture){
            console::GenerationRegion region;
            for(int cx=-3;cx<=2;++cx)
                for(int cz=-2;cz<=2;++cz)
                    region.insert(cx,cz,std::make_unique<console::ChunkStorage>());
            Level level(seed);level.setBlockAccess(region);
            Random random(seed);
            const int x=fixture==8?-17:7,y=64,z=fixture==8?-1:7;
            auto block=[&](int dx,int dy,int dz,int tile){
                region.setTileAndData(x+dx,y+dy,z+dz,tile,0);
            };
            block(0,1,0,1);block(0,-1,0,1);
            block(-1,0,0,1);block(0,0,-1,1);block(0,0,1,1);
            if(fixture==1)block(0,0,0,1);
            if(fixture==2)block(0,1,0,3);
            if(fixture==3)block(0,-1,0,3);
            if(fixture==4)block(0,0,0,3);
            if(fixture==5)block(0,0,1,0);
            if(fixture==6)block(1,0,0,1);
            if(fixture==7)block(0,0,1,3);
            const int tile=fixture==1?Tile::lava_Id:Tile::water_Id;
            int ticks=0;
            level.setGenerationLiquidTickHandler([&](int tickTile,Level& tickLevel,int tx,int ty,int tz,Random& tickRandom){
                require(tickTile==tile && tx==x && ty==y && tz==z,"Spring tick coordinates changed");
                require(&tickRandom==&random && tickLevel.getInstaTick(),"Spring tick must be immediate");
                ++ticks;
            });
            SpringFeature feature(tile);
            bool accepted=feature.place(&level,&random,x,y,z);
            const bool shouldPlace=fixture==0 || fixture==1 || fixture==8;
            const bool shouldAccept=fixture!=2 && fixture!=3 && fixture!=4;
            require(accepted==shouldAccept,"Spring validation changed");
            require(ticks==int(shouldPlace),"Spring tick count changed");
            require(level.getTile(x,y,z)==(shouldPlace?tile:fixture==1?1:fixture==4?3:0),
                    "Spring source placement changed");
            require(!level.getInstaTick(),"Spring left instant ticking enabled");
            std::cout<<seed<<' '<<fixture<<' '<<accepted<<' '<<level.getTile(x,y,z)
                     <<' '<<ticks<<' '<<random.nextLong()<<'\n';
        }
    }
}
