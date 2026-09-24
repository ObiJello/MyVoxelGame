#include "ConsoleLightmap.h"
#include "render_light_fixture.h"
#include "liquid_surface_fixture.h"
#include "liquid_flow_fixture.h"
#include "Vec3.h"
#include <bit>
#include <iostream>
int main() {
    Vec3::CreateNewThreadStorage();
    int sample=0;
    for(int dimension:{-1,0,1})for(std::int64_t time:{-1ll,0ll,6000ll,12000ll,18000ll,24000ll})
    for(float rain:{0.f,.5f,1.f})for(bool lightning:{false,true})for(int vision:{-1,80,201}) {
        console::LightFlicker flicker;
        for(int tick=0;tick<23;++tick){std::array<double,8> values;for(int i=0;i<8;++i)values[i]=((sample*3+tick*17+i*7)%101)/101.0;flicker.tick(values);}
        console::LightmapInput input;input.dimension=dimension;input.skyDarken=console::consoleSkyDarken(time,rain,rain*.75f,.375f,dimension);
        input.flicker=flicker.red();input.nightVisionTicks=vision;input.partialTick=.375f;input.lightning=lightning;
        const auto pixels=console::buildConsoleLightmap(input);
        std::uint64_t hash=14695981039346656037ull;for(const auto& pixel:pixels)for(auto channel:pixel)hash=(hash^channel)*1099511628211ull;
        std::cout<<sample++<<' '<<hash<<' '<<std::bit_cast<std::uint32_t>(input.skyDarken)<<' '
                 <<std::bit_cast<std::uint32_t>(flicker.red())<<' '<<std::bit_cast<std::uint32_t>(flicker.green())<<'\n';
    }
    for(bool ceiling:{false,true})for(bool propagate:{false,true})for(bool missing:{false,true})
    for(int x:{-33,-16,-1,0,15,16,31,32})for(int y:{-1,0,127,128,255,256})for(int emission:{0,7,15}) {
        RenderLightFixture access;access.ceiling=ceiling;access.propagate=propagate;access.missing=missing;
        std::cout<<console::sampleConsoleLight(access,x,y,1,emission)<<' '<<console::sampleConsoleLiquidLight(access,x,y,1)<<'\n';
    }
    for(int pattern=0;pattern<27;++pattern)for(int data=0;data<16;++data)for(bool above:{false,true}){
        LiquidSurfaceFixture access;int value=pattern;
        for(int i=1;i<4;++i){access.cells[i]=value%3;value/=3;}
        for(int i=0;i<4;++i)access.metadata[i]=(data+i*3)&15;
        access.above=above;
        std::cout<<std::bit_cast<std::uint32_t>(console::consoleLiquidCorner(access,0,40,0))<<' '
                 <<std::bit_cast<std::uint32_t>(console::consoleLiquidDepth(data))<<'\n';
    }
    for(int pattern=0;pattern<128;++pattern)for(int data:{0,3,7,8,15}){
        LiquidFlowFixture access;access.cells[{0,40,0}]={1,data};
        const int offsets[4][2]={{-1,0},{0,-1},{1,0},{0,1}};
        for(int i=0;i<4;++i){int x=offsets[i][0],z=offsets[i][1];access.cells[{x,40,z}]={(pattern>>(i*2))&3,(pattern+i*3)&15};
            access.cells[{x,39,z}]={pattern&(1<<i)?1:0,(pattern+i)&7};access.cells[{x,41,z}]={pattern&(1<<(i+1))?2:0,0};}
        auto flow=console::consoleLiquidFlow(access,0,40,0);double angle=console::consoleLiquidSlope(access,0,40,0);
        std::cout<<std::bit_cast<std::uint64_t>(flow.x)<<' '<<std::bit_cast<std::uint64_t>(flow.y)<<' '<<std::bit_cast<std::uint64_t>(flow.z)<<' '<<std::bit_cast<std::uint64_t>(angle);
        for(bool lava:{false,true}){for(auto uv:console::consoleLiquidTopUV(float(angle),lava))std::cout<<' '<<std::bit_cast<std::uint32_t>(uv.u)<<' '<<std::bit_cast<std::uint32_t>(uv.v);
            for(float h:{0.f,.125f,.5f,1.f})for(bool right:{false,true}){auto uv=console::consoleLiquidSideUV(h,right,lava);std::cout<<' '<<std::bit_cast<std::uint32_t>(uv.u)<<' '<<std::bit_cast<std::uint32_t>(uv.v);}}
        std::cout<<'\n';
    }
}
