#include "FileHeader.h"
#include <set>
FileEntry* FileHeader::AddFile(const wstring& name,unsigned length){
    for(auto& entry:fileTable)if(name==entry->data.filename)return entry.get();
    const auto start=GetStartOfNextData();
    if(length>PS3_MAX_SAVE_BYTES-GetFileSize() || GetFileSize()+length>PS3_MAX_SAVE_BYTES-SAVE_FILE_ENTRY_V2_SIZE)throw IoError("Save exceeds PS3 archive capacity");
    auto entry=std::make_unique<FileEntry>(name,length,start);fileTable.push_back(std::move(entry));lastFile=fileTable.back().get();return lastFile;
}
void FileHeader::RemoveFile(FileEntry* file){
    if(!file)return;
    auto it=std::find_if(fileTable.begin(),fileTable.end(),[&](auto& p){return p.get()==file;});
    if(it==fileTable.end())throw IoError("Save entry does not belong to this header");
    AdjustStartOffsets(file,file->getFileSize(),true);fileTable.erase(it);lastFile=fileTable.empty()?nullptr:fileTable.back().get();
}
unsigned FileHeader::GetStartOfNextData(){
    std::uint64_t total=SAVE_FILE_HEADER_SIZE;for(auto& entry:fileTable)total+=entry->getFileSize();
    if(total>PS3_MAX_SAVE_BYTES)throw IoError("Save data exceeds capacity");return static_cast<unsigned>(total);
}
unsigned FileHeader::GetFileSize(){
    std::uint64_t total=GetStartOfNextData()+static_cast<std::uint64_t>(m_saveVersion==1?SAVE_FILE_ENTRY_V1_SIZE:SAVE_FILE_ENTRY_V2_SIZE)*fileTable.size();
    if(total>PS3_MAX_SAVE_BYTES)throw IoError("Save table exceeds capacity");return static_cast<unsigned>(total);
}
void FileHeader::AdjustStartOffsets(FileEntry* file,unsigned bytes,bool subtract){
    auto it=std::find_if(fileTable.begin(),fileTable.end(),[&](auto& p){return p.get()==file;});
    if(it==fileTable.end())throw IoError("Save entry does not belong to this header");
    auto first=it+1;
    for(auto p=first;p!=fileTable.end();++p){auto& e=**p;
        if(subtract?(e.data.startOffset<bytes || e.currentFilePointer<bytes):(e.data.startOffset>UINT32_MAX-bytes || e.currentFilePointer>UINT32_MAX-bytes))throw IoError("Save offset overflow");}
    for(auto p=first;p!=fileTable.end();++p){auto& e=**p;if(subtract){e.data.startOffset-=bytes;e.currentFilePointer-=bytes;}else{e.data.startOffset+=bytes;e.currentFilePointer+=bytes;}}
}
bool FileHeader::fileExists(const wstring& name){for(auto& entry:fileTable)if(name==entry->data.filename)return true;return false;}
std::vector<FileEntry*>* FileHeader::getFilesWithPrefix(const wstring& prefix){
    std::unique_ptr<std::vector<FileEntry*>> result;
    for(auto& entry:fileTable)if(wstring(entry->data.filename).starts_with(prefix)){if(!result)result=std::make_unique<std::vector<FileEntry*>>();result->push_back(entry.get());}
    return result.release();
}
SaveByteOrder FileHeader::getEndian(ESavePlatform platform){
    switch(platform){
    case SAVE_FILE_PLATFORM_X360:case SAVE_FILE_PLATFORM_PS3:return SaveByteOrder::Big;
    case SAVE_FILE_PLATFORM_NONE:case SAVE_FILE_PLATFORM_XBONE:case SAVE_FILE_PLATFORM_PS4:case SAVE_FILE_PLATFORM_PSVITA:case SAVE_FILE_PLATFORM_WIN64:return SaveByteOrder::Little;
    default:throw IoError("Unknown console save platform");
    }
}
void FileHeader::WriteHeader(std::span<unsigned char> bytes){
    const auto offset=GetStartOfNextData();SaveWire::range(bytes.size(),0,GetFileSize());
    const unsigned version=m_saveVersion?m_saveVersion:SAVE_FILE_VERSION_NUMBER;
    const unsigned stride=version==1?SAVE_FILE_ENTRY_V1_SIZE:SAVE_FILE_ENTRY_V2_SIZE;
    SaveWire::write(bytes,0,4,offset,m_saveEndian);SaveWire::write(bytes,4,4,fileTable.size()*(version==1?stride:1),m_saveEndian);
    SaveWire::write(bytes,8,2,m_originalSaveVersion,m_saveEndian);SaveWire::write(bytes,10,2,version,m_saveEndian);
    std::size_t at=offset;
    for(auto& entry:fileTable){
        auto name=console::io::utf16Units(entry->data.filename);if(name.size()>=64)throw IoError("Save filename exceeds UTF-16 field");
        for(unsigned i=0;i<64;++i)SaveWire::write(bytes,at+i*2,2,i<name.size()?name[i]:0,m_saveEndian);
        SaveWire::write(bytes,at+128,4,entry->data.length,m_saveEndian);SaveWire::write(bytes,at+132,4,entry->data.startOffset,m_saveEndian);
        if(version>1)SaveWire::write(bytes,at+136,8,std::bit_cast<std::uint64_t>(entry->data.lastModifiedTime),m_saveEndian);at+=stride;
    }
}
void FileHeader::ReadHeader(std::span<const unsigned char> bytes,ESavePlatform platform){
    if(bytes.size()>PS3_MAX_SAVE_BYTES)throw IoError("Save exceeds PS3 archive capacity");
    auto endian=getEndian(platform);const auto offset=SaveWire::read(bytes,0,4,endian),tableSize=SaveWire::read(bytes,4,4,endian);
    const auto original=SaveWire::read(bytes,8,2,endian),version=SaveWire::read(bytes,10,2,endian);
    if(version<1 || version>SAVE_FILE_VERSION_NUMBER || original>version)throw IoError("Unsupported save-table version");
    unsigned stride=version==1?SAVE_FILE_ENTRY_V1_SIZE:SAVE_FILE_ENTRY_V2_SIZE;
    if(version==1 && tableSize%stride)throw IoError("Misaligned legacy save-table size");
    const auto count=version==1?tableSize/stride:tableSize;
    if(offset<SAVE_FILE_HEADER_SIZE || count>PS3_MAX_SAVE_BYTES/stride)throw IoError("Invalid save-table location or count");
    SaveWire::range(bytes.size(),offset,count*stride);
    std::vector<std::unique_ptr<FileEntry>> next;next.reserve(count);std::set<wstring> names;
    std::uint64_t expectedOffset=SAVE_FILE_HEADER_SIZE;
    for(std::uint64_t i=0;i<count;++i){
        auto at=offset+i*stride;std::vector<std::uint16_t> units;bool terminated=false;
        for(unsigned c=0;c<64;++c){auto unit=static_cast<std::uint16_t>(SaveWire::read(bytes,at+c*2,2,endian));if(!unit){terminated=true;break;}units.push_back(unit);}
        if(!terminated || units.empty())throw IoError("Invalid save-table filename");
        auto name=console::io::nativeString(units);if(!names.insert(name).second)throw IoError("Duplicate save-table filename");
        auto length=static_cast<unsigned>(SaveWire::read(bytes,at+128,4,endian)),start=static_cast<unsigned>(SaveWire::read(bytes,at+132,4,endian));
        if(start!=expectedOffset || start>offset || length>offset-start)throw IoError("Invalid or overlapping save-file range");expectedOffset+=length;
        auto entry=std::make_unique<FileEntry>(name,length,start);
        if(version>1)entry->data.lastModifiedTime=std::bit_cast<std::int64_t>(SaveWire::read(bytes,at+136,8,endian));next.push_back(std::move(entry));
    }
    if(expectedOffset!=offset)throw IoError("Save table does not follow its payloads");
    fileTable.swap(next);lastFile=fileTable.empty()?nullptr:fileTable.back().get();m_savePlatform=platform;m_saveEndian=endian;
    m_saveVersion=static_cast<short>(version);m_originalSaveVersion=static_cast<short>(original);
}
