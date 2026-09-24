#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <array>
#include <memory>
using namespace std;
using LPVOID=void*;
constexpr unsigned SAVE_FILE_HEADER_SIZE=12,SAVE_FILE_VERSION_NUMBER=8;
// The archived struct's UTF-16 ABI, rather than macOS's 32-bit wchar_t ABI.
struct FileEntrySaveData {char16_t filename[64]{};unsigned length=0,startOffset=0;std::int64_t lastModifiedTime=0;};
static_assert(sizeof(FileEntrySaveData)==144);
struct FileEntry {FileEntrySaveData data;unsigned getFileSize(){return data.length;}};
class FileHeaderWire {
public:
    vector<FileEntry*> fileTable;short m_originalSaveVersion=8;
    void WriteHeader(LPVOID);
    unsigned GetStartOfNextData();
    unsigned GetFileSize();
};
