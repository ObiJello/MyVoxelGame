#include "BookMesh.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static bool near(float a,float b){return std::abs(a-b)<1e-5f;}
int main(){try{
    using namespace console;
    BookAnimation animation(187);
    const int light=(10<<20)|(6<<4);
    auto closed=buildBookMesh(20,80,30,animation,light);
    require(closed.size()==240,"Source BookModel has seven parts and masked page faces");
    for(const auto& vertex:closed){
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z),
                "Closed book mesh positions are finite");
        require(vertex.u>=-.01f && vertex.u<=1.01f && vertex.v>=-.01f && vertex.v<=1.01f,
                "Book UVs remain within the source atlas and its 0.1-pixel face inset");
        require(near(vertex.lightU,6.5f/16) && near(vertex.lightV,10.5f/16),
                "Book pages use the console lightmap");
    }
    animation.tick({100,80,100},20,80,30);
    require(animation.open==0 && animation.time==1,"Distant book remains closed while time advances");
    for(int tick=0;tick<12;++tick)animation.tick({20.5,80,30.5},20,80,30);
    require(near(animation.open,1) && animation.time==13,"Nearby player opens book at source tenth-per-tick rate");
    auto open=buildBookMesh(20,80,30,animation,light);
    require(open.size()==closed.size(),"Opening preserves all model faces");
    bool moved=false;for(std::size_t i=0;i<open.size();++i)
        moved|=!near(open[i].x,closed[i].x) || !near(open[i].z,closed[i].z);
    require(moved,"Lids and pages rotate around their source pivots");
    const float old=animation.open;
    animation.tick({100,80,100},20,80,30);
    require(animation.open<old,"Book closes when no player is near");
    std::cout<<"Source book model parts, page motion, proximity and lighting passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
