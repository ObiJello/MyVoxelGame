#include "World.h"
#include "PS3WorldStorage.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}
int main(){try{
 using namespace console;
 const auto path=std::filesystem::temp_directory_path()/
     ("console-spawner-"+std::to_string(getpid())+".inner");
 struct Cleanup{std::filesystem::path file;~Cleanup(){std::error_code error;std::filesystem::remove(file,error);}} cleanup{path};
 World source;source.generate(86,true);
 for(int x=27;x<=37;++x)for(int z=27;z<=37;++z)source.set(x,179,z,Stone);
 require(source.set(32,180,32,static_cast<Block>(52)),"Place a source spawner block");
 source.save(path);
 auto archive=PS3WorldStorage::readFile(path);
 auto chunk=archive->chunk(0,-2,-2);
 require(chunk && chunk->extra,"Spawner owner chunk is saved");
 auto tiles=std::make_unique<TagList>();
 auto spawner=std::make_unique<CompoundTag>();
 spawner->putString(L"id",L"MobSpawner");spawner->putInt(L"x",-32);
 spawner->putInt(L"y",180);spawner->putInt(L"z",-32);
 spawner->putString(L"EntityId",L"Zombie");spawner->putShort(L"Delay",0);
 spawner->putShort(L"MinSpawnDelay",10);spawner->putShort(L"MaxSpawnDelay",20);
 spawner->putShort(L"SpawnCount",16);
 tiles->add(spawner.get());spawner.release();
 chunk->extra->put(L"TileEntities",tiles.get());tiles.release();
 archive->putChunk(0,*chunk);archive->writeFile(path);
 World active;require(active.load(path),"Spawner fixture loads");
 active.setPlayerPosition({32.5,180,32.5});active.tickTime();
 const auto spawnerMob=[](const SimulatedEntity& entity){return entity.id==L"Zombie" && entity.position.y>=179;};
 const auto spawnedCount=std::count_if(active.entities().begin(),active.entities().end(),spawnerMob);
 require(spawnedCount>0,"Nearby spawner creates live entities");
 auto travel=[&](double x,double z){
  for(int frame=0;frame<300 && active.streamAround({x,180,z},2)==false;++frame){}
  require(!active.streaming(),"Spawner world finishes bounded chunk streaming");
 };
 travel(400,400);
 require(active.entities().empty(),"Outgoing chunk releases its live entities");
 travel(32,32);
 require(std::count_if(active.entities().begin(),active.entities().end(),spawnerMob)==spawnedCount,
         "Returning to a chunk restores its spawned entities");
 active.save(path);
 auto savedArchive=PS3WorldStorage::readFile(path);
 int savedMobs=0;
 for(int cx=-3;cx<=-1;++cx)for(int cz=-3;cz<=-1;++cz){
  auto saved=savedArchive->chunk(0,cx,cz);if(!saved || !saved->extra)continue;
  auto* entities=dynamic_cast<TagList*>(saved->extra->get(L"Entities"));if(!entities)continue;
  for(int i=0;i<entities->size();++i){
   auto* mob=dynamic_cast<CompoundTag*>(entities->get(i));if(!mob || !mob->getBoolean(L"console_port.simulated"))continue;
   auto* pos=mob->getList(L"Pos");
   if(mob->getString(L"id")!=L"Zombie" || !pos || pos->size()!=3 ||
      !dynamic_cast<DoubleTag*>(pos->get(1)) ||
      dynamic_cast<DoubleTag*>(pos->get(1))->data<179)continue;
   require(mob->getList(L"Rotation")->size()==2 && mob->getShort(L"Health")==20,
           "Spawned mobs carry source base-entity and health NBT");
   ++savedMobs;
  }
 }
 require(savedMobs==int(spawnedCount),"Spawned mobs have native owning-chunk entity records");
 World reloaded;require(reloaded.load(path),"Spawned-entity world reloads");
 require(reloaded.entities().size()==active.entities().size(),"Live entities persist in owning chunk NBT");
 reloaded.setPlayerPosition({300,180,300});reloaded.tickTime();
 require(reloaded.entities().empty(),"Distant transient mobs despawn");
 std::cout<<"Spawner delay, nearby spawning, entity persistence and despawn passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
