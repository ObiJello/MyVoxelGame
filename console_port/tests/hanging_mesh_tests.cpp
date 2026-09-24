#include "HangingMesh.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static bool near(float a,float b){return std::abs(a-b)<1e-5f;}
int main(){try{
    using namespace console;
    HangingDecoration painting;
    painting.kind=HangingDecoration::Kind::Painting;
    painting.tileX=10;painting.tileY=70;painting.tileZ=20;painting.dir=0;painting.motive=L"Pool";
    const int light=(13<<20)|(7<<4);
    auto south=buildHangingMesh(painting,light);
    require(south.painting.size()==2*6*6,"32x16 painting has two source segments with six faces each");
    require(south.frame.empty() && south.itemAtlas.empty(),"Painting uses only the artwork atlas");
    require(near(south.painting[0].u,16.f/256) && near(south.painting[0].v,48.f/256),
            "Source painting segments traverse the art atlas in reverse X/Y order");
    require(near(south.painting[0].lightU,7.5f/16) && near(south.painting[0].lightV,13.5f/16),
            "Painting vertices receive the world lightmap");
    int samples=0;
    auto segmentLit=buildHangingMesh(painting,light,[&](int x,int y,int z){
        require(y==70 && z==21 && (x==10 || x==11),"Source segment light samples follow painting width");
        ++samples;return (13<<20)|((x==10?4:9)<<4);
    });
    require(samples==2 && near(segmentLit.painting[0].lightU,4.5f/16) &&
            near(segmentLit.painting[36].lightU,9.5f/16),"Painting segments retain separate local brightness");
    painting.dir=2;auto north=buildHangingMesh(painting,light);
    require(near(south.painting[0].z+north.painting[0].z,41.f),"Opposite painting directions mirror across support block");
    painting.motive=L"DonkeyKong";
    require(buildHangingMesh(painting,light).painting.size()==12*6*6,"64x48 motive has twelve segments");
    painting.motive=L"unrecognized";
    require(buildHangingMesh(painting,light).painting.size()==6*6,"Unknown motive falls back to source Kebab");
    HangingDecoration frame;frame.kind=HangingDecoration::Kind::ItemFrame;
    frame.tileX=10;frame.tileY=70;frame.tileZ=20;frame.dir=3;
    auto empty=buildHangingMesh(frame,light);
    require(empty.frame.size()==5*6*6 && empty.itemAtlas.empty(),"Empty frame has source rim and inset back");
    frame.itemId=264;auto upright=buildHangingMesh(frame,light);frame.itemRotation=1;
    auto filled=buildHangingMesh(frame,light);
    require(filled.itemAtlas.size()==6,"Displayed item uses the item atlas");
    require(!near(filled.itemAtlas[0].y,upright.itemAtlas[0].y),"Frame rotation changes item position");
    std::cout<<"Painting motifs, directions, item frames and lightmap passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
