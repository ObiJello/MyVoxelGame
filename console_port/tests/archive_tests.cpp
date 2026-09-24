#include "ConsoleSaveArchive.h"
#include "ChunkRecord.h"
#include "LevelData.h"
#include "LevelSettings.h"
#include "LevelType.h"
#include <iostream>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
int main(){try{
    ConsoleSaveArchive archive;unsigned char payload[]={1,2,3};archive.put(L"a",payload,0x0102030405060708ll);
    auto big=archive.serialize();require(big.size()==159 && big[3]==15 && big[7]==1 && big[9]==8 && big[11]==8 && big[15]==0 && big[16]=='a',"PS3 prefix and UTF-16 filename byte order");
    require(big[15+131]==3 && big[15+135]==12 && big[15+136]==1 && big[15+143]==8,"PS3 fixed-width length, offset and timestamp fields");
    for(auto platform:{SAVE_FILE_PLATFORM_PS3,SAVE_FILE_PLATFORM_X360,SAVE_FILE_PLATFORM_PS4,SAVE_FILE_PLATFORM_PSVITA,SAVE_FILE_PLATFORM_XBONE,SAVE_FILE_PLATFORM_WIN64}){
        auto wire=archive.serialize(platform);auto read=ConsoleSaveArchive::read(wire,platform);require(std::equal(payload,payload+3,read->get(L"a").begin()) && read->modifiedTime(L"a")==0x0102030405060708ll,"Platform endian round trip");}
    archive.put(L"b",{},22);archive.put(L"maps/\U0001f30d.dat",payload,33);unsigned char larger[]={8,7,6,5,4};archive.put(L"a",larger,44);
    auto read=ConsoleSaveArchive::read(archive.serialize());require(read->names()==archive.names() && read->get(L"b").empty() && read->get(L"a").size()==5,"Growing entry adjusts later offsets, including empty files");
    auto borrowed=archive.get(L"a");archive.put(L"a",borrowed.subspan(1,2),55);require(archive.get(L"a")[0]==7 && archive.get(L"a").size()==2,"Aliased replacement copies before reallocating");
    require(archive.remove(L"b") && !archive.remove(L"b"),"Removing middle and missing entries");archive.remove(L"maps/\U0001f30d.dat");require(ConsoleSaveArchive::read(archive.serialize())->names()==std::vector<std::wstring>{L"a"},"Removing last file updates table and offsets");
    rejects([&]{archive.put(L"",payload);});rejects([&]{archive.put(std::wstring(64,L'x'),payload);});rejects([&]{archive.put(std::wstring(L"a\0b",3),payload);});
    std::wstring maxName(61,L'x');maxName+=L"\U0001f30d";archive.put(maxName,payload);require(ConsoleSaveArchive::read(archive.serialize())->contains(maxName),"63 UTF-16 units fit filename field");maxName+=L"x";rejects([&]{archive.put(maxName,payload);});
    for(std::size_t cut=0;cut<big.size();++cut){std::vector<unsigned char> truncated(big.begin(),big.begin()+cut);rejects([&]{ConsoleSaveArchive::read(truncated);});}
    auto bad=big;bad[3]=0;rejects([&]{ConsoleSaveArchive::read(bad);});bad=big;bad[7]=255;rejects([&]{ConsoleSaveArchive::read(bad);});bad=big;bad[11]=9;rejects([&]{ConsoleSaveArchive::read(bad);});
    bad=big;bad[15+135]=0;rejects([&]{ConsoleSaveArchive::read(bad);});bad=big;for(unsigned i=0;i<128;++i)bad[15+i]=1;rejects([&]{ConsoleSaveArchive::read(bad);});
    ConsoleSaveArchive pair;pair.put(L"a",std::span(payload,1));pair.put(L"b",std::span(payload,1));auto paired=pair.serialize();
    auto duplicate=paired;std::copy_n(duplicate.begin()+14,128,duplicate.begin()+14+144);rejects([&]{ConsoleSaveArchive::read(duplicate);});
    auto overlap=paired;overlap[14+144+135]=12;rejects([&]{ConsoleSaveArchive::read(overlap);});
    for(unsigned version=2;version<=7;++version){auto older=big;older[9]=1;older[11]=version;auto kept=ConsoleSaveArchive::read(older);require(kept->serialize()==older,"V2-V7 rewrites must not claim to convert legacy payloads");}
    // Legacy V1 has a byte-sized table length and no timestamps; both endian paths.
    for(auto platform:{SAVE_FILE_PLATFORM_PS3,SAVE_FILE_PLATFORM_WIN64}){
        bool be=platform==SAVE_FILE_PLATFORM_PS3;std::vector<unsigned char> legacy(148);legacy[be?3:0]=12;legacy[be?7:4]=136;legacy[be?9:8]=1;legacy[be?11:10]=1;legacy[12+(be?1:0)]='x';legacy[12+132+(be?3:0)]=12;
        auto old=ConsoleSaveArchive::read(legacy,platform);require(old->originalVersion()==1 && old->readVersion()==1 && old->modifiedTime(L"x")==0,"V1 decoding and implicit zero timestamp");
        require(old->serialize(platform)==legacy,"Legacy container version must stay unchanged without a payload converter");
    }
    // Exercise the independently ported world metadata and chunk-record codecs
    // together inside the original archive layout. No Sony wrapper is involved.
    LevelType::staticCtor();GameType::staticCtor();LevelSettings settings(1234,GameType::SURVIVAL,true,false,true,LevelType::lvl_normal,54,3);LevelData level(&settings,L"Console World");
    std::unique_ptr<CompoundTag> levelTag(level.createTag());auto levelBytes=NbtIo::compress(levelTag.get());std::unique_ptr<unsigned char[]> owned(levelBytes.data);
    console::ChunkRecord chunk;chunk.x=-1;chunk.z=2;chunk.lowerBlocks->set(1,64,1,5);ByteArrayOutputStream chunkBytes;DataOutputStream output(&chunkBytes);chunk.write(&output);
    ConsoleSaveArchive world;world.put(L"level.dat",std::span(levelBytes.data,levelBytes.length),1);world.put(L"chunk-test.bin",std::span(chunkBytes.buf.data,chunkBytes.size()),2);
    auto restored=ConsoleSaveArchive::read(world.serialize());auto levelPayload=restored->get(L"level.dat");std::unique_ptr<CompoundTag> restoredTag(NbtIo::decompress(byteArray(const_cast<unsigned char*>(levelPayload.data()),levelPayload.size())));LevelData restoredLevel(restoredTag.get());require(restoredLevel.getSeed()==1234,"Metadata survives archive serialization");
    auto chunkPayload=restored->get(L"chunk-test.bin");ByteArrayInputStream chunkInput(byteArray(const_cast<unsigned char*>(chunkPayload.data()),chunkPayload.size()));struct Detach{ByteArrayInputStream& input;~Detach(){input.reset();}} detach{chunkInput};DataInputStream chunkReader(&chunkInput);auto restoredChunk=console::ChunkRecord::read(&chunkReader);require(restoredChunk->x==-1 && restoredChunk->lowerBlocks->get(1,64,1)==5,"Chunk codec survives archive serialization");
    std::cout<<"Save archive endianness, UTF-16 fields, versions, mutation and codec integration passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
