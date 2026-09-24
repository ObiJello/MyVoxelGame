#include "PS3WorldStorage.h"
#include "ChunkStorageCodec.h"
#include <fstream>
#include <future>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
int main(int argc,char** argv){try{
    require(argc==2,"Test requires a workspace scratch directory");auto base=std::filesystem::path(argv[1]);std::filesystem::create_directories(base);
    auto pattern=(base/"save-test-XXXXXX").string();std::vector<char> temp(pattern.begin(),pattern.end());temp.push_back(0);require(::mkdtemp(temp.data())!=nullptr,"Cannot create test directory");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{std::filesystem::path(temp.data())};auto directory=cleanup.path;
    auto path=directory/"World 🧱.inner";
    console::ChunkStorage chunk;chunk.set(2,65,3,89,5);chunk.skyLight.set(2,66,3,15);chunk.blockLight.set(2,65,3,15);console::ChunkRecord context;context.x=-1;context.z=2;context.extra->putString(L"Preserve",L"entity data");
    auto record=console::ChunkStorageCodec::capture(chunk,context);console::PS3WorldStorage world;world.putChunk(0,*record,1234);auto committed=world.writeFile(path);require(committed.directorySynced,"Test filesystem should support directory fsync");
    auto loaded=console::PS3WorldStorage::readFile(path);auto stored=loaded->chunk(0,-1,2);auto restored=console::ChunkStorageCodec::restore(*stored);require(restored.storage->blocks==chunk.blocks && restored.storage->metadata.get(2,65,3)==5 && restored.storage->blockLight.get(2,65,3)==15 && stored->extra->getString(L"Preserve")==L"entity data","Chunk snapshot reaches disk and restores through the original inner archive");
    struct stat info{};require(::stat(path.c_str(),&info)==0 && (info.st_mode&0777)==0600,"Save file permissions are private");
    auto before=console::NativeSaveFile::read(path);rejects([&]{console::NativeSaveFile::replace(path,{});});require(console::NativeSaveFile::read(path)==before,"Invalid replacement leaves the old save intact");
    auto occupied=directory/"occupied";std::filesystem::create_directory(occupied);{std::ofstream marker(occupied/"keep");marker<<"preserved";}rejects([&]{console::NativeSaveFile::replace(occupied,before);});require(std::filesystem::exists(occupied/"keep"),"Failed rename preserves destination directory");
    rejects([&]{console::NativeSaveFile::replace(directory/"missing"/"save",before);});
    for(auto& entry:std::filesystem::directory_iterator(directory))require(!entry.path().filename().string().starts_with(".console-save-"),"Failed writes clean temporary siblings");
    auto corrupt=directory/"corrupt";{std::ofstream stream(corrupt,std::ios::binary);stream<<"truncated";}rejects([&]{console::PS3WorldStorage::readFile(corrupt);});
    auto oversized=directory/"oversized";{std::ofstream stream(oversized,std::ios::binary);stream.seekp(PS3_MAX_SAVE_BYTES);stream.put(0);}rejects([&]{console::NativeSaveFile::read(oversized);});
    auto pipe=directory/"pipe";require(::mkfifo(pipe.c_str(),0600)==0,"Creating fixture FIFO");rejects([&]{console::NativeSaveFile::read(pipe);});rejects([&]{console::NativeSaveFile::read(directory);});rejects([&]{console::NativeSaveFile::read(directory/"absent");});rejects([&]{console::NativeSaveFile::read(std::filesystem::path(std::string("bad\0name",8)));});
    // Concurrent readers must see complete old or new files, including size
    // changes. Each read holds its opened inode across the writer's rename.
    std::vector<unsigned char> a(12000,17),b(33000,239);console::NativeSaveFile::replace(path,a);
    auto reader=std::async(std::launch::async,[&]{for(int i=0;i<200;++i){auto bytes=console::NativeSaveFile::read(path);require(bytes==a || bytes==b,"Reader saw a partial replacement");}});
    for(int i=0;i<20;++i)console::NativeSaveFile::replace(path,i%2?std::span(a):std::span(b));reader.get();
    // Separate temp names also keep two simultaneous writers from sharing a
    // staging file. Last writer wins, and either whole payload is valid.
    auto writeA=std::async(std::launch::async,[&]{console::NativeSaveFile::replace(path,a);});auto writeB=std::async(std::launch::async,[&]{console::NativeSaveFile::replace(path,b);});writeA.get();writeB.get();auto last=console::NativeSaveFile::read(path);require(last==a || last==b,"Concurrent writers must commit one complete payload");
    for(auto& entry:std::filesystem::directory_iterator(directory))require(!entry.path().filename().string().starts_with(".console-save-"),"Committed writes leave no temporary files");
    std::cout<<"Native archive persistence, atomic replacement, failure cleanup and concurrent reads passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
