#ifdef CONSOLE_RLE_REFERENCE
#include "OriginalRle.h"
#else
#include "ConsoleCompression.h"
#endif
#include <cstdint>
#include <vector>
#include <iostream>
int main(){
    for(unsigned mode=0;mode<5;++mode)for(unsigned size:{1u,2u,3u,4u,255u,256u,257u,4096u,32768u}){
        std::vector<unsigned char> input(size),encoded,decoded;
        for(unsigned i=0;i<size;++i)input[i]=mode==0?0:mode==1?255:mode==2?static_cast<unsigned char>(i):mode==3?static_cast<unsigned char>((i%2)?255:1):static_cast<unsigned char>((i/257)%256);
#ifdef CONSOLE_RLE_REFERENCE
        OriginalRle original;encoded.resize(size*2);unsigned encodedSize=encoded.size();original.CompressRLE(encoded.data(),&encodedSize,input.data(),input.size());encoded.resize(encodedSize);
        decoded.resize(size);unsigned decodedSize=decoded.size();original.DecompressRLE(decoded.data(),&decodedSize,encoded.data(),encoded.size());decoded.resize(decodedSize);
#else
        encoded=console::compression::encodeRle(input);decoded=console::compression::decodeRle(encoded,input.size());
#endif
        if(input!=decoded)return 1;std::uint64_t hash=14695981039346656037ull;for(auto b:encoded)hash=(hash^b)*1099511628211ull;std::cout<<mode<<' '<<size<<' '<<encoded.size()<<' '<<hash<<'\n';
    }
}
