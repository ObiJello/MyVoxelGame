#include "ConsoleLzx.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
extern "C" {
#include "mspack.h"
#include "lzx.h"
}
namespace console {
namespace {
void ignoreMessage(mspack_file*,const char*,...){}
struct Memory {std::span<const unsigned char> input;std::span<unsigned char> output;std::size_t position=0;};
int readMemory(mspack_file* file,void* buffer,int count){auto& f=*reinterpret_cast<Memory*>(file);if(count<0)return -1;auto n=std::min(std::size_t(count),f.input.size()-f.position);std::memcpy(buffer,f.input.data()+f.position,n);f.position+=n;return int(n);}
int writeMemory(mspack_file* file,void* buffer,int count){auto& f=*reinterpret_cast<Memory*>(file);if(count<0 || std::size_t(count)>f.output.size()-f.position)return -1;std::memcpy(f.output.data()+f.position,buffer,count);f.position+=count;return count;}
}
std::vector<unsigned char> decodeConsoleLzx(std::span<const unsigned char> input,std::size_t maxOutput){
 if(input.empty() || input.size()>64*1024*1024 || maxOutput>64*1024*1024)throw std::runtime_error("Invalid LZX capacity");
 std::vector<unsigned char> packed;std::size_t offset=0,total=0;
 auto word=[&](){if(input.size()-offset<2)throw std::runtime_error("Truncated LZX frame header");int n=(input[offset]<<8)|input[offset+1];offset+=2;return n;};
 while(offset<input.size()){
  // XMem emits a five-byte zero terminator after its last frame.
  if(input.size()-offset==5 && std::all_of(input.begin()+offset,input.end(),[](auto b){return b==0;})){offset+=5;break;}
  int output=32768,compressed;
  if(input[offset]==255){++offset;output=word();compressed=word();}else compressed=word();
  if(output<=0 || output>32768 || compressed<=0 || std::size_t(compressed)>input.size()-offset || std::size_t(output)>maxOutput-total)
   throw std::runtime_error("Invalid LZX frame size");
  packed.insert(packed.end(),input.begin()+offset,input.begin()+offset+compressed);offset+=compressed;total+=output;
 }
 if(total==0)throw std::runtime_error("Empty LZX stream");
 std::vector<unsigned char> result(total);Memory source{packed,{},0},destination{{},result,0};
 mspack_system sys{};sys.read=readMemory;sys.write=writeMemory;
 sys.alloc=[](mspack_system*,std::size_t n)->void*{return std::malloc(n);};sys.free=std::free;
 sys.copy=[](void* src,void* dst,std::size_t n){std::memcpy(dst,src,n);};sys.message=ignoreMessage;
 std::unique_ptr<lzxd_stream,decltype(&lzxd_free)> decoder(lzxd_init(&sys,reinterpret_cast<mspack_file*>(&source),reinterpret_cast<mspack_file*>(&destination),17,0,4096,total,0),lzxd_free);
 if(!decoder || lzxd_decompress(decoder.get(),total)!=MSPACK_ERR_OK || destination.position!=total)throw std::runtime_error("Invalid console LZX data");
 return result;
}
}
