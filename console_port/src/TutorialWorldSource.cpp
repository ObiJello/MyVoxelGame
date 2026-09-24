#include "TutorialWorldSource.h"
#include "ConsoleLzx.h"
#include "NbtIo.h"
#include "io/IoCompat.h"
namespace console {
namespace {
std::uint32_t word(std::span<const unsigned char> bytes,int at){
    if(bytes.size()<std::size_t(at)+4)throw IoError("Truncated archived tutorial wrapper");
    return (std::uint32_t(bytes[at])<<24)|(std::uint32_t(bytes[at+1])<<16)|
           (std::uint32_t(bytes[at+2])<<8)|bytes[at+3];
}
struct Input {
    ByteArrayInputStream bytes;
    DataInputStream stream;
    explicit Input(std::span<const unsigned char> data):
        bytes(byteArray(const_cast<unsigned char*>(data.data()),data.size())),stream(&bytes){}
    ~Input(){bytes.reset();}
    void requireEnd(){int value=bytes.read();if(value!=-1){std::size_t remaining=1;while(bytes.read()!=-1)++remaining;
        throw IoError("Trailing tutorial NBT data: "+std::to_string(remaining)+" bytes, first "+std::to_string(value));}}
};
}
TutorialWorldSource::TutorialWorldSource(std::span<const unsigned char> wrappedSave){
    if(wrappedSave.size()<9 || word(wrappedSave,0)!=0)throw IoError("Invalid archived tutorial wrapper");
    const auto size=word(wrappedSave,4);
    if(size<12 || size>PS3_MAX_SAVE_BYTES)throw IoError("Invalid archived tutorial size");
    try{
        auto decoded=decodeConsoleLzx(wrappedSave.subspan(8),size);
        if(decoded.size()!=size || decoded[8]!=0 || decoded[9]!=0 ||
           decoded[10]!=0 || decoded[11]!=2)
            throw IoError("Unexpected archived tutorial save version");
        // Original FileHeader accepts originalVersion zero for legacy saves.
        // Its safe port starts at one; normalize this private copy to two.
        // Chunks are still always parsed through the legacy NBT path below.
        decoded[9]=2;
        save_=ConsoleSaveArchive::read(decoded,SAVE_FILE_PLATFORM_X360);
    }catch(const std::runtime_error& error){throw IoError(error.what());}
    if(!save_->contains(L"level.dat"))throw IoError("Archived tutorial has no level metadata");
    for(int x=-1;x<=0;++x)for(int z=-1;z<=0;++z){
        const auto name=L"r."+std::to_wstring(x)+L"."+std::to_wstring(z)+L".mcr";
        if(!save_->contains(name))throw IoError("Archived tutorial is missing an overworld region");
        regions_.emplace(std::pair{x,z},ConsoleRegionFile::read(save_->get(name),SAVE_FILE_PLATFORM_X360));
    }
}
std::unique_ptr<LevelData> TutorialWorldSource::metadata()const{
    Input input(save_->get(L"level.dat"));
    // Legacy level.dat reserves a larger fixed record after the root tag.
    // The additional bytes are unrelated to LevelData's Data compound.
    std::unique_ptr<CompoundTag> root(NbtIo::read(&input.stream));
    if(!root || !dynamic_cast<CompoundTag*>(root->get(L"Data")))
        throw IoError("Archived tutorial has invalid level metadata");
    return std::make_unique<LevelData>(root->getCompound(L"Data"));
}
std::unique_ptr<ChunkRecord> TutorialWorldSource::chunk(int x,int z)const{
    const int rx=x>>5,rz=z>>5;
    auto region=regions_.find({rx,rz});if(region==regions_.end())return nullptr;
    auto bytes=region->second.chunk(unsigned(x)&31,unsigned(z)&31);
    if(!bytes)return nullptr;
    Input input(*bytes);std::unique_ptr<ChunkRecord> record;
    try{record=ChunkRecord::readLegacy(&input.stream);input.requireEnd();}
    catch(const std::exception& error){throw IoError("Archived tutorial chunk "+std::to_string(x)+","+
        std::to_string(z)+": "+error.what());}
    if(record->x!=x || record->z!=z)throw IoError("Archived tutorial chunk coordinates do not match slot");
    return record;
}
}
