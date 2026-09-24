#include "World.h"
#include <stdexcept>
#include <iostream>
#include <filesystem>
#include <unistd.h>
static void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
int main(){try{
 using namespace console;
 World water;water.generate(41,true);
 for(int x=29;x<=35;++x)for(int z=29;z<=35;++z)water.set(x,179,z,Stone);
 water.set(32,180,32,Water);water.updateLiquidNeighbors(32,180,32);
 for(int i=0;i<4;++i)water.tickTime();
 require(water.get(31,180,32)==Air,"Water spread before its five-tick delay");
 water.tickTime();
 require((water.get(32,180,32)==Water || water.get(32,180,32)==static_cast<Block>(8)) &&
         water.get(31,180,32)==static_cast<Block>(8) &&
         water.getData(31,180,32)==1,"Source water spreads one depth level across supported ground");
 World falling;falling.generate(42,true);
 falling.set(32,181,32,Water);falling.updateLiquidNeighbors(32,181,32);
 for(int i=0;i<5;++i)falling.tickTime();
 require(falling.get(32,180,32)==static_cast<Block>(8) && falling.getData(32,180,32)==8,
         "Water falls with source falling-depth metadata");
 World lava;lava.generate(43,true);
 for(int x=29;x<=35;++x)for(int z=29;z<=35;++z)lava.set(x,179,z,Stone);
 lava.set(32,180,32,Lava);lava.updateLiquidNeighbors(32,180,32);
 for(int i=0;i<29;++i)lava.tickTime();
 require(lava.get(31,180,32)==Air,"Lava spread before its thirty-tick delay");
 lava.tickTime();
 require(lava.get(31,180,32)==Lava && lava.getData(31,180,32)==2,
         "Overworld lava spreads two depth levels after thirty ticks");
 World persistent;persistent.generate(44,true);
 for(int x=29;x<=35;++x)for(int z=29;z<=35;++z)persistent.set(x,179,z,Stone);
 persistent.set(32,180,32,Water);persistent.updateLiquidNeighbors(32,180,32);
 persistent.tickTime();persistent.tickTime();
 const auto path=std::filesystem::temp_directory_path()/
     ("console-flow-"+std::to_string(getpid())+".inner");
 struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);}} cleanup{path};
 persistent.save(path);
 World restored;require(restored.load(path),"Saved fluid world reloads");
 restored.tickTime();restored.tickTime();
 require(restored.get(31,180,32)==Air,"Reload retains the original fluid tick deadline");
 restored.tickTime();
 require(restored.get(31,180,32)==static_cast<Block>(8),"Reloaded scheduled water tick resumes flow");
 World streamed;streamed.generate(45,true);
 auto travel=[&](double x,double z){
  for(int frame=0;frame<300 && !streamed.streamAround({x,180,z},2);++frame){}
  require(!streamed.streaming(),"Fluid chunk streaming completes");
 };
 travel(150,40);
 for(int x=146;x<=154;++x)for(int z=36;z<=44;++z)streamed.set(x,179,z,Stone);
 streamed.set(150,180,40,Water);streamed.updateLiquidNeighbors(150,180,40);
 streamed.tickTime();streamed.tickTime();
 travel(400,400);travel(150,40);
 streamed.tickTime();streamed.tickTime();
 require(streamed.get(149,180,40)==Air,"Evicted fluid chunk retains a pending deadline");
 streamed.tickTime();
 require(streamed.get(149,180,40)==static_cast<Block>(8),"Fluid tick resumes after chunk eviction and return");
 std::cout<<"Scheduled water and lava flow, metadata and downward spread passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
