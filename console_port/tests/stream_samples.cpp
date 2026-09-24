#include "InputOutputStream.h"
#include <iostream>
int main(){
    for(unsigned size:{1,32,1024})for(unsigned count:{0,1,31,32,33,1023,4097}){
        ByteArrayOutputStream output(size);BufferedOutputStream buffered(&output,13);
        for(unsigned i=0;i<count;++i)buffered.write((i*17+3)&255);
        buffered.close();
        byteArray bytes=output.toByteArray();ByteArrayInputStream input(bytes);
        std::uint64_t hash=14695981039346656037ULL;
        for(int b;(b=input.read())!=-1;)hash=(hash^b)*1099511628211ULL;
        std::cout<<size<<' '<<count<<' '<<output.size()<<' '<<hash<<'\n';
        output.reset();
        byteArray payload(97);for(unsigned i=0;i<payload.length;++i)payload[i]=i^0xa5;
        buffered.write(payload,5,78);buffered.write(payload,2,3);buffered.flush();delete[] payload.data;
        byteArray result=output.toByteArray();ByteArrayInputStream sliced(result,4,72);
        unsigned char scratch[19]{};byteArray target(scratch,19);hash=0;
        sliced.skip(7);
        for(int n;(n=sliced.read(target,2,13))!=-1;)for(int i=2;i<2+n;++i)hash=hash*31+scratch[i];
        std::cout<<output.size()<<' '<<hash<<'\n';
    }
}
