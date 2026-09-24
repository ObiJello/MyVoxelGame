#include "ConsoleRegionFile.h"
namespace console {
ConsoleRegionFile::ConsoleRegionFile(ESavePlatform sourcePlatform):platform(sourcePlatform){
    switch(platform){
    case SAVE_FILE_PLATFORM_PS3:case SAVE_FILE_PLATFORM_X360:endian=SaveByteOrder::Big;break;
    case SAVE_FILE_PLATFORM_WIN64:case SAVE_FILE_PLATFORM_PS4:case SAVE_FILE_PLATFORM_PSVITA:case SAVE_FILE_PLATFORM_XBONE:endian=SaveByteOrder::Little;break;
    default:throw IoError("Unknown region platform");
    }
}
unsigned ConsoleRegionFile::index(int x,int z){
    if(x<0 || x>=32 || z<0 || z>=32)throw IoError("Region-local chunk coordinates outside 0..31");
    return static_cast<unsigned>(x+z*32);
}
ConsoleRegionFile ConsoleRegionFile::read(std::span<const unsigned char> data,ESavePlatform sourcePlatform){
    ConsoleRegionFile result(sourcePlatform);
    if(data.empty())return result; // The console creates its two header sectors lazily.
    if(data.size()<headerBytes || data.size()>PS3_MAX_SAVE_BYTES)throw IoError("Invalid region size");
    // Original lazy first writes can leave the final sector short. Like its
    // constructor, pad that sector, but require the complete payload to exist.
    const auto paddedSize=(data.size()+sectorBytes-1)/sectorBytes*sectorBytes;
    result.sectorFree.assign(paddedSize/sectorBytes,true);result.sectorFree[0]=result.sectorFree[1]=false;
    for(unsigned i=0;i<1024;++i){
        auto offset=static_cast<std::uint32_t>(SaveWire::read(data,i*4,4,result.endian));
        result.offsets[i]=offset;result.timestamps[i]=static_cast<std::uint32_t>(SaveWire::read(data,sectorBytes+i*4,4,result.endian));
        if(!offset)continue;
        const auto start=offset>>8,count=offset&255;
        if(start<2 || !count || start>=result.sectorFree.size() || count>result.sectorFree.size()-start)throw IoError("Invalid chunk sector range");
        for(unsigned j=0;j<count;++j){if(!result.sectorFree[start+j])throw IoError("Overlapping chunk sectors");result.sectorFree[start+j]=false;}
        auto at=static_cast<std::size_t>(start)*sectorBytes;
        auto length=SaveWire::read(data,at,4,result.endian)&0x7fffffffu;
        auto decoded=SaveWire::read(data,at+4,4,result.endian);
        if(!length || length>count*sectorBytes-chunkHeaderBytes || decoded>PS3_MAX_SAVE_BYTES)throw IoError("Invalid chunk payload length");
        SaveWire::range(data.size(),at+chunkHeaderBytes,length);
    }
    result.bytes.assign(data.begin(),data.end());result.bytes.resize(paddedSize,0);result.sizeDelta=0;return result;
}
bool ConsoleRegionFile::hasChunk(int x,int z)const{return offsets[index(x,z)]!=0;}
std::uint32_t ConsoleRegionFile::timestamp(int x,int z)const{return timestamps[index(x,z)];}
std::optional<std::vector<unsigned char>> ConsoleRegionFile::chunk(int x,int z)const{
    auto offset=offsets[index(x,z)];if(!offset)return std::nullopt;
    auto at=static_cast<std::size_t>(offset>>8)*sectorBytes;
    auto length=static_cast<std::uint32_t>(SaveWire::read(bytes,at,4,endian));
    auto decoded=static_cast<std::size_t>(SaveWire::read(bytes,at+4,4,endian));
    return compression::decompressChunk(std::span(bytes).subspan(at+chunkHeaderBytes,length&0x7fffffffu),decoded,platform,(length&0x80000000u)!=0);
}
void ConsoleRegionFile::put(int x,int z,std::span<const unsigned char> data,std::uint32_t time){
    auto slot=index(x,z);auto compressed=compression::compressChunk(data,platform);
    // Preserve the original extra sector at an exact boundary. Compression and
    // allocation finish on a candidate before any live table or payload changes.
    if((compressed.size()+chunkHeaderBytes)/sectorBytes+1>=256)throw IoError("Chunk exceeds the console region sector limit");
    auto next=*this;next.store(slot,compressed,data.size(),time);*this=std::move(next);
}
void ConsoleRegionFile::store(unsigned slot,std::span<const unsigned char> compressed,std::size_t decodedSize,std::uint32_t time){
    const auto needed=(compressed.size()+chunkHeaderBytes)/sectorBytes+1;
    auto offset=offsets[slot];std::size_t start=offset>>8,allocated=offset&255;
    if(bytes.empty())bytes.resize(headerBytes,0);
    if(!start || allocated!=needed){
        for(std::size_t i=0;i<allocated;++i)sectorFree[start+i]=true;
        std::fill_n(bytes.begin()+start*sectorBytes,allocated*sectorBytes,0);
        std::size_t runStart=0,runLength=0;
        // Original first-fit scan, including holes made by the current rewrite.
        for(std::size_t i=2;i<sectorFree.size();++i){
            if(!sectorFree[i])runLength=0;
            else {if(!runLength)runStart=i;++runLength;if(runLength>=needed)break;}
        }
        if(runLength>=needed){start=runStart;for(std::size_t i=0;i<needed;++i)sectorFree[start+i]=false;}
        else {
            start=sectorFree.size();
            if(needed>PS3_MAX_SAVE_BYTES/sectorBytes-start)throw IoError("Region exceeds save capacity");
            sectorFree.resize(start+needed,false);bytes.resize((start+needed)*sectorBytes,0);sizeDelta+=needed*sectorBytes;
        }
        offsets[slot]=static_cast<std::uint32_t>((start<<8)|needed);
        SaveWire::write(bytes,slot*4,4,offsets[slot],endian);
    }
    const auto at=start*sectorBytes;
    SaveWire::write(bytes,at,4,compressed.size()|0x80000000u,endian);SaveWire::write(bytes,at+4,4,decodedSize,endian);
    std::copy(compressed.begin(),compressed.end(),bytes.begin()+at+chunkHeaderBytes);
    // Match original overwrite semantics: padding in a reused allocation is
    // unchanged; released allocations are zeroed by the branch above.
    timestamps[slot]=time;SaveWire::write(bytes,sectorBytes+slot*4,4,time,endian);
}
}
