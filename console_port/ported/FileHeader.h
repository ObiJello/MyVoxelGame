// Ported file-table semantics from FileHeader.h/.cpp. Wire fields are explicitly
// marshalled as UTF-16 and fixed-width integers instead of host struct dumps.
#pragma once
#include "SaveFormat.h"
struct FileEntrySaveData {
    wchar_t filename[64]{};
    std::uint32_t length=0;
    union {std::uint32_t startOffset=0;std::uint32_t regionIndex;};
    std::int64_t lastModifiedTime=0;
};
class FileEntry {
public:
    FileEntrySaveData data;
    std::uint32_t currentFilePointer=0;
    FileEntry()=default;
    FileEntry(const wstring& name,std::uint32_t length,std::uint32_t offset){
        auto units=console::io::utf16Units(name);if(name.empty() || units.size()>=64 || name.find(L'\0')!=wstring::npos)throw IoError("Invalid save filename");
        std::copy(name.begin(),name.end(),data.filename);data.length=length;data.startOffset=offset;currentFilePointer=offset;
    }
    unsigned getFileSize(){return data.length;}
    bool isRegionFile(){return data.filename[0]==0;}
    unsigned getRegionFileIndex(){return data.regionIndex;}
    void updateLastModifiedTime(){data.lastModifiedTime=SaveWire::now();}
    static bool newestFirst(FileEntry* a,FileEntry* b){return a->data.lastModifiedTime>b->data.lastModifiedTime;}
};
class ConsoleSaveArchive;
class FileHeader {
    friend class ConsoleSaveArchive;
    std::vector<std::unique_ptr<FileEntry>> fileTable;
    ESavePlatform m_savePlatform=SAVE_FILE_PLATFORM_LOCAL;
    SaveByteOrder m_saveEndian=SaveByteOrder::Little;
    short m_saveVersion=0,m_originalSaveVersion=SAVE_FILE_VERSION_NUMBER;
public:
    FileEntry* lastFile=nullptr;
    FileHeader()=default;
    FileHeader(const FileHeader&)=delete;
    FileHeader& operator=(const FileHeader&)=delete;
protected:
    FileEntry* AddFile(const wstring& name,unsigned length=0);
    void RemoveFile(FileEntry* file);
    void WriteHeader(std::span<unsigned char> bytes);
    void ReadHeader(std::span<const unsigned char> bytes,ESavePlatform platform=SAVE_FILE_PLATFORM_LOCAL);
    unsigned GetStartOfNextData();
    unsigned GetFileSize();
    void AdjustStartOffsets(FileEntry* file,unsigned bytes,bool subtract=false);
    bool fileExists(const wstring& name);
    std::vector<FileEntry*>* getFilesWithPrefix(const wstring& prefix);
    void setSaveVersion(int version){m_saveVersion=static_cast<short>(version);}
    int getSaveVersion(){return m_saveVersion;}
    void setOriginalSaveVersion(int version){if(version<1 || version>SAVE_FILE_VERSION_NUMBER)throw IoError("Invalid original save version");m_originalSaveVersion=static_cast<short>(version);}
    int getOriginalSaveVersion(){return m_originalSaveVersion;}
    ESavePlatform getSavePlatform(){return m_savePlatform;}
    void setPlatform(ESavePlatform platform){m_saveEndian=getEndian(platform);m_savePlatform=platform;}
    SaveByteOrder getSaveEndian(){return m_saveEndian;}
    void setEndian(SaveByteOrder endian){m_saveEndian=endian;}
    static SaveByteOrder getEndian(ESavePlatform platform);
};
