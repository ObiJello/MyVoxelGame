#include "DataInputStream.h"
#include "DataOutputStream.h"
#include "InputOutputStream.h"
#include "BasicTypeContainers.h"
#include <iostream>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct CountedOutput:DataOutputStream{using DataOutputStream::DataOutputStream;int count()const{return written;}};
static std::unique_ptr<ByteArrayInputStream> bytes(const std::vector<unsigned char>& values){
    byteArray array(static_cast<unsigned>(values.size()));std::copy(values.begin(),values.end(),array.data);
    return std::make_unique<ByteArrayInputStream>(array);
}
int main(){try{
    ByteArrayOutputStream output;CountedOutput writer(&output);
    writer.writeLong(std::numeric_limits<std::int64_t>::min());writer.writeFloat(-0.0f);
    std::uint64_t nanBits=0x7ff8000000000042ULL;writer.writeDouble(std::bit_cast<double>(nanBits));writer.writeByte(255);
    require(writer.count()==21 && output.size()==21,"Primitive output byte count is wrong");
    require(output.buf[0]==128 && output.buf[7]==0 && output.buf[8]==128 && output.buf[11]==0,"Big-endian scalar encoding differs");
    ByteArrayInputStream buffer(output.toByteArray());DataInputStream reader(&buffer);
    require(reader.readLong()==std::numeric_limits<std::int64_t>::min(),"Signed 64-bit minimum was corrupted");
    require(std::bit_cast<std::uint32_t>(reader.readFloat())==0x80000000u,"Negative zero lost its sign");
    require(std::bit_cast<std::uint64_t>(reader.readDouble())==nanBits && reader.readUnsignedByte()==255,"NaN payload or unsigned byte changed");
    bool eof=false;try{reader.readBoolean();}catch(const EndOfStream&){eof=true;}require(eof,"EOF was treated as true");
    for(unsigned n=0;n<8;++n){auto shortBuffer=bytes(std::vector<unsigned char>(n));DataInputStream input(shortBuffer.get());eof=false;
        try{input.readLong();}catch(const EndOfStream&){eof=true;}require(eof,"Truncated integer was accepted");}
    output.reset();DataOutputStream strings(&output);
    std::wstring message={L'A',0,L'\u00e9',L'\U0001f30d'};strings.writeUTF(message);
    const std::vector<unsigned char> expected={0,11,65,0xc0,0x80,0xc3,0xa9,0xed,0xa0,0xbc,0xed,0xbc,0x8d};
    require(output.size()==expected.size() && std::equal(expected.begin(),expected.end(),output.buf.data),"Modified UTF wire encoding differs");
    ByteArrayInputStream utfBuffer(output.toByteArray());DataInputStream utf(&utfBuffer);require(utf.readUTF()==message,"Native wide Unicode roundtrip failed");
    for(auto malformed:std::vector<std::vector<unsigned char>>{{0,1,0x80},{0,2,0xe2,0x80},{0,2,0xc2,0x41},{0,4,0xf0,0x9f,0x8c,0x8d}}){
        auto bad=bytes(malformed);DataInputStream input(bad.get());bool rejected=false;try{input.readUTF();}catch(const InvalidUtf&){rejected=true;}
        require(rejected,"Malformed modified UTF was accepted");
    }
    auto truncated=bytes({0,3,0xe2,0x80});DataInputStream shortUtf(truncated.get());eof=false;
    try{shortUtf.readUTF();}catch(const EndOfStream&){eof=true;}require(eof,"Truncated UTF payload was accepted");
    auto prior=output.size();bool rejected=false;try{strings.writeUTF(std::wstring(65536,L'a'));}catch(const InvalidUtf&){rejected=true;}
    require(rejected && output.size()==prior,"Oversized UTF changed the output stream");
    output.reset();strings.writeChars(L"\U0001f30d");ByteArrayInputStream charBuffer(output.toByteArray());DataInputStream chars(&charBuffer);
    require(chars.readChar()==0xd83c && chars.readChar()==0xdf0d,"writeChars did not emit UTF-16 units");
    require(std::isinf(Float::POSITIVE_INFINITY) && Double::isInfinite(-std::numeric_limits<double>::infinity()),"Original infinity helper defects remain");
    auto* owned=new ByteArrayInputStream(byteArray(1));DataInputStream detached(owned);detached.deleteChildStream();detached.deleteChildStream();
    rejected=false;try{detached.readInt();}catch(const IoError&){rejected=true;}require(rejected,"Deleted child stream was dereferenced");
    std::cout<<"Passed big-endian primitives, signed limits, IEEE bits, UTF, truncation and child ownership checks\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
