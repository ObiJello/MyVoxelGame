#include "SkullMesh.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static bool near(float a,float b){return std::abs(a-b)<1e-5f;}
int main(){try{
    using namespace console;
    const int light=(12<<20)|(6<<4);
    auto standing=buildSkullMesh(10,70,20,1,0,light);
    require(standing.size()==36,"SkeletonHeadModel is a six-face cube");
    float lowest=100,highest=-100;
    for(const auto& vertex:standing){
        lowest=std::min(lowest,vertex.y);highest=std::max(highest,vertex.y);
        require(vertex.u>=0 && vertex.u<=.5f && vertex.v>=0 && vertex.v<=.5f,
                "Source skull UVs fit the 64x32 skin atlas");
        require(near(vertex.lightU,6.5f/16) && near(vertex.lightV,12.5f/16),
                "Skull geometry retains source lightmap coordinates");
    }
    require(lowest>69.99f && highest>70.5f,"Standing skull sits on its supporting block");
    auto wall=buildSkullMesh(10,70,20,2,0,light);
    require(wall.size()==36 && wall[0].y>standing[0].y,"Wall skull uses source quarter-block vertical offset");
    auto rotated=buildSkullMesh(10,70,20,1,4,light);
    require(!near(rotated[0].x,standing[0].x) || !near(rotated[0].z,standing[0].z),
            "Standing skull applies sixteenth-turn NBT rotation");
    require(buildSkullMesh(10,70,20,0,0,light).empty(),"Invalid support direction has no skull geometry");
    std::cout<<"Source skull box, UV, support orientation and lightmap passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
