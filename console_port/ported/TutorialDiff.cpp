#include "TutorialDiff.h"
#include "io/IoCompat.h"
#include <algorithm>
#include <charconv>
#include <string>
namespace console {
namespace {
std::uint32_t number(std::span<const std::uint8_t> bytes,std::size_t& at,int width){
    if(at>bytes.size() || bytes.size()-at<std::size_t(width))
        throw IoError("Truncated tutorial difference");
    std::uint32_t value=0;
    for(int i=0;i<width;++i)value=(value<<8)|bytes[at++];
    return value;
}
std::pair<int,int> regionName(std::span<const std::uint8_t> bytes,std::size_t& at){
    std::string name;
    bool ended=false;
    for(int i=0;i<31;++i){
        const auto code=number(bytes,at,2);
        if(code==0){ended=true;continue;}
        if(ended || code<32 || code>126)throw IoError("Invalid tutorial region name");
        name+=char(code);
    }
    if(!ended)throw IoError("Unterminated tutorial region name");
    if(!name.starts_with("r.") || !name.ends_with(".mcr"))
        throw IoError("Invalid tutorial region coordinates");
    const auto coords=name.substr(2,name.size()-6);
    const auto split=coords.find('.');
    if(split==std::string::npos)throw IoError("Invalid tutorial region coordinates");
    int x=0,z=0;
    const auto [xEnd,xError]=std::from_chars(coords.data(),coords.data()+split,x);
    const auto [zEnd,zError]=std::from_chars(coords.data()+split+1,
                                            coords.data()+coords.size(),z);
    if(xError!=std::errc{} || zError!=std::errc{} ||
       xEnd!=coords.data()+split || zEnd!=coords.data()+coords.size() ||
       name!="r."+std::to_string(x)+"."+std::to_string(z)+".mcr" ||
       x<-1000000 || x>1000000 || z<-1000000 || z>1000000)
        throw IoError("Invalid tutorial region coordinates");
    return {x,z};
}
}
TutorialDiff TutorialDiff::read(std::span<const std::uint8_t> bytes){
    if(bytes.size()>1024*1024)throw IoError("Tutorial difference exceeds capacity");
    std::size_t at=0;
    const auto regions=number(bytes,at,4);
    if(!regions || regions>16)throw IoError("Invalid tutorial region count");
    TutorialDiff result;
    for(std::uint32_t region=0;region<regions;++region){
        const auto [rx,rz]=regionName(bytes,at);
        const auto chunks=number(bytes,at,4);
        if(!chunks || chunks>1024)throw IoError("Invalid tutorial chunk difference count");
        for(std::uint32_t chunk=0;chunk<chunks;++chunk){
            const auto index=number(bytes,at,2);
            const auto size=number(bytes,at,4);
            if(index>=1024 || !size || size>1024*1024 ||
               at>bytes.size() || size>bytes.size()-at)
                throw IoError("Invalid tutorial chunk difference");
            const auto end=at+size;
            const int x=rx*32+int(index%32),z=rz*32+int(index/32);
            auto [slot,inserted]=result.patches_.try_emplace({x,z});
            if(!inserted)throw IoError("Duplicate tutorial chunk difference");
            std::uint32_t previousEnd=0;
            while(at<end){
                if(end-at<5)throw IoError("Truncated tutorial byte edit");
                const auto offset=number(bytes,at,4);
                const auto count=number(bytes,at,1);
                if(!count || count>end-at || offset<previousEnd ||
                   offset>1024*1024-count)
                    throw IoError("Invalid tutorial byte edit");
                TutorialBytePatch edit;edit.offset=offset;
                edit.bytes.assign(bytes.begin()+at,bytes.begin()+at+count);
                at+=count;previousEnd=offset+count;
                slot->second.push_back(std::move(edit));
            }
        }
    }
    if(bytes.size()-at!=8 ||
       !std::all_of(bytes.begin()+at,bytes.end(),[](auto value){return value==0;}))
        throw IoError("Invalid tutorial difference trailer");
    return result;
}
const std::vector<TutorialBytePatch>* TutorialDiff::chunk(int x,int z)const{
    const auto it=patches_.find({x,z});
    return it==patches_.end()?nullptr:&it->second;
}
std::size_t TutorialDiff::patchCount()const{
    std::size_t total=0;
    for(const auto& [key,edits]:patches_)total+=edits.size();
    return total;
}
bool TutorialDiff::apply(int x,int z,std::vector<std::uint8_t>& record)const{
    const auto* edits=chunk(x,z);if(!edits)return false;
    for(const auto& edit:*edits)
        if(edit.offset>record.size() || edit.bytes.size()>record.size()-edit.offset)
            throw IoError("Tutorial edit exceeds source chunk record");
    auto next=record;
    for(const auto& edit:*edits)
        std::copy(edit.bytes.begin(),edit.bytes.end(),next.begin()+edit.offset);
    record.swap(next);
    return true;
}
}
