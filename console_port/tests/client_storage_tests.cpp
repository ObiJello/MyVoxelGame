#include "World.h"
#include "PS3WorldStorage.h"
#include "LevelSettings.h"
#include "WorldLibrary.h"
#include "ChunkGenerator.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Expected client storage rejection");}
int main(int argc,char** argv){try{
    using namespace console;
    require(argc==2,"Missing scratch directory");auto base=std::filesystem::path(argv[1]);std::filesystem::create_directories(base);
    auto name=(base/"client-save-XXXXXX").string();std::vector<char> temporary(name.begin(),name.end());temporary.push_back(0);require(::mkdtemp(temporary.data())!=nullptr,"Cannot create fixture directory");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{temporary.data()};auto path=cleanup.path/"world.inner";
    World source;require(World::height==256,"Client exposes original build height");
    auto flatChunk=generateFlatChunk();
    for(int x=0;x<16;++x)for(int z=0;z<16;++z)for(int y=0;y<128;++y)
        require(flatChunk.blocks[(x*16+z)*128+y]==(y==0?Bedrock:y<=2?Dirt:y==3?Grass:Air),"Original superflat layer ordering");
    for(auto biome:flatChunk.biomes)require(biome==1,"Original fixed plains biome");
    {
        World flat;flat.generate(42,true);require(flat.isFlat() && flat.spawn().y>4 && flat.spawn().y<4.01,"Flat world spawns on the grass surface");
        require(flat.get(0,0,0)==Bedrock && flat.get(127,3,127)==Grass && flat.get(10,4,10)==Air,"Flat terrain reaches client boundaries");
        World handoff;const auto flatSeed=flat.seed;const auto flatRevision=flat.revision;
        flat.swapWith(handoff);
        require(handoff.isFlat() && handoff.seed==flatSeed && handoff.revision==flatRevision && handoff.get(127,3,127)==Grass,
            "Prepared world state can move to the client without regenerating chunks");
        flat.swapWith(handoff);
        auto flatPath=allocateWorldSave(cleanup.path/"flat-fixture");flat.save(flatPath);
        auto flatArchive=PS3WorldStorage::readFile(flatPath);auto flatHalo=flatArchive->chunk(0,-6,-6);
        require(flatHalo && flatHalo->lowerBlocks->get(0,3,0)==Grass,"Flat halo uses flat generation");
        World flatLoaded;require(flatLoaded.load(flatPath) && flatLoaded.isFlat() && flatLoaded.spawn().y<5,"Flat generator type survives save/load");
        require(flatLoaded.neighboringBiomes(0,0)==std::array<int,9>{1,1,1,1,1,1,1,1,1},"Flat biome halo survives loading");
    }
    auto initialTime=source.time();source.tickTime();source.tickTime();
    require(source.time()==initialTime+2,"World clock advances one tick per update");
    require(listSavedWorlds(cleanup.path).empty(),"Empty save library");
    auto firstSave=allocateWorldSave(cleanup.path),secondSave=allocateWorldSave(cleanup.path);
    require(firstSave!=secondSave && firstSave.parent_path()!=secondSave.parent_path(),"Independent world save directories");
    source.setName("First World");source.save(firstSave);
    {World clockSave;require(clockSave.load(firstSave) && clockSave.time()==source.time(),"World clock persists across reload");}
    source.setName("Second World");source.save(secondSave);
    auto library=listSavedWorlds(cleanup.path);
    require(library.size()==2 && library[0].name=="First World" && library[1].name=="Second World","Library reads original metadata names in stable order");
    require(library[0].path==firstSave && library[1].path==secondSave,"World selection retains the correct save path");
    rejects([&]{source.setName("");});rejects([&]{source.setName(std::string(26,'x'));});
    rejects([&]{source.setName("Bad\nName");});
    auto broken=allocateWorldSave(cleanup.path);{std::ofstream file(broken);file<<"bad save";}
    library=listSavedWorlds(cleanup.path);
    require(library.size()==3 && !library.back().readable,"Corrupt save stays listed without hiding valid worlds");
    require(source.set(0,255,0,Stone) && !source.set(0,256,0,Stone),"Client edits reach the top original section and stop at the build ceiling");
    source.set(16,64,16,Log);source.setData(16,64,16,2);source.set(15,180,16,Lava);
    require(source.getData(16,64,16)==2 && source.blockLight(15,180,16)==15 && source.blockLight(16,180,16)==14,"Metadata and upper-half cross-chunk light use the ported storage");
    source.save(path);auto original=PS3WorldStorage::readFile(path);auto metadata=original->metadata();require(metadata && metadata->getGameType()==GameType::CREATIVE,"Client saves original level metadata");
    auto top=original->chunk(0,-4,-4);require(top && top->upperBlocks->get(0,127,0)==Stone,"Local client coordinates map into signed console chunks");
    auto wood=original->chunk(0,-3,-3);require(wood && wood->lowerData->get(0,64,0)==2,"Client metadata reaches original packed sections");
    auto halo=original->chunk(0,-6,-6);require(halo!=nullptr,"Prepared neighbor halo is saved with the active region");
    World restored;require(restored.load(path) && restored.blockSnapshot()==source.blockSnapshot() && restored.getData(16,64,16)==2,"Native archive restores full-height blocks and metadata");
    require(restored.blockLight(16,180,16)==14,"Wrapped ceiling height is repaired using full-height light propagation");
    restored.set(15,180,16,Air);require(restored.blockLight(16,180,16)==0,"Removing a loaded light source updates the neighboring chunk");
    // A valid saved heightmap permits an unchanged save to retain the exact
    // stored light fields. Synthetic light below checks serialization only.
    source.set(0,255,0,Air);source.save(path);original=PS3WorldStorage::readFile(path);auto center=original->chunk(0,0,0);
    center->extra->putString(L"UnknownEntityPayload",L"keep");center->lowerBlockLight->set(7,40,7,9);original->putChunk(0,*center);
    ChunkRecord outside;outside.x=12;outside.z=12;outside.lowerBlocks->set(0,0,0,200);outside.extra->putString(L"Outside",L"untouched");original->putChunk(0,outside);original->putChunk(1,outside);
    auto data=original->metadata();data->setTime(1ll<<44);original->putMetadata(*data);
    auto bytes=original->serialize();auto container=ConsoleSaveArchive::read(bytes,SAVE_FILE_PLATFORM_PS3);const unsigned char custom[]={0,1,200,255};container->put(L"unknown.resource",custom);NativeSaveFile::replace(path,container->serialize(SAVE_FILE_PLATFORM_PS3));
    World retained;require(retained.load(path) && retained.blockLight(71,40,71)==9,"Loading valid saved light does not silently rebuild it");retained.save(path);
    auto again=PS3WorldStorage::readFile(path);auto savedCenter=again->chunk(0,0,0);require(savedCenter->lowerBlockLight->get(7,40,7)==9 && savedCenter->extra->getString(L"UnknownEntityPayload")==L"keep","Unchanged saves preserve packed light and unknown chunk fields");
    require(again->metadata()->getTime()==(1ll<<44) && again->chunk(0,12,12)->lowerBlocks->get(0,0,0)==200 && again->chunk(1,12,12)->extra->getString(L"Outside")==L"untouched" && again->entry(L"unknown.resource").size()==4,"World time, unloaded chunks, other dimensions and unknown archive files survive client saves");
    auto snapshot=retained.blockSnapshot();auto revision=retained.revision;
    auto pipe=cleanup.path/"pipe";require(::mkfifo(pipe.c_str(),0600)==0,"Cannot create fixture pipe");rejects([&]{retained.load(pipe);});require(retained.revision==revision,"Nonregular save files are rejected without replacing the live world");
    auto invalid=again->chunk(0,-4,-4);invalid->lowerBlocks->set(0,0,0,200);again->putChunk(0,*invalid);auto invalidPath=cleanup.path/"unsupported.inner";again->writeFile(invalidPath);
    rejects([&]{retained.load(invalidPath);});require(retained.revision==revision && retained.blockSnapshot()==snapshot,"Unsupported client block rejects the whole load before modifying the live world");
    auto survival=PS3WorldStorage::readFile(path);auto survivalData=survival->metadata();survivalData->setGameType(GameType::ADVENTURE);survival->putMetadata(*survivalData);survival->writeFile(invalidPath);rejects([&]{retained.load(invalidPath);});require(retained.revision==revision,"Unported adventure saves cannot be opened");
    auto incomplete=ConsoleSaveArchive::read(NativeSaveFile::read(path),SAVE_FILE_PLATFORM_PS3);incomplete->remove(PS3WorldStorage::regionName(0,-4,-4));NativeSaveFile::replace(invalidPath,incomplete->serialize(SAVE_FILE_PLATFORM_PS3));rejects([&]{retained.load(invalidPath);});require(retained.blockSnapshot()==snapshot,"Missing active-region chunks cannot partially replace the live world");
    auto occupied=cleanup.path/"occupied";std::filesystem::create_directory(occupied);retained.set(64,240,64,Bricks);rejects([&]{retained.save(occupied);});retained.save(path);World retry;require(retry.load(path) && retry.get(64,240,64)==Bricks,"Failed file commit retains edits for a subsequent save");
    // Earlier 128-high prototype files migrate into full-height chunks. Their
    // original file remains unchanged when the client saves to world.inner.
    std::vector<unsigned char> old(World::width*128*World::depth,Air);old[(31*World::width+17)*128+127]=Bricks;std::uint64_t hash=14695981039346656037ull;for(auto value:old)hash=(hash^value)*1099511628211ull;
    auto oldPath=cleanup.path/"world.mcp";std::ofstream stream(oldPath,std::ios::binary);stream.write("MCPORT01",8);auto put=[&](std::uint64_t value){for(int i=0;i<8;++i)stream.put(static_cast<char>(value>>(i*8)));};put(std::uint64_t(INT64_MIN));put(old.size());put(hash);stream.write(reinterpret_cast<char*>(old.data()),old.size());stream.close();
    World migrated;require(migrated.load(oldPath) && migrated.seed==INT64_MIN && migrated.get(17,127,31)==Bricks && migrated.get(17,128,31)==Air,"128-high prototype migration preserves signed seed and column layout");migrated.save(cleanup.path/"migrated.inner");std::ifstream unchanged(oldPath,std::ios::binary);char signature[8];unchanged.read(signature,8);require(std::memcmp(signature,"MCPORT01",8)==0,"Migration retains the old prototype file");
    std::cout<<"Client full-height chunk storage, native saves, metadata/light preservation, failed loads and prototype migration passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
