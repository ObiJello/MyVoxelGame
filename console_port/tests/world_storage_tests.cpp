#include "PS3WorldStorage.h"
#include "ConsoleSavePaths.h"
#include "LevelSettings.h"
#include "LevelType.h"
#include <iostream>
#include <climits>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
static std::vector<unsigned char> encode(CompoundTag* tag){ByteArrayOutputStream bytes;DataOutputStream out(&bytes);NbtIo::write(tag,&out);return {bytes.buf.data,bytes.buf.data+bytes.size()};}
static std::unique_ptr<CompoundTag> decode(std::span<const unsigned char> bytes){return std::unique_ptr<CompoundTag>(NbtIo::decompress(byteArray(const_cast<unsigned char*>(bytes.data()),bytes.size())));}
int main(){try{
    using console::PS3WorldStorage;
    PS3WorldStorage world;
    std::vector<std::wstring> expected{L"DIM-1r.-1.-1.mcr",L"DIM-1r.0.-1.mcr",L"DIM-1r.0.0.mcr",L"DIM-1r.-1.0.mcr",L"DIM1/r.-1.-1.mcr",L"DIM1/r.0.-1.mcr",L"DIM1/r.0.0.mcr",L"DIM1/r.-1.0.mcr",L"r.-1.-1.mcr",L"r.0.-1.mcr",L"r.0.0.mcr",L"r.-1.0.mcr"};
    require(world.names()==expected,"Original initial region file order and distinct dimension prefixes");
    require(!world.metadata() && !world.chunk(0,32,32) && world.names()==expected,"Missing metadata/chunk reads leave the archive unchanged");
    require(PS3WorldStorage::regionName(-1,-1,32)==L"DIM-1r.-1.1.mcr" && PS3WorldStorage::regionName(1,-32,-33)==L"DIM1/r.-1.-2.mcr" && PS3WorldStorage::regionName(0,INT_MIN,INT_MAX)==L"r.-67108864.67108863.mcr","Signed coordinate grouping at boundaries and integer limits");
    rejects([&]{world.chunk(2,0,0);});
    LevelType::staticCtor();GameType::staticCtor();LevelSettings settings(-123456789,GameType::SURVIVAL,true,false,true,LevelType::lvl_normal,54,3);LevelData level(&settings,L"PS3 world");level.setSpawn(-12,70,25);level.setTime(1ll<<41);world.putMetadata(level,1234000);
    require(level.getVersion()==0x4abc,"McRegion metadata version after successful save");
    auto root=decode(world.entry(L"level.dat"));require(root->contains(L"Data") && root->getCompound(L"Data")->getLong(L"RandomSeed")==-123456789,"level.dat has original Data root wrapper");
    for(int dimension:{-1,0,1})for(auto [x,z]:std::vector<std::pair<int,int>>{{-33,-32},{-1,0},{0,-1},{31,31},{32,33}}){
        console::ChunkRecord chunk;chunk.x=x;chunk.z=z;chunk.lastUpdate=(1ll<<40)+x;chunk.lowerBlocks->set(2,5,4,dimension+2);chunk.upperData->set(1,3,7,13);chunk.extra->putString(L"Unknown",L"keep me");world.putChunk(dimension,chunk,5678000);
    }
    auto saved=world.serialize();auto loaded=PS3WorldStorage::read(saved);auto restored=loaded->metadata();require(restored->getSeed()==-123456789 && restored->getTime()==(1ll<<41) && restored->getXSpawn()==-12,"World metadata restores through archive");
    for(int dimension:{-1,0,1})for(auto [x,z]:std::vector<std::pair<int,int>>{{-33,-32},{-1,0},{0,-1},{31,31},{32,33}}){auto chunk=loaded->chunk(dimension,x,z);require(chunk && chunk->lowerBlocks->get(2,5,4)==dimension+2 && chunk->upperData->get(1,3,7)==13 && chunk->lastUpdate==(1ll<<40)+x && chunk->extra->getString(L"Unknown")==L"keep me","Dimensions, signed chunk coordinates, storage and opaque entity fields round trip");}
    // Preserve unknown archive entries, unknown metadata and legacy Player NBT.
    auto outer=ConsoleSaveArchive::read(saved);root=decode(outer->get(L"level.dat"));root->putString(L"FutureRoot",L"untouched");auto* data=root->getCompound(L"Data");data->putInt(L"FutureField",987);auto player=std::make_unique<CompoundTag>();player->putInt(L"Score",111);data->put(L"Player",player.release());outer->put(L"level.dat",encode(root.get()),1);unsigned char custom[]={6,5,4,3};outer->put(L"data/future.dat",custom,2);
    auto augmented=PS3WorldStorage::read(outer->serialize());auto changed=augmented->metadata();changed->setLevelName(L"Renamed");augmented->putMetadata(*changed,3);root=decode(augmented->entry(L"level.dat"));data=root->getCompound(L"Data");require(root->getString(L"FutureRoot")==L"untouched" && data->getInt(L"FutureField")==987 && data->getCompound(L"Player")->getInt(L"Score")==111 && data->getString(L"LevelName")==L"Renamed" && std::equal(std::begin(custom),std::end(custom),augmented->entry(L"data/future.dat").begin()),"Metadata edits preserve unported data and opaque archive entries");
    auto older=saved;older[9]=2;auto wrongFamily=PS3WorldStorage::read(older);rejects([&]{wrongFamily->chunk(0,-1,0);});older=saved;older[11]=7;rejects([&]{PS3WorldStorage::read(older);});
    // Reject a structurally valid chunk deliberately placed under the wrong slot.
    console::ChunkRecord wrong;wrong.x=7;wrong.z=8;ByteArrayOutputStream wrongBytes;DataOutputStream wrongOut(&wrongBytes);wrong.write(&wrongOut);console::ConsoleRegionFile misplaced;misplaced.put(0,0,std::span(wrongBytes.buf.data,wrongBytes.size()));outer->put(L"r.0.0.mcr",misplaced.serialize());auto mismatch=PS3WorldStorage::read(outer->serialize());rejects([&]{mismatch->chunk(0,0,0);});
    // Invalid metadata must not be silently replaced by defaults during an edit.
    CompoundTag noData;outer->put(L"level.dat",encode(&noData));auto malformed=PS3WorldStorage::read(outer->serialize());auto unchanged=malformed->serialize();rejects([&]{malformed->metadata();});rejects([&]{malformed->putMetadata(level);});require(malformed->serialize()==unchanged,"Failed metadata rewrite is transactional");
    std::cout<<"PS3 world storage paths, metadata, dimensions, record integration and preservation passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
