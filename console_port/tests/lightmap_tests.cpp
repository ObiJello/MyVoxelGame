#include "ConsoleLightmap.h"
#include "TerrainMesh.h"
#include "CelestialMesh.h"
#include "SkyColour.h"
#include "BiomeTint.h"
#include "render_light_fixture.h"
#include "liquid_surface_fixture.h"
#include "liquid_flow_fixture.h"
#include <iostream>
#include <limits>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F action){try{action();}catch(const std::exception&){return;}throw std::runtime_error("Expected invalid render lighting input to fail");}
int main(){try{
    using namespace console;
    require(consoleCloudColour(6000,0,0,0)==std::array<float,3>{1,1,1},"Original daytime cloud white");
    require(consoleCloudColour(18000,0,0,0)==std::array<float,3>{.1f,.1f,.15f},"Original night cloud tint");
    auto clouds=buildCloudMesh({0,64,0},0,{1,1,1},0);
    require(clouds.size()==1536 && clouds[0].x==-256 && clouds[0].z==-224 && clouds[0].y==128+.33f,"Original basic cloud coverage and altitude");
    require(clouds[0].u==-256/2048.f && clouds[0].v==-224/2048.f && clouds[0].a==.8f,"Original cloud UV scale and opacity");
    auto wrappedClouds=buildCloudMesh({-2048,400,2048},0,{1,1,1},0);
    require(wrappedClouds[0].u==clouds[0].u && wrappedClouds[0].v==clouds[0].v && wrappedClouds[0].y==clouds[0].y,"Cloud texture repeats in both directions without following camera height");
    auto movingClouds=buildCloudMesh({0,64,0},100,{1,1,1},0);
    require(std::abs(movingClouds[0].u-clouds[0].u-3.f/2048)<.000001f,"Original cloud drift advances .03 blocks per tick");
    rejects([]{buildCloudMesh({},0,{1,1,2});});
    std::array<int,9> biomeSamples{};biomeSamples.fill(1);
    require(consoleBiomeTint(TintKind::Water,biomeSamples)==0xffffff,"Original ordinary water tint leaves texture colour intact");
    require(consoleBiomeTint(TintKind::Foliage,biomeSamples,1)==0x619961 && consoleBiomeTint(TintKind::Foliage,biomeSamples,6)==0x80a755,"Original spruce and birch colour overrides retain metadata flags");
    biomeSamples.fill(6);require(consoleBiomeTint(TintKind::Water,biomeSamples)==0xe0ffae,"Original swamp water tint");
    biomeSamples[0]=1;
    require(consoleBiomeTint(TintKind::Water,biomeSamples)==(((224*8+255)/9)<<16 | (255<<8) | (174*8+255)/9),"Nine-biome tint average truncates each channel independently");
    rejects([&]{auto bad=biomeSamples;bad[8]=255;consoleBiomeTint(TintKind::Grass,bad);});
    auto plainsSky=consoleSkyColour(1,6000,0,0,0,0);
    require(plainsSky==std::array<float,3>{121/255.f,167/255.f,1},"Original plains sky palette");
    require(consoleSkyColour(2,6000,0,0,0,0)==std::array<float,3>{111/255.f,178/255.f,1},"Original desert sky palette");
    require(consoleSkyColour(1,18000,0,0,0,0)==std::array<float,3>{0,0,0},"Night sky darkens fully");
    auto stormSky=consoleSkyColour(1,6000,1,1,0,0);
    require(stormSky[0]<plainsSky[0] && stormSky[2]<plainsSky[2],"Rain and thunder desaturate and darken sky");
    auto flashSky=consoleSkyColour(1,18000,0,0,2,0);
    require(flashSky[0]==.8f*.45f && flashSky[2]==.45f,"Original lightning tint");
    rejects([]{consoleSkyColour(23,0);});rejects([]{consoleSkyColour(0,0,0,2);});
    for(int phase=0;phase<8;++phase){
        auto sky=buildCelestialMesh(phase*24000ll+6000,0,0);
        require(sky.sun.size()==6 && sky.moon.size()==6,"Original celestial quads triangulate");
        require(sky.sun[0].y==100 && sky.moon[0].y==-100,"Original noon sun and moon elevations");
        require(sky.sun[0].x==30 && sky.sun[0].z==-30,"Original celestial Y rotation and sun size");
        require(sky.moon[0].u==(phase%4+1)/4.f && sky.moon[0].v==(phase/4+1)/2.f,"Original moon phase atlas order");
        require(consoleMoonPhase(phase*24000ll)==phase,"Original moon phase period");
    }
    auto midnight=buildCelestialMesh(18000,0,0);
    require(midnight.sun[0].y < -99 && midnight.moon[0].y>99,"Sun and moon exchange at midnight");
    auto rainy=buildCelestialMesh(6000,.75f);
    require(rainy.sun[0].a==.25f && rainy.moon[0].a==.25f,"Original rain celestial opacity");
    require(consoleMoonPhase(8*24000)==0 && consoleMoonPhase(-24000)==-1,"Original signed phase remainder");
    rejects([]{buildCelestialMesh(0,2);});rejects([]{buildCelestialMesh(0,0,2);});
    auto day=buildConsoleLightmap({});
    require(day[0]==LightPixel{14,14,14,255},"Original dark lightmap texel");
    require(day[240]==LightPixel{250,250,250,255},"Original full daylight texel");
    require(day[10][0]>day[10][1] && day[10][1]>day[10][2],"Original warm block-light color");
    LightmapInput input;input.skyDarken=.2f;auto night=buildConsoleLightmap(input);
    require(night[240][0]<day[240][0] && night[0]==day[0],"Night dims skylight, not the unlit floor");
    input.nightVisionTicks=201;auto vision=buildConsoleLightmap(input);
    require(vision[0]==LightPixel{252,252,252,255},"Original night-vision normalization");
    input={};input.dimension=-1;auto nether=buildConsoleLightmap(input);
    require(nether[0][0]>day[0][0],"Nether ambient ramp");
    input.dimension=1;auto end=buildConsoleLightmap(input);require(end[0]==end[240],"End lightmap ignores sky component");
    require(consoleSkyDarken(6000)>.99f && consoleSkyDarken(18000)==.2f,"Original day and night sky brightness");
    require(consoleSkyDarken(6000,1,1)<consoleSkyDarken(6000),"Rain and thunder darken daylight");
    require(consoleTimeOfDay(1234,.5f,-1)==.5f && consoleTimeOfDay(1234,.5f,1)==0,"Original dimension celestial angles");
    require(consoleNightVisionScale(201,.5f)==1 && consoleNightVisionScale(0,1)==.7f,"Night-vision duration boundaries");
    LightFlicker flicker;std::array<double,8> samples{.75,.25,.5,.5,.25,.75,.5,.5};flicker.tick(samples);
    require(flicker.red()>0 && flicker.green()<0,"Independent original red and green flicker channels");
    float previous=flicker.red();samples[7]=1;rejects([&]{flicker.tick(samples);});require(flicker.red()==previous,"Invalid flicker sample rejects before mutation");
    for(float invalid:{-1.f,2.f,std::numeric_limits<float>::quiet_NaN()}){
        input={};input.skyDarken=invalid;rejects([&]{buildConsoleLightmap(input);});
        rejects([&]{consoleSkyDarken(0,invalid);});rejects([&]{consoleTimeOfDay(0,invalid);});
    }
    input={};input.dimension=2;rejects([&]{buildConsoleLightmap(input);});input={};input.flicker=std::numeric_limits<float>::infinity();rejects([&]{buildConsoleLightmap(input);});
    RenderLightFixture access;
    const int direct=sampleConsoleLight(access,0,40,0,15);
    require((direct&255)==240 && (direct>>20)==access.storedLight(LightLayer::Sky,0,40,0),"Packed light coordinate and emission floor");
    access.ceiling=true;require((sampleConsoleLight(access,0,40,0,0)>>20)==0,"Ceiling suppresses skylight");
    access.ceiling=false;require((sampleConsoleLight(access,0,256,0,0)>>20)==15,"Above-build-height sky boundary");
    access.missing=true;require(sampleConsoleLight(access,0,40,0,0)==15<<20,"Missing chunk surrounding light");
    rejects([&]{sampleConsoleLight(access,INT_MAX,0,0,0);});rejects([&]{sampleConsoleLight(access,0,0,0,16);});
    access={};const int lower=sampleConsoleLight(access,0,40,0,0),upper=sampleConsoleLight(access,0,41,0,0);
    require(sampleConsoleLiquidLight(access,0,40,0)==(std::max(lower&255,upper&255)|(std::max((lower>>16)&255,(upper>>16)&255)<<16)),"Liquid lighting combines each channel independently with the cell above");
    LiquidSurfaceFixture liquid;
    require(std::abs(consoleLiquidCorner(liquid,0,40,0)-(1-(11.f/9+3)/14))<.000001f,"Isolated source uses original weighted corner height");
    liquid.cells.fill(1);require(std::abs(consoleLiquidCorner(liquid,0,40,0)-8.f/9)<.000001f,"Still pool corner height");
    liquid.metadata.fill(7);require(std::abs(consoleLiquidCorner(liquid,0,40,0)-1.f/9)<.000001f,"Shallow fluid metadata changes surface height");
    liquid.metadata.fill(8);require(std::abs(consoleLiquidCorner(liquid,0,40,0)-8.f/9)<.000001f,"Falling fluid data uses source depth");
    liquid.above=true;require(consoleLiquidCorner(liquid,0,40,0)==1,"Stacked liquids join at full block height");
    liquid.above=false;liquid.cells.fill(2);rejects([&]{consoleLiquidCorner(liquid,0,40,0);});
    rejects([&]{consoleLiquidDepth(-1);});rejects([&]{consoleLiquidDepth(16);});rejects([&]{sampleConsoleLiquidLight(access,0,INT_MAX,0);});
    LiquidFlowFixture flowAccess;flowAccess.cells[{0,40,0}]={1,0};
    require(consoleLiquidSlope(flowAccess,0,40,0)==-1000,"Still fluid uses original slope sentinel");
    flowAccess.cells[{1,40,0}]={1,5};auto flow=consoleLiquidFlow(flowAccess,0,40,0);
    require(flow.x==1 && flow.y==0 && flow.z==0,"Fluid flows toward lower eastern level");
    flowAccess.cells.erase({1,40,0});flowAccess.cells[{1,39,0}]={1,0};flow=consoleLiquidFlow(flowAccess,0,40,0);
    require(flow.x==1,"Flow detects lower neighboring fluid across a drop");
    flowAccess.cells[{1,40,0}]={2,0};require(consoleLiquidSlope(flowAccess,0,40,0)==-1000,"Solid wall blocks drop flow");
    flowAccess.cells[{0,40,0}]={1,8};flow=consoleLiquidFlow(flowAccess,0,40,0);require(flow.y==-1,"Falling liquid receives downward current beside a solid wall");
    flowAccess.cells[{1,40,0}]={3,0};flow=consoleLiquidFlow(flowAccess,0,40,0);require(flow.y==0,"Original ice exception does not trigger downward current");
    auto stillUV=consoleLiquidTopUV(-1000,false),movingUV=consoleLiquidTopUV(0,false);
    require(stillUV[0].u>13.f/16 && stillUV[0].u<14.f/16 && movingUV[0].u>=14.f/16,"Original still/flowing water atlas rectangles");
    require(consoleLiquidTopUV(0,true)[0].v>movingUV[0].v,"Lava uses its separate atlas rows");
    rejects([&]{consoleLiquidTopUV(std::numeric_limits<float>::infinity(),false);});rejects([&]{consoleLiquidSideUV(2,false,false);});
    rejects([&]{consoleLiquidFlow(flowAccess,INT_MAX,0,0);});rejects([&]{consoleLiquidFlow(flowAccess,10,40,10);});
    flowAccess.cells[{0,40,0}]={1,16};rejects([&]{consoleLiquidFlow(flowAccess,0,40,0);});

    World world;
    // A sealed room exercises the same CPU mesh builder used by the GL renderer.
    for(int x=6;x<=10;++x)for(int y=38;y<=42;++y)for(int z=6;z<=10;++z)
        if(x==6 || x==10 || y==38 || y==42 || z==6 || z==10)world.set(x,y,z,Stone);
    require(world.renderLight(8,40,8)==0,"Sealed room samples darkness from both layers");
    require((world.renderLight(-1,255,0)>>20)==15,"Visible region edge samples the lighting halo");
    auto darkMesh=buildTerrainMesh(world);require(!darkMesh.opaque.empty(),"Real terrain mesh emits faces");
    auto roomLights=[](const TerrainMesh& mesh){float result=0;for(const auto& v:mesh.opaque)if(v.x==7 && v.y==40 && v.z==8)result=std::max(result,v.lightU);return result;};
    require(roomLights(darkMesh)==.5f/16,"Room wall vertices carry the darkest block-light texel center");
    world.set(7,40,8,Lava);auto litMesh=buildTerrainMesh(world);
    require(roomLights(litMesh)>roomLights(darkMesh) && world.blockLight(8,40,8)>0,"Block emission reaches the real mesh after an edit");
    require((world.renderLight(8,40,8)&255)==world.blockLight(8,40,8)*16,"Air retains its own stored light rather than propagating a brighter neighbor");
    world.set(7,40,8,Air);auto cleared=buildTerrainMesh(world);require(roomLights(cleared)==roomLights(darkMesh),"Removing a source darkens rendered walls again");
    world.set(20,255,20,Stone);auto ceiling=buildTerrainMesh(world);bool fullSky=false;
    for(const auto& v:ceiling.opaque){require(v.lightU>=.5f/16 && v.lightU<=15.5f/16 && v.lightV>=.5f/16 && v.lightV<=15.5f/16,"Mesh light UV bounds");if(v.y==256 && v.x==20 && v.z==20 && v.lightV==15.5f/16)fullSky=true;}
    require(fullSky,"Build-ceiling top face receives full sky light");
    world.set(30,60,30,Water);auto sourceMesh=buildTerrainMesh(world);
    float sourceTop=0;for(const auto& v:sourceMesh.water)if(v.x>=30 && v.x<=31 && v.z>=30 && v.z<=31)sourceTop=std::max(sourceTop,v.y);
    world.setData(30,60,30,7);auto shallowMesh=buildTerrainMesh(world);float shallowTop=0;
    for(const auto& v:shallowMesh.water)if(v.x>=30 && v.x<=31 && v.z>=30 && v.z<=31)shallowTop=std::max(shallowTop,v.y);
    require(sourceTop>shallowTop && sourceTop<61,"Actual water mesh uses original metadata-dependent heights");
    world.set(30,61,30,Water);auto stackedMesh=buildTerrainMesh(world);bool joins=false;
    for(const auto& v:stackedMesh.water)if(v.x==30 && v.z==30 && v.y==61)joins=true;
    require(joins,"Stacked water sides meet without the former fixed-height gap");
    world.set(32,60,30,Lava);auto lavaMesh=buildTerrainMesh(world);bool litLavaTop=false;
    const float expectedHeight=60+consoleLiquidCorner(LiquidSurfaceFixture{},0,40,0)-.001f;
    for(const auto& v:lavaMesh.opaque)if(v.x==32 && v.z==30 && std::abs(v.y-expectedHeight)<.00001f && v.lightU==15.5f/16)litLavaTop=true;
    require(litLavaTop,"Lava surface uses its own full emission rather than dim air above");
    world.set(40,60,40,Water);world.set(41,60,40,Water);world.setData(41,60,40,5);
    auto movingMesh=buildTerrainMesh(world);bool flowingTop=false,flowingSide=false;
    for(const auto& vertex:movingMesh.water)if(vertex.x==40 && vertex.z==40){
        if(vertex.y>60.5f && vertex.u>=14.f/16)flowingTop=true;
        if(vertex.y==60 && vertex.u>=14.f/16)flowingSide=true;
    }
    require(flowingTop && flowingSide,"Actual fluid mesh uses flow-oriented top UVs and flowing side atlas");
    rejects([&]{world.renderLight(-2,40,0);});
    std::cout<<"Console lightmap, propagated sampling, effects and real terrain mesh lighting passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
