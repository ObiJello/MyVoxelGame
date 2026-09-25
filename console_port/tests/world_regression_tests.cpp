// Regressions found in review: liquid depth, furnace facing and entity
// persistence across streaming-window moves.
#include "World.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}
int main(){try{
 using namespace console;
 {
  // LiquidTileStatic::setDynamic keeps depth: one source must not become many.
  World world;world.generate(41,true);
  for(int x=4;x<=60;++x)for(int z=4;z<=60;++z)world.set(x,179,z,Stone);
  require(world.setTileAndUpdate(32,180,32,static_cast<Block>(8)),"Place a water source");
  for(int i=0;i<600;++i)world.tickTime();
  int sources=0,wet=0;
  for(int x=4;x<=60;++x)for(int z=4;z<=60;++z){
   const int id=world.get(x,180,z);
   if(id==8 || id==9){++wet;if(world.getData(x,180,z)==0)++sources;}
  }
  require(sources==1,"Flowing water does not create new sources");
  // Seven-block spread on a flat floor is a diamond of at most 113 cells.
  require(wet>1 && wet<=113,"Water spreads a bounded distance");
 }
 {
  // FurnaceTile::setLit keeps the facing metadata.
  World world;world.generate(7,true);
  const int y=world.surface(40,40)+1;
  require(world.placeBlock(42,y,40,static_cast<Block>(61),0,{40.5,double(y),40.5},1.0),"Place a furnace");
  const int facing=world.getData(42,y,40);
  require(world.setCreativeHotbarItem(0,4,0,8) && world.setCreativeHotbarItem(1,263,0,8),"Stock the hotbar");
  require(world.transferFurnaceItem(42,y,40,0,false,0) && world.transferFurnaceItem(42,y,40,1,false,1),"Load the furnace");
  for(int i=0;i<3;++i)world.tickTime();
  require(world.get(42,y,40)==62,"Furnace lights");
  require(world.getData(42,y,40)==facing,"Lit furnace keeps its facing");
 }
 {
  // Spawned mobs survive the window moving away and back.
  World world;world.generate(41,true);
  const int y=world.surface(10,64)+1;
  world.setPlayerPosition({64,double(world.surface(64,64)+1),64});
  require(world.spawnCreativeEgg(92,{10.5,double(y),64.5}),"Spawn a cow");
  world.tickTime();
  require(world.entities().size()==1,"Cow is live");
  const auto travel=[&](double x,double z){
   world.setPlayerPosition({x,double(y),z});
   for(int frame=0;frame<3000 && !world.streamAround({x,double(y),z},2);++frame){}
   while(world.streaming())world.streamAround({x,double(y),z},2);
   world.tickTime();
  };
  travel(90,64);
  require(world.originX()!=0,"Window moved");
  travel(64,64);
  require(world.entities().size()==1,"Cow survives a window round trip");
  travel(200,64);travel(64,64);
  require(world.entities().size()==1,"Cow survives eviction and reload");
 }
 {
  // A killed mob loaded from a save does not come back after a window move.
  const auto path=std::filesystem::temp_directory_path()/
      ("console-regression-"+std::to_string(getpid())+".inner");
  struct Cleanup{std::filesystem::path file;~Cleanup(){std::error_code error;std::filesystem::remove(file,error);}} cleanup{path};
  World source;source.generate(41,true);
  const int y=source.surface(64,64)+1;
  source.setPlayerPosition({64.5,double(y),60.5});
  require(source.spawnCreativeEgg(50,{64.5,double(y),66.5}),"Spawn a creeper");
  source.save(path);
  World world;require(world.load(path),"Reload the creeper world");
  world.setPlayerPosition({64.5,double(y),60.5});
  const auto creepers=[&]{int count=0;for(const auto& entity:world.entities())if(entity.id==L"Creeper")++count;return count;};
  require(creepers()==1,"Saved creeper loads");
  for(int i=0;i<300 && creepers();++i){
   for(const auto& entity:world.entities())if(entity.id==L"Creeper" && entity.health>0 && entity.invulnerableTicks<=10){
    const auto target=entity.position;
    world.attackEntity({64.5,y+1.62,60.5},{target.x-64.5,target.y+.9-(y+1.62),target.z-60.5},276,20);
    break;
   }
   world.tickTime();
  }
  require(!creepers(),"Creeper is killed");
  world.setPlayerPosition({64,double(y),90});
  for(int frame=0;frame<3000 && !world.streamAround({64,double(y),90},2);++frame){}
  while(world.streaming())world.streamAround({64,double(y),90},2);
  world.tickTime();
  require(world.originZ()!=0,"Window moved after the kill");
  require(!creepers(),"Killed saved creeper stays dead");
 }
 std::cout<<"World regression checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
