#include "ConsoleSaveArchive.h"
std::size_t ConsoleSaveArchive::indexOf(const wstring& name)const{for(std::size_t i=0;i<header.fileTable.size();++i)if(name==header.fileTable[i]->data.filename)return i;return contents.size();}
bool ConsoleSaveArchive::contains(const wstring& name)const{return indexOf(name)!=contents.size();}
std::span<const unsigned char> ConsoleSaveArchive::get(const wstring& name)const{auto index=indexOf(name);if(index==contents.size())throw IoError("Save entry not found");return contents[index];}
std::int64_t ConsoleSaveArchive::modifiedTime(const wstring& name)const{auto index=indexOf(name);if(index==contents.size())throw IoError("Save entry not found");return header.fileTable[index]->data.lastModifiedTime;}
std::vector<wstring> ConsoleSaveArchive::names()const{std::vector<wstring> result;for(auto& entry:header.fileTable)result.emplace_back(entry->data.filename);return result;}
void ConsoleSaveArchive::put(const wstring& name,std::span<const unsigned char> data,std::int64_t modified){
    const auto index=indexOf(name);const bool existing=index!=contents.size();
    std::uint64_t previous=existing?contents[index].size():0;
    if(data.size()>PS3_MAX_SAVE_BYTES || header.GetFileSize()-previous+data.size()+(existing?0:SAVE_FILE_ENTRY_V2_SIZE)>PS3_MAX_SAVE_BYTES)throw IoError("Save exceeds PS3 archive capacity");
    std::vector<unsigned char> next(data.begin(),data.end());
    FileEntry* entry;
    if(existing){
        entry=header.fileTable[index].get();auto old=entry->data.length;
        if(next.size()>=old)header.AdjustStartOffsets(entry,static_cast<unsigned>(next.size()-old));else header.AdjustStartOffsets(entry,static_cast<unsigned>(old-next.size()),true);
        contents[index]=std::move(next);entry->data.length=static_cast<unsigned>(data.size());entry->currentFilePointer=entry->data.startOffset;
    }else{
        contents.reserve(contents.size()+1);entry=header.AddFile(name,static_cast<unsigned>(data.size()));contents.push_back(std::move(next));
    }
    entry->data.lastModifiedTime=modified;
}
bool ConsoleSaveArchive::remove(const wstring& name){auto index=indexOf(name);if(index==contents.size())return false;header.RemoveFile(header.fileTable[index].get());contents.erase(contents.begin()+index);return true;}
std::vector<unsigned char> ConsoleSaveArchive::serialize(ESavePlatform platform){
    header.setPlatform(platform);std::vector<unsigned char> bytes(header.GetFileSize());
    for(std::size_t i=0;i<contents.size();++i)std::copy(contents[i].begin(),contents[i].end(),bytes.begin()+header.fileTable[i]->data.startOffset);
    header.WriteHeader(bytes);return bytes;
}
std::unique_ptr<ConsoleSaveArchive> ConsoleSaveArchive::read(std::span<const unsigned char> data,ESavePlatform platform){
    auto result=std::make_unique<ConsoleSaveArchive>();result->header.ReadHeader(data,platform);result->contents.reserve(result->header.fileTable.size());
    for(auto& entry:result->header.fileTable){auto begin=data.begin()+entry->data.startOffset;result->contents.emplace_back(begin,begin+entry->data.length);}return result;
}
