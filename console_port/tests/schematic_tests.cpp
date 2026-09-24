#include "ConsoleSchematic.h"
#include "TutorialSchematics.h"
#include "ConsoleLzx.h"
#include "ConsoleGameRuleFile.h"
#include "ConsoleDlcPack.h"
#include "ChunkStorageCodec.h"
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iostream>
static void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(int argc,char** argv){try{
 require(argc==2,"Asset path");int files=0;
 for(auto& entry:std::filesystem::directory_iterator(argv[1])){
  if(entry.path().extension()!=".sch")continue;
  std::ifstream in(entry.path(),std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});
  auto schematic=console::ConsoleSchematic::read(bytes);++files;
  require(schematic.tags!=nullptr,"Original entity NBT retained");
  int placedTiles=0,placedEntities=0;
  for(int cx=0;cx<(schematic.width()+15)/16;++cx)for(int cz=0;cz<(schematic.depth()+15)/16;++cz){
   auto translated=schematic.tagsForChunk(cx,cz,0,0,0);
   placedTiles+=translated->getList(L"TileEntities")->size();placedEntities+=translated->getList(L"Entities")->size();
  }
  require(placedTiles==schematic.tags->getList(L"TileEntities")->size() && placedEntities==schematic.tags->getList(L"Entities")->size(),"Every original schematic entity assigned to exactly one chunk");
  for(auto cut:{std::size_t(0),std::size_t(4),std::size_t(12),bytes.size()/2,bytes.size()-1}){
   bool rejected=false;try{console::ConsoleSchematic::read(std::span<const unsigned char>(bytes).first(cut));}catch(const std::exception&){rejected=true;}
   require(rejected,"Truncated tutorial assets rejected");
  }
  auto trailing=bytes;trailing.push_back(0);bool rejected=false;try{console::ConsoleSchematic::read(trailing);}catch(const std::exception&){rejected=true;}
  require(rejected,"Trailing schematic data rejected");
  console::ChunkStorage chunk;schematic.apply(chunk,-1,-1,-10,124,-9);
  chunk.recalculateHeightmap();console::ChunkRecord context;context.x=-1;context.z=-1;
  context.extra=schematic.tagsForChunk(-1,-1,-10,124,-9);
  auto record=console::ChunkStorageCodec::capture(chunk,context);auto restored=console::ChunkStorageCodec::restore(*record);
  require(restored.storage->blocks==chunk.blocks && record->extra->equals(context.extra.get()),"Tutorial block registry permits native chunk capture and opaque entity preservation");
  for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<256;++y){
   int sx=x-6,sz=z-7,sy=y-124;bool inside=sx>=0 && sx<schematic.width() && sz>=0 && sz<schematic.depth() && sy>=0 && sy<schematic.height();
   int expected=inside?schematic.block(sx,sy,sz):0;require(chunk.blocks[(x*16+z)*256+y]==expected,"Chunk clipping across negative coordinates and height 128");
   require(chunk.metadata.get(x,y,z)==(inside?schematic.data(sx,sy,sz):0),"Original schematic metadata placement");
  }
 }
 require(files==19,"All nineteen supplied tutorial schematic assets decoded");
 std::ifstream packFile(std::filesystem::path(argv[1]).parent_path()/"Tutorial.pck",std::ios::binary);
 std::vector<unsigned char> packBytes((std::istreambuf_iterator<char>(packFile)),{});auto pack=console::ConsoleDlcPack::read(packBytes);bool rulesFound=false;
 for(const auto& entry:pack.entries)if(entry.type==7){
  auto rules=console::ConsoleGameRuleFile::read(entry.bytes);rulesFound=true;
  std::cout<<"Packed tutorial: "<<rules.files.size()<<" schematics, "<<rules.rules.size()<<" rule roots\n";
  for(const auto& [filename,bytes]:rules.files){
   auto schematic=console::ConsoleSchematic::read(bytes);require(schematic.width()>0,"Packed schematic decoded");
   std::ifstream looseFile(std::filesystem::path(argv[1])/std::filesystem::path(filename),std::ios::binary);
   require(bool(looseFile),"PS3 package schematic has a source fixture");
   std::vector<unsigned char> looseBytes((std::istreambuf_iterator<char>(looseFile)),{});
   auto loose=console::ConsoleSchematic::read(looseBytes);
   require(schematic.width()==loose.width() && schematic.height()==loose.height() && schematic.depth()==loose.depth(),"PS3 package and source schematic dimensions match");
   for(int x=0;x<schematic.width();++x)for(int z=0;z<schematic.depth();++z)for(int y=0;y<schematic.height();++y)
    require(schematic.block(x,y,z)==loose.block(x,y,z) && schematic.data(x,y,z)==loose.data(x,y,z),"PS3 package and source schematic voxels match");
   require(schematic.tags->equals(loose.tags.get()),"PS3 package and source schematic entities match");
  }
  require(rules.rules.size()==2,"Original MapOptions and LevelRules roots");
  require(rules.rules[0].name==L"MapOptions" && rules.rules[0].attributes.at(L"seed")==L"1227750481513469519","Packed original tutorial seed");
 }
 require(rulesFound,"Original packaged tutorial rules decoded");
 std::vector<unsigned char> emptyRules(28);require(console::ConsoleGameRuleFile::read(emptyRules).rules.empty(),"Original version-zero uncompressed rule file");
 emptyRules.push_back(7);bool badRules=false;try{console::ConsoleGameRuleFile::read(emptyRules);}catch(const std::exception&){badRules=true;}
 require(badRules,"Nonzero trailing rule data rejected");
 console::TutorialSchematics tutorial(std::filesystem::path(argv[1]).parent_path());
 require(console::tutorialSeed==1227750481513469519ll && console::tutorialSpawn==std::array<int,3>{97,72,-106},"Original tutorial seed and spawn");
 require(console::tutorialPlacements.size()==17 && tutorial.placements().size()==17,"Original active schematic selection");
 const auto& mapOptions=tutorial.rules().front();
 require(mapOptions.name==L"MapOptions" && mapOptions.attributes.at(L"seed")==std::to_wstring(console::tutorialSeed),"PS3 tutorial uses the generated-world seed");
 require(mapOptions.attributes.at(L"spawnX")==L"97" && mapOptions.attributes.at(L"spawnY")==L"72" && mapOptions.attributes.at(L"spawnZ")==L"-106","PS3 tutorial spawn comes from MapOptions");
 require(!mapOptions.attributes.contains(L"baseSaveName"),"PS3 tutorial is not backed by the legacy Xbox save");
 std::vector<std::pair<int,int>> forcedTemples;
 for(const auto& child:mapOptions.children){
  require(child.name!=L"BiomeOverride","Unexpected PS3 tutorial biome override");
  if(child.name==L"StartFeature"){
   require(child.attributes.at(L"feature")==L"2","Tutorial forced feature type changed");
   forcedTemples.emplace_back(std::stoi(child.attributes.at(L"chunkX")),std::stoi(child.attributes.at(L"chunkZ")));
  }
 }
 require(forcedTemples==std::vector<std::pair<int,int>>{{1,15},{-20,6}},"Original forced temple chunk coordinates");
 for(std::size_t i=0;i<tutorial.placements().size();++i){const auto& packaged=tutorial.placements()[i];const auto& source=console::tutorialPlacements[i];
  require(packaged.filename==source.filename && packaged.x==source.x && packaged.y==source.y && packaged.z==source.z,"Packaged layout matches original XML placements in order");
 }
 for(const auto& placement:console::tutorialPlacements){
  const auto& schematic=tutorial.schematic(placement.filename);int cx=int(std::floor(double(placement.x)/16)),cz=int(std::floor(double(placement.z)/16));
  console::ChunkStorage chunk;require(tutorial.apply(chunk,cx,cz),"Tutorial structure crosses its original chunk");
  for(int y=0;y<schematic.height();++y){int x=placement.x-cx*16,z=placement.z-cz*16;
   require(chunk.blocks[(x*16+z)*256+placement.y+y]==schematic.block(0,y,0),"Original structure location and vertical data");
  }
 }
 console::ChunkStorage spawnChunk;require(tutorial.apply(spawnChunk,6,-7),"Original spawn chunk intersects castle schematic");
 const auto& castle=tutorial.schematic("CasTes2.sch");
 require(spawnChunk.blocks[(1*16+6)*256+71]==castle.block(103,11,82),"Original castle floor at tutorial spawn");
 // Translation preserves original chest payloads and does not mutate the source.
 console::ConsoleSchematic fixture;fixture.tags=std::make_unique<CompoundTag>();
 auto tileList=std::make_unique<TagList>();auto chest=std::make_unique<CompoundTag>();
 chest->putString(L"id",L"Chest");chest->putInt(L"x",1);chest->putInt(L"y",2);chest->putInt(L"z",3);chest->putString(L"CustomPayload",L"keep");
 tileList->add(chest.get());chest.release();fixture.tags->put(L"TileEntities",tileList.get());tileList.release();
 auto relocated=fixture.tagsForChunk(-1,-1,-16,64,-16);auto* placed=dynamic_cast<CompoundTag*>(relocated->getList(L"TileEntities")->get(0));
 require(placed && placed->getInt(L"x")==-15 && placed->getInt(L"y")==66 && placed->getInt(L"z")==-13 && placed->getString(L"CustomPayload")==L"keep","Chest NBT translated with payload intact");
 placed->putString(L"CustomPayload",L"edited");
 auto* original=dynamic_cast<CompoundTag*>(fixture.tags->getList(L"TileEntities")->get(0));
 require(original->getInt(L"x")==1 && original->getString(L"CustomPayload")==L"keep","Schematic NBT remains reusable after placement");
 require(fixture.tagsForChunk(0,0,-16,64,-16)->getList(L"TileEntities")->size()==0,"Entity assignment excludes other chunks");
 console::ChunkStorage remote;require(!tutorial.apply(remote,26,26),"Unrelated terrain is untouched");
 for(const auto& invalid:std::vector<std::vector<unsigned char>>{{},{255},{255,0,1,0,20,0},{0,0}}){
  bool rejected=false;try{console::decodeConsoleLzx(invalid,1000);}catch(const std::exception&){rejected=true;}require(rejected,"Malformed LZX frame rejected");
 }
 std::cout<<"19 original tutorial schematics decoded, 17 original placements and malformed data checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
