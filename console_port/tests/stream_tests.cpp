#include "InputOutputStream.h"
#include <iostream>
#include <type_traits>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Sink:ByteArrayOutputStream{int flushes=0,closes=0;void flush()override{++flushes;}void close()override{++closes;}};
int main(){try{
    static_assert(!std::is_copy_constructible_v<ByteArrayInputStream>);
    static_assert(!std::is_copy_constructible_v<ByteArrayOutputStream>);
    static_assert(!std::is_copy_constructible_v<BufferedOutputStream>);
    ByteArrayOutputStream output(0);
    for(unsigned i=0;i<8;++i)output.write(i);
    byteArray slice(output.buf.data+2,4);output.write(slice);
    require(output.size()==12,"Zero-capacity growth or self-append lost data");
    for(unsigned i=0;i<4;++i)require(output.buf[8+i]==i+2,"Self-append read a freed buffer");
    unsigned before=output.size();bool rejected=false;
    try{output.write(output.buf,output.buf.length,1);}catch(const std::out_of_range&){rejected=true;}
    require(rejected && output.size()==before,"Invalid range changed output");
    ByteArrayInputStream input(output.toByteArray());
    require(input.skip(-1)==0 && input.read()==0,"Negative skip moved the input cursor");
    require(input.skip(std::numeric_limits<std::int64_t>::max())==11,"Large skip overflowed");
    require(input.read()==-1 && input.read(byteArray())==0,"EOF zero-length read differs from stream contract");
    unsigned char destination[2]{};rejected=false;
    try{input.read(byteArray(destination,2),1,2);}catch(const std::out_of_range&){rejected=true;}
    require(rejected,"Invalid destination read was accepted");
    byteArray owned(3);owned[0]=41;owned[1]=42;owned[2]=43;
    {ByteArrayInputStream borrowed(owned);require(borrowed.read()==41,"Borrowed stream read differs");borrowed.reset();}
    require(owned[2]==43,"Reset did not detach borrowed input storage");delete[] owned.data;
    Sink sink;{BufferedOutputStream buffered(&sink,2);buffered.write(7);buffered.flush();buffered.write(8);buffered.close();}
    require(sink.size()==2 && sink.buf[0]==7 && sink.buf[1]==8,"Buffered close lost bytes");
    require(sink.flushes==2 && sink.closes==1,"Flush or close did not reach downstream stream");
    rejected=false;try{BufferedOutputStream invalid(&sink,0);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"Zero-capacity buffered stream was accepted");
    unsigned char data[3]={1,2,3};ByteArrayInputStream bounded(byteArray(new unsigned char[3]{1,2,3},3),1,std::numeric_limits<unsigned>::max());
    require(bounded.read()==2 && bounded.read()==3 && bounded.read()==-1,"Input constructor range overflowed");
    (void)data;
    std::cout<<"Passed stream ownership, growth, aliasing, range, EOF, skip and buffered-flush checks\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
