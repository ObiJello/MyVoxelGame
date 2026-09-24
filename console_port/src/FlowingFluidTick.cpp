#include "FlowingFluidTick.h"
#include "Random.h"
#include <algorithm>
#include <array>

namespace console {
namespace {
constexpr std::array<std::array<int,2>,4> offsets{{{-1,0},{1,0},{0,-1},{0,1}}};
struct Pass {
    FlowingFluidAccess& level;
    Random& random;
    int dynamic,still;
    bool lava;
    bool same(int x,int y,int z)const{
        const int id=level.tile(x,y,z);return id==dynamic || id==still;
    }
    int depth(int x,int y,int z)const{return same(x,y,z)?level.data(x,y,z):-1;}
    bool blocked(int x,int y,int z)const{
        if(!level.inside(x,y,z))return true;
        const int id=level.tile(x,y,z);
        if(id==64 || id==71 || id==63 || id==68 || id==65 || id==83 || id==90)return true;
        return id && level.blocksMotion(x,y,z);
    }
    bool canSpread(int x,int y,int z)const{
        if(!level.inside(x,y,z) || same(x,y,z))return false;
        const int id=level.tile(x,y,z);
        return id!=10 && id!=11 && !blocked(x,y,z);
    }
    int slope(int x,int y,int z,int pass,int from)const{
        int lowest=1000;
        for(int d=0;d<4;++d){
            if((d==0&&from==1)||(d==1&&from==0)||(d==2&&from==3)||(d==3&&from==2))continue;
            const int xx=x+offsets[d][0],zz=z+offsets[d][1];
            if(blocked(xx,y,zz) || (same(xx,y,zz) && level.data(xx,y,zz)==0))continue;
            if(!blocked(xx,y-1,zz))return pass;
            if(pass<4)lowest=std::min(lowest,slope(xx,y,zz,pass+1,d));
        }
        return lowest;
    }
    std::array<bool,4> spread(int x,int y,int z)const{
        std::array<int,4> distance{1000,1000,1000,1000};
        for(int d=0;d<4;++d){
            const int xx=x+offsets[d][0],zz=z+offsets[d][1];
            if(blocked(xx,y,zz) || (same(xx,y,zz) && level.data(xx,y,zz)==0))continue;
            distance[d]=blocked(xx,y-1,zz)?slope(xx,y,zz,1,d):0;
        }
        const int lowest=*std::min_element(distance.begin(),distance.end());
        std::array<bool,4> result{};
        for(int d=0;d<4;++d)result[d]=distance[d]==lowest;
        return result;
    }
    void spreadTo(int x,int y,int z,int amount){
        if(canSpread(x,y,z))level.put(x,y,z,dynamic,amount);
    }
    void tick(int x,int y,int z){
        if(level.tile(x,y,z)!=dynamic)return;
        int current=depth(x,y,z),drop=lava?2:1;
        bool becomeStatic=true;
        if(current>0){
            int highest=-100,sources=0;
            for(auto [dx,dz]:offsets){
                int neighbor=depth(x+dx,y,z+dz);
                if(neighbor<0)continue;
                if(neighbor==0)++sources;
                if(neighbor>=8)neighbor=0;
                highest=highest<0?neighbor:std::min(highest,neighbor);
            }
            int next=highest+drop;
            if(highest<0 || next>=8)next=-1;
            const int above=depth(x,y+1,z);
            if(above>=0)next=above>=8?above:above+8;
            if(!lava && sources>=2 &&
               (level.blocksMotion(x,y-1,z) || (same(x,y-1,z) && level.data(x,y-1,z)==0)))next=0;
            if(lava && current<8 && next<8 && next>current && random.nextInt(4)!=0){
                next=current;becomeStatic=false;
            }
            if(next==current){if(becomeStatic)level.put(x,y,z,still,current);}
            else{
                current=next;
                if(current<0)level.put(x,y,z,0,0);
                else level.put(x,y,z,dynamic,current);
            }
        }else level.put(x,y,z,still,current);
        if(canSpread(x,y-1,z)){
            if(lava && (level.tile(x,y-1,z)==8 || level.tile(x,y-1,z)==9)){
                level.put(x,y-1,z,1,0);return;
            }
            spreadTo(x,y-1,z,current>=8?current:current+8);
        }else if(current>=0 && (current==0 || blocked(x,y-1,z))){
            const auto directions=spread(x,y,z);
            const int next=current>=8?1:current+drop;
            if(next>=8)return;
            for(int d=0;d<4;++d)if(directions[d])spreadTo(x+offsets[d][0],y,z+offsets[d][1],next);
        }
    }
};
}
void tickFlowingFluid(FlowingFluidAccess& level,Random& random,int x,int y,int z){
    const int id=level.tile(x,y,z);
    if(id!=8 && id!=10)return;
    Pass{level,random,id,id+1,id==10}.tick(x,y,z);
}
}
