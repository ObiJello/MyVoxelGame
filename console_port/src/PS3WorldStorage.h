#pragma once
#include "ConsoleSaveArchive.h"
#include "ConsoleRegionFile.h"
#include "ChunkRecord.h"
#include "LevelData.h"
#include "NativeSaveFile.h"
namespace console {
// PS3 inner-save storage with legacy NBT and version-8 chunk records.
// Original-version selects the record family; container versions are preserved.
// Source filenames and codecs are connected;
// player loading, asynchronous save scheduling and Sony storage wrappers remain
// separate. File APIs persist the inner archive; they do not instantiate simulation.
class PS3WorldStorage {
    std::unique_ptr<ConsoleSaveArchive> archive;
    explicit PS3WorldStorage(std::unique_ptr<ConsoleSaveArchive> data):archive(std::move(data)){}
    std::unique_ptr<CompoundTag> metadataRoot()const;
public:
    PS3WorldStorage();
    static std::unique_ptr<PS3WorldStorage> read(std::span<const unsigned char> data);
    static std::unique_ptr<PS3WorldStorage> readFile(const std::filesystem::path& path);
    NativeSaveCommit writeFile(const std::filesystem::path& path);
    static std::wstring regionName(int dimension,int chunkX,int chunkZ);
    std::unique_ptr<LevelData> metadata()const;
    void putMetadata(LevelData& level,std::int64_t modified=SaveWire::now());
    std::unique_ptr<ChunkRecord> chunk(int dimension,int x,int z)const;
    void putChunk(int dimension,ChunkRecord& record,std::int64_t modified=SaveWire::now());
    std::vector<unsigned char> serialize(){return archive->serialize(SAVE_FILE_PLATFORM_PS3);}
    std::vector<std::wstring> names()const{return archive->names();}
    bool containsEntry(const std::wstring& name)const{return archive->contains(name);}
    void putEntry(const std::wstring& name,std::span<const unsigned char> bytes){archive->put(name,bytes);}
    std::span<const unsigned char> entry(const std::wstring& name)const{return archive->get(name);}
};
}
