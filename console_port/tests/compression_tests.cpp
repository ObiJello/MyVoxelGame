#include "ConsoleCompression.h"
#include "ChunkRecord.h"
#include <iostream>
#include <thread>
#include <atomic>
using namespace console::compression;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const IoError&){return;}throw std::runtime_error("Expected rejection");}
int main(){try{
    require(encodeRle({}).empty() && decodeRle({},0).empty(),"Empty RLE streams");
    std::vector<unsigned char> runs={255,1,255,255,1,255,255,255,1,7,7,7,7};require(encodeRle(runs)==std::vector<unsigned char>({255,0,1,255,1,1,255,2,1,255,3,7}),"RLE escaped literals and four-byte run boundary");
    rejects([&]{decodeRle(std::vector<unsigned char>{255},1);});rejects([&]{decodeRle(std::vector<unsigned char>{255,3},4);});rejects([&]{decodeRle(std::vector<unsigned char>{255,255,1},255);});rejects([&]{decodeRle(std::vector<unsigned char>{1},2);});
    for(auto platform:{SAVE_FILE_PLATFORM_PS3,SAVE_FILE_PLATFORM_PS4,SAVE_FILE_PLATFORM_WIN64,SAVE_FILE_PLATFORM_XBONE,SAVE_FILE_PLATFORM_PSVITA})for(unsigned size:{0u,1u,100u,131072u}){
        std::vector<unsigned char> data(size);for(unsigned i=0;i<size;++i)data[i]=static_cast<unsigned char>((i*137+i/17)%256);
        auto encoded=compressChunk(data,platform);require(decompressChunk(encoded,size,platform)==data,"Platform RLE plus deflate round trip, including original fixed-buffer overflow sizes");
        auto shortInput=encoded;shortInput.pop_back();rejects([&]{decompressChunk(shortInput,size,platform);});auto trailing=encoded;trailing.push_back(0);rejects([&]{decompressChunk(trailing,size,platform);});
        rejects([&]{decompressChunk(encoded,size+1,platform);});
    }
    std::vector<unsigned char> packageBytes(1024);for(std::size_t i=0;i<packageBytes.size();++i)packageBytes[i]=static_cast<unsigned char>(i/4);
    auto packageStream=compressChunk(packageBytes,SAVE_FILE_PLATFORM_PS3);
    require(decompressPs3Package(packageStream,packageBytes.size())==packageBytes,"PS3 package stream without alignment decodes");
    packageStream.push_back(0);
    require(decompressPs3Package(packageStream,packageBytes.size())==packageBytes,"PS3 EdgeZLib alignment byte decodes");
    packageStream.push_back(0);rejects([&]{decompressPs3Package(packageStream,packageBytes.size());});
    // Independent raw-DEFLATE stored-block fixture with the PS3 size prefix.
    std::vector<unsigned char> stored={0,0,0,3,1,3,0,252,255,'A','B','C'};
    require(decompressChunk(stored,3,SAVE_FILE_PLATFORM_PS3,false)==std::vector<unsigned char>({'A','B','C'}),"PS3 raw DEFLATE fixture");stored[3]=4;rejects([&]{decompressChunk(stored,4,SAVE_FILE_PLATFORM_PS3,false);});
    rejects([&]{decompressChunk(std::vector<unsigned char>{255,255,255,255},1,SAVE_FILE_PLATFORM_PS3);});rejects([&]{compressChunk({},SAVE_FILE_PLATFORM_X360);});
    std::atomic_bool failed=false;std::vector<std::thread> workers;for(int i=0;i<8;++i)workers.emplace_back([&,i]{try{std::vector<unsigned char> data(8192,i);for(int pass=0;pass<10;++pass){auto encoded=compressChunk(data,SAVE_FILE_PLATFORM_PS3);if(decompressChunk(encoded,data.size(),SAVE_FILE_PLATFORM_PS3)!=data)failed=true;}}catch(...){failed=true;}});for(auto& worker:workers)worker.join();require(!failed,"Compression contexts must be independent across workers");
    console::ChunkRecord record;record.lowerBlocks->set(3,62,4,24);ByteArrayOutputStream bytes;DataOutputStream out(&bytes);record.write(&out);auto payload=std::span(bytes.buf.data,bytes.size());auto packed=compressChunk(payload,SAVE_FILE_PLATFORM_PS3);auto restored=decompressChunk(packed,payload.size(),SAVE_FILE_PLATFORM_PS3);require(std::equal(payload.begin(),payload.end(),restored.begin()),"Original chunk record survives PS3 compression framing");
    std::cout<<"Console RLE, zlib/PS3 framing, malformed streams and concurrency passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
