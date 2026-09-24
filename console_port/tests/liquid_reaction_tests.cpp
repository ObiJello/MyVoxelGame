#include "LiquidReaction.h"
#include "World.h"
#include <climits>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Access final:console::LiquidReactionAccess {
    bool isLava=true;int metadata=0,mask=0;
    bool lava(int,int,int)const override{return isLava;}
    bool water(int x,int y,int z)const override{
        int bit=x==-1?0:x==1?1:y==-1?2:y==1?3:z==-1?4:5;
        return mask&(1<<bit);
    }
    int data(int,int,int)const override{return metadata;}
};
int main(int argc,char** argv){try{
    using namespace console;
    for(bool lava:{false,true})for(int mask=0;mask<64;++mask)for(int data=0;data<16;++data){
        Access access;access.isLava=lava;access.mask=mask;access.metadata=data;
        auto result=consoleLiquidReaction(access,0,0,0);
        bool touching=lava && (mask&~4)!=0;
        require(result.fizz==touching,"Contact includes sides and above but excludes water below");
        int expected=!touching?0:data==0?49:data<=4?4:0;
        require(result.replacement==expected,"Source/shallow/deep/falling lava reaction thresholds");
    }
    Access access;access.mask=1;access.metadata=16;
    bool rejected=false;try{consoleLiquidReaction(access,0,0,0);}catch(const std::out_of_range&){rejected=true;}
    require(rejected,"Reject invalid contacted-fluid metadata");
    rejected=false;try{consoleLiquidReaction(access,INT_MAX,0,0);}catch(const std::out_of_range&){rejected=true;}
    require(rejected,"Reject overflowing neighbor coordinates");
    World world;
    world.set(15,180,16,Lava);world.set(16,180,16,Water);
    require(world.get(15,180,16)==Lava && world.blockLight(15,180,16)==15,"Raw storage remains no-update");
    auto effects=world.updateLiquidNeighbors(16,180,16);
    require(effects.size()==1 && world.get(15,180,16)==Obsidian,"Water edit cools lava across chunk boundary");
    require(world.blockLight(15,180,16)==0 && world.getData(15,180,16)==0,"Solidification clears emission and metadata");
    require(solid(Obsidian) && validBlock(Obsidian) && textureTile(Obsidian,0)==37,"Obsidian collision, storage and original atlas tile");
    world.set(30,180,30,Water);world.set(30,179,30,Lava);world.setData(30,179,30,4);
    world.updateLiquidNeighbors(30,179,30);require(world.get(30,179,30)==Cobble,"Placed shallow lava cools under water");
    world.set(40,179,40,Water);world.set(40,180,40,Lava);
    require(world.updateLiquidNeighbors(40,180,40).empty() && world.get(40,180,40)==Lava,"Water below cannot solidify lava");
    require(world.updateLiquidNeighbors(-1,0,0).empty(),"Outside edits cannot notify the world");
    require(argc==2,"Scratch path required");std::filesystem::create_directories(argv[1]);
    auto name=(std::filesystem::path(argv[1])/"liquid-save-XXXXXX").string();std::vector<char> temp(name.begin(),name.end());temp.push_back(0);
    require(mkdtemp(temp.data())!=nullptr,"Temporary save directory");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{temp.data()};
    auto path=cleanup.path/"world.inner";world.save(path);World loaded;
    require(loaded.load(path) && loaded.get(15,180,16)==Obsidian && loaded.get(30,179,30)==Cobble,"Cooled blocks round-trip through native saves");
    std::cout<<"2048 liquid contact cases and client cooling/storage checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
