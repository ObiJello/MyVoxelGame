#pragma once
#include "FileHeader.h"
// Native in-memory replacement for ConsoleSaveFileOriginal's contiguous virtual
// allocation. Serializes the inner archive passed to the PS3 StorageManager;
// Sony packaging, signing, profile filenames and other-platform compression remain separate.
class ConsoleSaveArchive {
    FileHeader header;
    std::vector<std::vector<unsigned char>> contents;
    std::size_t indexOf(const wstring& name)const;
public:
    void put(const wstring& name,std::span<const unsigned char> data,std::int64_t modified=SaveWire::now());
    bool remove(const wstring& name);
    bool contains(const wstring& name)const;
    std::span<const unsigned char> get(const wstring& name)const;
    std::int64_t modifiedTime(const wstring& name)const;
    std::vector<wstring> names()const;
    std::vector<unsigned char> serialize(ESavePlatform platform=SAVE_FILE_PLATFORM_PS3);
    static std::unique_ptr<ConsoleSaveArchive> read(std::span<const unsigned char> data,ESavePlatform platform=SAVE_FILE_PLATFORM_PS3);
    int originalVersion(){return header.getOriginalSaveVersion();}
    int readVersion(){return header.getSaveVersion();}
};
