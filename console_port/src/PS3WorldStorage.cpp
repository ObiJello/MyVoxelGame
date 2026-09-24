#include "PS3WorldStorage.h"
#include "ConsoleSavePaths.h"
namespace console {
namespace {
struct BorrowedInput {
    ByteArrayInputStream bytes;
    DataInputStream data;
    explicit BorrowedInput(std::span<const unsigned char> source):bytes(byteArray(const_cast<unsigned char*>(source.data()),source.size())),data(&bytes){}
    ~BorrowedInput(){bytes.reset();}
    void requireEnd(){if(bytes.read()!=-1)throw IoError("Trailing data in save record");}
};
const wchar_t* prefix(int dimension){
    switch(dimension){case -1:return savepath::nether;case 0:return L"";case 1:return savepath::end;default:throw IoError("Unknown console dimension");}
}
unsigned local(int coordinate){return static_cast<unsigned>(coordinate)&31u;}
}
PS3WorldStorage::PS3WorldStorage():archive(std::make_unique<ConsoleSaveArchive>()){
    for(auto name:savepath::initialRegions)archive->put(name,{});
}
std::unique_ptr<PS3WorldStorage> PS3WorldStorage::readFile(const std::filesystem::path& path){
    return read(NativeSaveFile::read(path));
}
NativeSaveCommit PS3WorldStorage::writeFile(const std::filesystem::path& path){
    auto bytes=serialize();return NativeSaveFile::replace(path,bytes);
}
std::unique_ptr<PS3WorldStorage> PS3WorldStorage::read(std::span<const unsigned char> data){
    auto archive=ConsoleSaveArchive::read(data,SAVE_FILE_PLATFORM_PS3);
    // Original-version, not last-written-version, selects the chunk record family
    // in McRegionChunkStorage. Do not reinterpret old NBT chunks as version 8.
    if(archive->originalVersion()>archive->readVersion())throw IoError("Original archive version exceeds current version");
    return std::unique_ptr<PS3WorldStorage>(new PS3WorldStorage(std::move(archive)));
}
std::wstring PS3WorldStorage::regionName(int dimension,int chunkX,int chunkZ){
    // C++20 arithmetic right shift and the original 32-chunk cache grouping.
    return std::wstring(prefix(dimension))+L"r."+std::to_wstring(chunkX>>5)+L"."+std::to_wstring(chunkZ>>5)+L".mcr";
}
std::unique_ptr<CompoundTag> PS3WorldStorage::metadataRoot()const{
    if(!archive->contains(L"level.dat"))return nullptr;
    BorrowedInput input(archive->get(L"level.dat"));std::unique_ptr<CompoundTag> root(NbtIo::read(&input.data));
    if(!root || !dynamic_cast<CompoundTag*>(root->get(L"Data")))throw IoError("Missing level.dat Data compound");
    input.requireEnd();return root;
}
std::unique_ptr<LevelData> PS3WorldStorage::metadata()const{
    auto root=metadataRoot();return root?std::make_unique<LevelData>(root->getCompound(L"Data")):nullptr;
}
void PS3WorldStorage::putMetadata(LevelData& level,std::int64_t modified){
    auto root=metadataRoot();if(!root){root=std::make_unique<CompoundTag>();auto data=std::make_unique<CompoundTag>();root->put(L"Data",data.get());data.release();}
    std::unique_ptr<CompoundTag> fields(level.createTag());fields->putInt(L"version",savepath::regionVersion);
    auto* data=root->getCompound(L"Data");std::unique_ptr<std::vector<Tag*>> tags(fields->getAllTags());
    // Retain unknown fields and any older embedded player tag while updating
    // the known metadata fields; those simulation loaders are not ported yet.
    for(auto* field:*tags){std::unique_ptr<Tag> copy(field->copy());data->put(field->getName().c_str(),copy.get());copy.release();}
    ByteArrayOutputStream bytes;DataOutputStream output(&bytes);NbtIo::write(root.get(),&output);
    archive->put(L"level.dat",std::span(bytes.buf.data,bytes.size()),modified);
    level.setVersion(savepath::regionVersion);
}
std::unique_ptr<ChunkRecord> PS3WorldStorage::chunk(int dimension,int x,int z)const{
    auto name=regionName(dimension,x,z);if(!archive->contains(name))return nullptr;
    auto region=ConsoleRegionFile::read(archive->get(name));auto payload=region.chunk(local(x),local(z));if(!payload)return nullptr;
    BorrowedInput input(*payload);
    auto record=archive->originalVersion()>=SAVE_FILE_VERSION_COMPRESSED_CHUNK_STORAGE?ChunkRecord::read(&input.data):ChunkRecord::readLegacy(&input.data);
    input.requireEnd();
    if(record->x!=x || record->z!=z)throw IoError("Chunk record coordinates do not match region slot");
    return record;
}
void PS3WorldStorage::putChunk(int dimension,ChunkRecord& record,std::int64_t modified){
    auto name=regionName(dimension,record.x,record.z);
    ByteArrayOutputStream bytes;DataOutputStream output(&bytes);
    if(archive->originalVersion()>=SAVE_FILE_VERSION_COMPRESSED_CHUNK_STORAGE)record.write(&output);else record.writeLegacy(&output);
    auto region=archive->contains(name)?ConsoleRegionFile::read(archive->get(name)):ConsoleRegionFile();
    region.put(local(record.x),local(record.z),std::span(bytes.buf.data,bytes.size()),static_cast<std::uint32_t>(modified/1000));
    archive->put(name,region.serialize(),modified);
}
}
