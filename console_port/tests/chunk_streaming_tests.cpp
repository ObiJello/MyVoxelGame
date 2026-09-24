#include "World.h"
#include "ChunkGenerator.h"
#include "TerrainMesh.h"
#include "PS3WorldStorage.h"
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>
#include <unistd.h>
static void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static double maxStreamMilliseconds=0;
static void travel(console::World& world,double x,double z){
 bool changed=false;for(int frame=0;frame<300;++frame){
  auto start=std::chrono::steady_clock::now();changed=world.streamAround({x,180,z},2);
  maxStreamMilliseconds=std::max(maxStreamMilliseconds,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
  if(changed || !world.streaming())break;
 }
 require(!world.streaming(),"Streaming finishes within the bounded frame budget");
 require(world.inside(int(x),180,int(z)),"Destination becomes playable");
 require(world.residentChunks()<=144,"Only an 8x8 window plus two-chunk halo remains resident");
}
int main(int argc,char** argv){try{
 using namespace console;
 require(argc==2,"Scratch directory required");std::filesystem::create_directories(argv[1]);
 auto name=(std::filesystem::path(argv[1])/"streaming-XXXXXX").string();std::vector<char> temp(name.begin(),name.end());temp.push_back(0);
 require(mkdtemp(temp.data()),"Temporary directory");
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}}cleanup{temp.data()};
 auto path=cleanup.path/"world.inner";
 World world;world.generate(42,true);
 require(world.worldMin()==-368 && world.worldMax()==496,"Original 54-chunk finite world bounds in client coordinates");
 world.set(16,180,16,Log);world.setData(16,180,16,6);world.set(15,180,16,Lava);
 require(!world.streamAround({64,180,64}) && !world.streaming(),"Center does not start needless streaming");
 const auto revision=world.revision;
 require(!world.streamAround({100,180,64},1) && world.streaming(),"Generation is spread over frames");
 require(world.originX()==0 && world.revision==revision && world.getData(16,180,16)==6,"Active window stays usable while preparing chunks");
 world.set(17,180,16,Wool);world.setData(17,180,16,14);
 // Saving mid-stream must retain edits; pending generated chunks can be regenerated.
 world.save(path);
 travel(world,100,64);require(world.originX()>0 && !world.collides({130.5,4.001,64.5}),"Walking can cross the former 128-block wall");
 travel(world,400,400);require(!world.inside(16,180,16),"Distant old chunks are unloaded");
 world.set(400,180,400,Log);world.setData(400,180,400,9);world.save(path);
 travel(world,-300,-300);
 require(world.get(-300,3,-300)==Grass,"Negative-coordinate streaming generates terrain");
 world.set(-300,181,-300,Wool);world.setData(-300,181,-300,11);
 const auto hit=world.raycast({-299.5,181.5,-297.5},{0,0,-1});require(hit.hit && hit.x==-300 && hit.z==-300,"Raycast addresses negative streamed coordinates");
 auto mesh=buildTerrainMesh(world);bool found=false;for(const auto& v:mesh.opaque)if(v.x==-300 && v.y==182 && v.z==-300)found=true;
 require(found,"Terrain mesh contains vertices at streamed world coordinates");
 require(world.skyColour(-300,-300)[2]>=0 && world.neighboringBiomes(-300,-300)[0]==1,"Sky and biome tint address streamed chunks");
 world.save(path);
 travel(world,64,64);
 require(world.get(16,180,16)==Log && world.getData(16,180,16)==6 && world.getData(17,180,16)==14,"Return trip restores edits made before and during streaming");
  require(world.blockLight(15,181,16)==14 && world.blockLight(16,181,16)==13,"Cross-chunk block light survives eviction and return");
 auto preserved=PS3WorldStorage::readFile(path);auto center=preserved->chunk(0,0,0);
 center->extra->putString(L"StreamingOpaqueFixture",L"preserved");preserved->putChunk(0,*center);
 ChunkRecord foreign;foreign.x=0;foreign.z=0;foreign.extra->putString(L"OtherDimension",L"preserved");preserved->putChunk(1,foreign);preserved->writeFile(path);
 World loaded;require(loaded.load(path),"Save while far from origin reloads");
 travel(loaded,400,400);require(loaded.getData(400,180,400)==9,"Positive distant edits survive disk reload");
 travel(loaded,-300,-300);require(loaded.get(-300,181,-300)==Wool && loaded.getData(-300,181,-300)==11,"Negative distant edits survive disk reload");
 travel(loaded,495,495);require(loaded.originX()==368 && loaded.originZ()==368 && loaded.collides({495.9,180,495.5}),"East/south finite edge remains bounded");
 travel(loaded,-368,-368);require(loaded.originX()==-368 && loaded.originZ()==-368 && loaded.collides({-368.1,180,-367.5}),"West/north finite edge remains bounded");
 loaded.save(path);auto roundTrip=PS3WorldStorage::readFile(path);
 require(roundTrip->chunk(0,0,0)->extra->getString(L"StreamingOpaqueFixture")==L"preserved" &&
         roundTrip->chunk(1,0,0)->extra->getString(L"OtherDimension")==L"preserved","Eviction and save retain unknown chunk fields and other dimensions");
 // Two independent worlds must produce the same signed-coordinate streamed
 // terrain and natural feature pass. Raw generator parity has its own test.
 World natural;natural.generate(1234);travel(natural,-150,200);
 World matching;matching.generate(1234);travel(matching,300,-300);travel(matching,-150,200);
 const int cx=-14,cz=8;
 for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<128;++y){
  int wx=cx*16+64+x,wz=cz*16+64+z;
  require(natural.get(wx,y,wz)==matching.get(wx,y,wz) && natural.getData(wx,y,wz)==matching.getData(wx,y,wz),
          "Signed-coordinate streamed natural features are deterministic");
 }
 auto naturalPath=cleanup.path/"natural.inner";natural.save(naturalPath);
 auto generatedArchive=PS3WorldStorage::readFile(naturalPath);
 int dungeonSpawners=0,dungeonChests=0;
 std::array<int,3> firstSpawner{},firstDungeonChest{};
 for(int chunkX=-4;chunkX<4;++chunkX)for(int chunkZ=-4;chunkZ<4;++chunkZ){
  auto record=generatedArchive->chunk(0,chunkX,chunkZ);
  if(!record || !record->extra)continue;
  auto* tiles=dynamic_cast<TagList*>(record->extra->get(L"TileEntities"));
  if(!tiles)continue;
  for(int i=0;i<tiles->size();++i){
   auto* tile=dynamic_cast<CompoundTag*>(tiles->get(i));if(!tile)continue;
   const int x=tile->getInt(L"x"),y=tile->getInt(L"y"),z=tile->getInt(L"z");
   if(tile->getString(L"id")==L"MobSpawner"){
    require((x>=0?x/16:(x-15)/16)==chunkX &&
            (z>=0?z/16:(z-15)/16)==chunkZ,"Dungeon spawner is stored in its own chunk");
    require(y>=0 && y<256 && (tile->getString(L"EntityId")==L"Zombie" ||
           tile->getString(L"EntityId")==L"Skeleton" || tile->getString(L"EntityId")==L"Spider"),
           "Dungeon spawner keeps its generated entity type");
    if(!dungeonSpawners)firstSpawner={x,y,z};
    ++dungeonSpawners;
   }else if(tile->getString(L"id")==L"Chest"){
    require(y>=0 && y<256 && tile->getList(L"Items"),"Dungeon chest has a native item list");
    if(!dungeonChests)firstDungeonChest={x,y,z};
    ++dungeonChests;
   }
  }
 }
 require(dungeonSpawners>0,"Natural world generates saved dungeon spawners");
 require(dungeonChests>0,"Natural world generates saved dungeon loot chests");
 World naturalReload;require(naturalReload.load(naturalPath),"Decorated natural world reloads");
 travel(naturalReload,-150,200);
 for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<128;++y){
  int wx=cx*16+64+x,wz=cz*16+64+z;
  require(natural.get(wx,y,wz)==naturalReload.get(wx,y,wz) && natural.getData(wx,y,wz)==naturalReload.getData(wx,y,wz),
          "Decorated streamed chunk survives native save and reload");
 }
 travel(naturalReload,firstSpawner[0]+64,firstSpawner[2]+64);
 require(naturalReload.get(firstSpawner[0]+64,firstSpawner[1],firstSpawner[2]+64)==static_cast<Block>(52),
         "Generated dungeon spawner survives native save, reload and streaming");
 travel(naturalReload,firstDungeonChest[0]+64,firstDungeonChest[2]+64);
 require(naturalReload.get(firstDungeonChest[0]+64,firstDungeonChest[1],firstDungeonChest[2]+64)==static_cast<Block>(54) &&
         naturalReload.chestItems(firstDungeonChest[0]+64,firstDungeonChest[1],firstDungeonChest[2]+64).size()>=27,
         "Generated dungeon chest and loot inventory survive native save and streaming");
 // A two-column move performs 12 chunk-generation calls and then 16
 // decoration calls at budget two. Keep editing the old view during
 // decoration; the staged target must not invalidate its live light region.
 World duringDecoration;duringDecoration.generate(75);
 duringDecoration.blockLight(16,181,16);
 for(int frame=0;frame<16;++frame)require(!duringDecoration.streamAround({100,180,64},2),"Two-column move commits before decoration finishes");
 require(duringDecoration.streaming() && duringDecoration.originX()==0,"Old world remains active during decoration");
 auto editStart=std::chrono::steady_clock::now();
 require(duringDecoration.set(16,180,16,Lava),"Edit during decoration remains accepted");
 const auto decorationEditMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-editStart).count();
 require(duringDecoration.blockLight(16,181,16)==14,"Edit during decoration retains prepared live light");
 travel(duringDecoration,100,64);travel(duringDecoration,64,64);
 require(duringDecoration.get(16,180,16)==Lava,"Edit during decoration survives streaming commit and return");
 World naturalEdges;naturalEdges.generate(2468);
 travel(naturalEdges,495,495);
 require(naturalEdges.originX()==368 && naturalEdges.originZ()==368,"Natural eastern finite edge remains reachable");
 travel(naturalEdges,-368,-368);
 require(naturalEdges.originX()==-368 && naturalEdges.originZ()==-368,"Natural western finite edge remains reachable");
 // Unsupported saved chunks fail before the active window changes.
 auto archive=PS3WorldStorage::readFile(path);ChunkRecord unsupported;unsupported.x=18;unsupported.z=18;unsupported.lowerBlocks->set(0,0,0,200);archive->putChunk(0,unsupported);archive->writeFile(path);
 World guarded;guarded.load(path);bool rejected=false;
 try{for(int i=0;i<200;++i)if(guarded.streamAround({400,180,400},2))break;}catch(const std::exception&){rejected=true;}
 require(rejected && guarded.originX()==0 && guarded.originZ()==0 && !guarded.streaming(),"Unsupported incoming block leaves the active world intact");
 require(guarded.getData(16,180,16)==6,"Failed stream retains existing edits");
 // Edits made while the new window's lighting is being prepared must be
 // included in the completed light pass and survive a later return trip.
 World editing;editing.generate(65,true);
 bool enteredLighting=false;
 for(int frame=0;frame<300 && editing.streaming()==false;++frame)editing.streamAround({100,180,64},2);
 for(int frame=0;frame<300;++frame){
  editing.streamAround({100,180,64},2);
  if(editing.originX()!=0 && editing.streaming()){enteredLighting=true;break;}
 }
 require(enteredLighting,"Streaming exposes a bounded lighting preparation phase");
 require(editing.set(100,180,64,Lava),"Edit during staged lighting is accepted");
 travel(editing,100,64);
 require(editing.get(100,180,64)==Lava && editing.blockLight(100,181,64)==14,"Staged lighting includes edits made during preparation");
 std::cout<<"Finite-world streaming, bounded residency, signed generation, meshes, lighting and save round trips passed; maximum stream call "<<maxStreamMilliseconds<<" ms, decoration-phase edit "<<decorationEditMilliseconds<<" ms\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
