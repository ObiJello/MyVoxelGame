#pragma once
#include "ConsoleCompression.h"
#include <array>
#include <iostream>
#include <cstring>
using std::vector;
using std::find;
using DWORD=std::uint32_t;
constexpr int FILE_BEGIN=0,FILE_END=2;
#define _CONTENT_PACKAGE
#define PIXBeginNamedEvent(...) ((void)0)
#define PIXEndNamedEvent() ((void)0)
#define ZeroMemory(p,n) std::memset(p,0,n)
// In-memory equivalent of the ConsoleSaveFile read/write cursor. The reference
// uses native little endian; independent tests cover PS3 big-endian fields.
struct ReferenceFileEntry {};
struct ReferenceSaveFile {
    vector<unsigned char> bytes;
    std::size_t cursor=0;
    void setFilePointer(ReferenceFileEntry*,int at,void*,int origin){cursor=origin==FILE_END?bytes.size()+at:at;}
    void writeFile(ReferenceFileEntry*,const void* data,std::size_t count,DWORD* written){
        if(cursor+count>bytes.size())bytes.resize(cursor+count,0);
        if(count)std::memcpy(bytes.data()+cursor,data,count);cursor+=count;*written=count;
    }
    void zeroFile(ReferenceFileEntry*,std::size_t count,DWORD* written){
        if(cursor+count>bytes.size())bytes.resize(cursor+count,0);
        std::fill_n(bytes.begin()+cursor,count,0);cursor+=count;*written=count;
    }
    void LockSaveAccess(){}void ReleaseSaveAccess(){}
};
// Compression is shared only to isolate the original RegionFile allocator and
// byte writer from the separately tested RLE/DEFLATE subsystem.
struct Compression {
    static Compression* getCompression(){static Compression instance;return &instance;}
    void CompressLZXRLE(unsigned char* out,unsigned* size,unsigned char* in,unsigned length){
        auto bytes=console::compression::compressChunk(std::span(in,length),SAVE_FILE_PLATFORM_WIN64);
        if(bytes.size()>length+2048)throw IoError("Reference fixture exceeds original compression buffer");
        std::copy(bytes.begin(),bytes.end(),out);*size=bytes.size();
    }
};
class OriginalRegion {
    static constexpr int SECTOR_BYTES=4096,CHUNK_HEADER_SIZE=8;
    static byteArray emptySector;
    ReferenceSaveFile storage;
    ReferenceSaveFile* m_saveFile=&storage;
    ReferenceFileEntry entry;
    ReferenceFileEntry* fileEntry=&entry;
    std::array<int,1024> offsets{},chunkTimestamps{};
    vector<bool> freeStorage{false,false};
    vector<bool>* sectorFree=&freeStorage;
    int sizeDelta=0;
    bool m_bIsEmpty=false;
    std::int64_t referenceTimeMillis=0;
    void write(int x,int z,unsigned char* data,int length);
    void write(int sectorNumber,unsigned char* data,int length,unsigned compLength);
    void zero(int sectorNumber,int length);
    bool outOfBounds(int x,int z);
    int getOffset(int x,int z);
    void insertInitialSectors();
    void setOffset(int x,int z,int offset);
    void setTimestamp(int x,int z,int value);
public:
    // Start with the original constructor's existing, two-sector empty file.
    // The port's corrected lazy first allocation is tested independently.
    OriginalRegion(){storage.bytes.resize(8192,0);}
    int getSizeDelta();
    bool hasChunk(int x,int z);
    void put(int x,int z,std::span<const unsigned char> input,std::uint32_t time){
        referenceTimeMillis=static_cast<std::int64_t>(time)*1000;
        write(x,z,const_cast<unsigned char*>(input.data()),input.size());
    }
    std::span<const unsigned char> serialize()const{return storage.bytes;}
    int takeSizeDelta(){return getSizeDelta();}
};
