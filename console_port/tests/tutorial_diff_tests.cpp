#include "TutorialDiff.h"
#include "World.h"
#include "PS3WorldStorage.h"
#include "NbtIo.h"
#include "ChunkStorageCodec.h"
#include "ConsoleSaveArchive.h"
#include "ConsoleRegionFile.h"
#include "ConsoleLzx.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F action){try{action();}catch(const std::exception&){return;}
    throw std::runtime_error("Malformed tutorial diff was accepted");}
int main(int argc,char** argv){try{
    using namespace console;
    require(argc==2,"Expected tutorial asset directory");
    const auto root=std::filesystem::path(argv[1]);
    std::ifstream file(root/"tutorialDiff",std::ios::binary);
    require(bool(file),"Open original tutorial difference");
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    const auto diff=TutorialDiff::read(bytes);
    std::ifstream sourceFile(root/"Tutorial",std::ios::binary);
    require(bool(sourceFile),"Open supplied original tutorial world");
    const std::vector<std::uint8_t> sourceBytes((std::istreambuf_iterator<char>(sourceFile)),{});
    require(sourceBytes.size()>8 && sourceBytes[0]==0 && sourceBytes[1]==0 &&
            sourceBytes[2]==0 && sourceBytes[3]==0,"Original tutorial world wrapper");
    auto decodedWorld=decodeConsoleLzx(std::span(sourceBytes).subspan(8),20*1024*1024);
    const std::uint32_t worldSize=(std::uint32_t(sourceBytes[4])<<24)|
        (std::uint32_t(sourceBytes[5])<<16)|(std::uint32_t(sourceBytes[6])<<8)|sourceBytes[7];
    require(decodedWorld.size()==worldSize && decodedWorld[8]==0 && decodedWorld[9]==0 &&
            decodedWorld[10]==0 && decodedWorld[11]==2,
            "Original tutorial wrapper and version-zero save table");
    // FileHeader's safe reader requires a nonzero original version. This old
    // archived table uses zero to designate the pre-compressed legacy family.
    decodedWorld[9]=2;
    auto original=ConsoleSaveArchive::read(decodedWorld,SAVE_FILE_PLATFORM_X360);
    require(original->names().size()==11 && original->contains(L"level.dat") &&
            original->contains(L"r.-1.-1.mcr"),"Original tutorial region inventory");
    auto region=ConsoleRegionFile::read(original->get(L"r.-1.-1.mcr"),SAVE_FILE_PLATFORM_X360);
    auto originalChunk=region.chunk(25,23);
    require(bool(originalChunk),"Original tutorial world contains the changed chunk");
    auto changedChunk=*originalChunk;
    require(diff.apply(-7,-9,changedChunk),"Apply source difference to its original record");
    {
        ByteArrayInputStream beforeInput(byteArray(originalChunk->data(),originalChunk->size()));
        ByteArrayInputStream afterInput(byteArray(changedChunk.data(),changedChunk.size()));
        DataInputStream beforeStream(&beforeInput),afterStream(&afterInput);
        auto before=ChunkRecord::readLegacy(&beforeStream);
        auto after=ChunkRecord::readLegacy(&afterStream);
        require(before->x==-7 && before->z==-9 && after->x==-7 && after->z==-9,
                "Original tutorial difference preserves chunk coordinates");
        beforeInput.reset();afterInput.reset();
    }
    auto incompatible=region.chunk(27,29);
    require(bool(incompatible),"Archived world contains second changed chunk");
    require(diff.apply(-5,-3,*incompatible),"Second source difference is present");
    bool exactRecord=true;
    try{
        ByteArrayInputStream input(byteArray(incompatible->data(),incompatible->size()));
        DataInputStream stream(&input);
        auto record=ChunkRecord::readLegacy(&stream);
        exactRecord=input.read()==-1;
        input.reset();
    }catch(const std::exception&){exactRecord=false;}
    require(!exactRecord,"Title-update diff is incompatible with the archived world baseline");
    require(diff.chunkCount()==37 && diff.patchCount()==8331,
            "Original title update contains four regions and 37 changed chunks");
    const auto* first=diff.chunk(-7,-9);
    require(first && first->size()==17 && first->front().offset==1750 &&
            first->front().bytes==std::vector<std::uint8_t>{0x52} &&
            first->back().offset==70117 && !diff.chunk(500,500),
            "Region slots and sparse byte edits map to original chunk coordinates");
    std::vector<std::uint8_t> small(70119,0);
    require(diff.apply(-7,-9,small) && small[1750]==0x52 &&
            !diff.apply(500,500,small),
            "A matching serialized chunk accepts the exact sparse replacements");
    auto shortRecord=std::vector<std::uint8_t>(100,0);
    rejects([&]{diff.apply(-7,-9,shortRecord);});
    require(std::all_of(shortRecord.begin(),shortRecord.end(),[](auto b){return b==0;}),
            "An out-of-range tutorial patch cannot partially change a chunk");
    auto broken=bytes;broken.pop_back();rejects([&]{TutorialDiff::read(broken);});
    broken=bytes;broken[0]=255;rejects([&]{TutorialDiff::read(broken);});
    broken=bytes;broken[80]=0;rejects([&]{TutorialDiff::read(broken);});
    const auto path=std::filesystem::temp_directory_path()/
        ("console-tutorial-diff-"+std::to_string(getpid())+".inner");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;
        std::filesystem::remove(path,ec);}} cleanup{path};
    World world;world.generateTutorial(root);
    for(int i=0;i<300;++i)if(world.streamAround({-40,150,-72},2) ||
                             !world.streaming())break;
    require(world.inside(-40,72,-72),"Affected tutorial chunk streamed into view");
    world.save(path);
    auto archive=PS3WorldStorage::readFile(path);
    auto record=archive->chunk(0,-7,-9);
    require(bool(record),"Affected tutorial record is available for format comparison");
    World classic;classic.generateArchivedTutorial(root);
    require(classic.isTutorial() && classic.seed==1171544198849424676LL &&
            classic.spawn().x==60 && classic.spawn().z==28,
            "Archived world retains its own seed and original spawn");
    const auto classicBlocks=classic.blockSnapshot();
    const auto classicPath=path.string()+".classic";
    struct ClassicCleanup{std::string path;~ClassicCleanup(){std::error_code ec;
        std::filesystem::remove(path,ec);}} classicCleanup{classicPath};
    classic.save(classicPath);
    World reloaded;require(reloaded.load(classicPath) && reloaded.isTutorial() &&
                           reloaded.blockSnapshot()==classicBlocks,
                           "Archived tutorial round trips through the playable native save");
    bool moved=false;
    for(int i=0;i<400;++i){
        moved|=reloaded.streamAround({-40,100,-72},2);
        if(moved && !reloaded.streaming())break;
    }
    require(moved && reloaded.inside(-40,100,-72),
            "Archived tutorial streams distant original chunks after save/reload");
    require(reloaded.set(-40,200,-72,Obsidian),"Distant archived chunk accepts a player edit");
    reloaded.save(classicPath);
    World edited;require(edited.load(classicPath),"Reload edited archived tutorial");
    for(int i=0;i<400;++i)if(edited.streamAround({-40,100,-72},2) &&
                               !edited.streaming())break;
    require(edited.inside(-40,200,-72) && edited.get(-40,200,-72)==Obsidian,
            "Archived chunks preserve edits across world streaming and reload");
    ByteArrayOutputStream output;DataOutputStream stream(&output);
    record->write(&stream);
    std::vector<std::uint8_t> wire(output.buf.data,output.buf.data+output.size());
    const auto wireSize=wire.size();
    bool inRange=true,decodable=false;
    try{diff.apply(-7,-9,wire);}catch(const std::exception&){inRange=false;}
    if(inRange){
        ByteArrayInputStream input(byteArray(wire.data(),wire.size()));
        DataInputStream source(&input);
        try{auto changed=ChunkRecord::read(&source);decodable=bool(changed);}
        catch(const std::exception&){}
        input.reset();
    }
    ByteArrayOutputStream oldOutput;DataOutputStream oldStream(&oldOutput);
    record->writeLegacy(&oldStream);
    std::vector<std::uint8_t> legacy(oldOutput.buf.data,
                                     oldOutput.buf.data+oldOutput.size());
    const auto legacySize=legacy.size();
    bool legacyDecodable=false;std::size_t legacyBlockChanges=0,legacyUpperChanges=0;
    try{
        diff.apply(-7,-9,legacy);
        ByteArrayInputStream input(byteArray(legacy.data(),legacy.size()));
        DataInputStream source(&input);
        try{
            auto changed=ChunkRecord::readLegacy(&source);legacyDecodable=bool(changed);
            if(changed){
                const auto before=ChunkStorageCodec::restore(*record);
                const auto after=ChunkStorageCodec::restore(*changed);
                for(std::size_t i=0;i<before.storage->blocks.size();++i)
                    if(before.storage->blocks[i]!=after.storage->blocks[i]){
                        ++legacyBlockChanges;
                        if(i%256>=128)++legacyUpperChanges;
                    }
            }
        }
        catch(const std::exception&){}
        input.reset();
    }catch(const std::exception&){}
    std::cout<<"Decoded original 11-entry tutorial save and 37 source chunk diffs / 8,331 edits; "
             <<"sample port record "<<wireSize<<" bytes, patch in range "<<inRange
             <<", parses "<<decodable<<"; 256-high legacy record "<<legacySize
             <<" bytes, patched NBT parses "<<legacyDecodable
             <<", changed blocks "<<legacyBlockChanges
             <<" (upper "<<legacyUpperChanges<<")\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
