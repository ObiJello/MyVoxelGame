#include "ConsoleRegionFile.h"
#include "ConsoleSaveArchive.h"
#include "ChunkRecord.h"
#include <iostream>
#include <zlib.h>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
static std::vector<unsigned char> snapshot(const console::ConsoleRegionFile& region){auto s=region.serialize();return {s.begin(),s.end()};}
static std::vector<unsigned char> noise(unsigned n){std::vector<unsigned char> data(n);std::uint32_t state=98123;for(auto& b:data){state^=state<<13;state^=state>>17;state^=state<<5;b=static_cast<unsigned char>(state);}return data;}
int main(){try{
    using console::ConsoleRegionFile;
    ConsoleRegionFile empty;require(empty.serialize().empty() && empty.takeSizeDelta()==8192 && empty.takeSizeDelta()==0 && !empty.hasChunk(0,0) && !empty.chunk(31,31),"Console lazy empty region and missing chunk");
    rejects([&]{empty.put(-1,0,{});});rejects([&]{empty.chunk(0,32);});rejects([&]{empty.hasChunk(32,0);});
    auto a=noise(15000),b=noise(123),c=noise(22000);
    for(auto platform:{SAVE_FILE_PLATFORM_PS3,SAVE_FILE_PLATFORM_PS4,SAVE_FILE_PLATFORM_PSVITA,SAVE_FILE_PLATFORM_WIN64,SAVE_FILE_PLATFORM_XBONE}){
        ConsoleRegionFile region(platform);region.put(31,0,a,0xfedcba98u);region.put(0,31,b,123);region.put(31,31,{},456);
        auto original=snapshot(region);auto restored=ConsoleRegionFile::read(original,platform);
        require(restored.chunk(31,0)==a && restored.chunk(0,31)==b && restored.chunk(31,31)->empty() && restored.timestamp(31,0)==0xfedcba98u && restored.takeSizeDelta()==0,"Region platform round trip, unsigned timestamp and empty payload");
        require(snapshot(restored)==original,"Read does not rewrite sector padding or tables");
        restored.put(31,0,c,1000);restored.put(31,0,b,1001);require(restored.chunk(31,0)==b && restored.chunk(0,31)==b && restored.chunk(31,31)->empty(),"Chunk grow/shrink preserves neighboring sectors");
        auto expected=snapshot(restored);rejects([&]{restored.put(0,0,noise(1100000));});require(snapshot(restored)==expected,"Oversized compressed chunk fails transactionally");
    }
    ConsoleRegionFile ps3;ps3.put(0,0,b,0x01020304);auto bytes=snapshot(ps3);
    require(bytes.size()==12288 && bytes[0]==0 && bytes[1]==0 && bytes[2]==2 && bytes[3]==1 && bytes[4096]==1 && bytes[4099]==4 && bytes[8192]>=128 && bytes[8199]==123,"PS3 offset, timestamp, RLE bit and eight-byte chunk header");
    for(std::size_t cut:{1u,4095u,4096u,8191u,8192u})rejects([&]{ConsoleRegionFile::read(std::span(bytes).first(cut));});
    auto encodedSize=SaveWire::read(bytes,8192,4,SaveByteOrder::Big)&0x7fffffffu;
    auto shortSector=std::span(bytes).first(8200+encodedSize);
    require(snapshot(ConsoleRegionFile::read(shortSector))==bytes,"Original short final sector is padded when its complete chunk payload is present");
    rejects([&]{ConsoleRegionFile::read(shortSector.first(shortSector.size()-1));});
    auto bad=bytes;bad[3]=0;rejects([&]{ConsoleRegionFile::read(bad);});bad=bytes;bad[2]=1;rejects([&]{ConsoleRegionFile::read(bad);});bad=bytes;bad[2]=3;rejects([&]{ConsoleRegionFile::read(bad);});
    bad=bytes;std::copy_n(bad.begin(),4,bad.begin()+4);rejects([&]{ConsoleRegionFile::read(bad);});
    bad=bytes;SaveWire::write(bad,8192,4,0x80000ff9u,SaveByteOrder::Big);rejects([&]{ConsoleRegionFile::read(bad);}); // 4089 + 8 exceeds 4096.
    bad=bytes;SaveWire::write(bad,8196,4,PS3_MAX_SAVE_BYTES+1,SaveByteOrder::Big);rejects([&]{ConsoleRegionFile::read(bad);});
    bad=bytes;bad[8200]=255;auto corrupt=ConsoleRegionFile::read(bad);rejects([&]{corrupt.chunk(0,0);});
    // A hand-assembled historical PS3 chunk without RLE: raw DEFLATE stored
    // block, prefixed by EdgeZLib's big-endian uncompressed length.
    std::vector<unsigned char> legacy(12288);legacy[2]=2;legacy[3]=1;
    unsigned char encoded[]={0,0,0,3,1,3,0,252,255,'a','b','c'};
    SaveWire::write(legacy,8192,4,sizeof(encoded),SaveByteOrder::Big);SaveWire::write(legacy,8196,4,3,SaveByteOrder::Big);std::copy(std::begin(encoded),std::end(encoded),legacy.begin()+8200);
    auto old=ConsoleRegionFile::read(legacy);require(old.chunk(0,0)==std::vector<unsigned char>{'a','b','c'},"Pre-RLE chunk uses the legacy secondary codec directly");
    ConsoleRegionFile xbox(SAVE_FILE_PLATFORM_X360);rejects([&]{xbox.put(0,0,b);});require(xbox.serialize().empty(),"Unsupported LZX fails before modifying storage");
    // End-to-end PS3 inner archive -> region -> DEFLATE/RLE -> chunk record.
    console::ChunkRecord chunk;chunk.x=-1;chunk.z=32;chunk.lastUpdate=1ll<<40;chunk.upperBlocks->set(7,80,9,12);
    ByteArrayOutputStream output;DataOutputStream dataOutput(&output);chunk.write(&dataOutput);
    ConsoleRegionFile region;region.put(31,0,std::span(output.buf.data,output.size()),222);
    ConsoleSaveArchive archive;archive.put(L"r.-1.1.mcr",region.serialize(),333);auto loaded=ConsoleSaveArchive::read(archive.serialize());
    auto loadedRegion=ConsoleRegionFile::read(loaded->get(L"r.-1.1.mcr"));auto payload=loadedRegion.chunk(31,0);
    ByteArrayInputStream input(byteArray(payload->data(),payload->size()));struct Detach{ByteArrayInputStream& input;~Detach(){input.reset();}}detach{input};DataInputStream dataInput(&input);auto decoded=console::ChunkRecord::read(&dataInput);
    require(decoded->x==-1 && decoded->z==32 && decoded->lastUpdate==(1ll<<40) && decoded->upperBlocks->get(7,80,9)==12,"Original chunk record survives the complete inner save storage path");
    std::cout<<"Region layout, transactions, validation, legacy compression and archive integration passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
