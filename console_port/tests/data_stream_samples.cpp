#include "DataInputStream.h"
#include "DataOutputStream.h"
#include "InputOutputStream.h"
#include <iostream>
int main(){
    for(int value:{std::numeric_limits<int>::min(),-1,0,1,std::numeric_limits<int>::max()}){
        ByteArrayOutputStream output;DataOutputStream writer(&output);
        writer.writeBoolean(value!=0);writer.writeByte(static_cast<::byte>(value));
        writer.writeShort(static_cast<short>(value));writer.writeChar(static_cast<wchar_t>(value&65535));
        writer.writeInt(value);writer.writeLong(static_cast<std::int64_t>(value)*123456789LL);
        writer.writeFloat(std::bit_cast<float>(static_cast<std::uint32_t>(value)));
        writer.writeDouble(std::bit_cast<double>(0x8000000000000000ULL|static_cast<std::uint32_t>(value)));
        std::wstring text={L'A',0,L'\u00e9',L'\u4e16'};writer.writeUTF(text);writer.flush();
        std::uint64_t hash=14695981039346656037ULL;
        for(unsigned i=0;i<output.size();++i)hash=(hash^output.buf[i])*1099511628211ULL;
        std::cout<<hash<<' '<<output.size()<<' ';
        ByteArrayInputStream bytes(output.toByteArray());DataInputStream reader(&bytes);
        std::cout<<reader.readBoolean()<<' '<<int(reader.readByte())<<' '<<reader.readShort()<<' '<<int(reader.readChar())<<' '
                 <<reader.readInt()<<' '<<reader.readLong()<<' '
                 <<std::bit_cast<std::uint32_t>(reader.readFloat())<<' '<<std::bit_cast<std::uint64_t>(reader.readDouble());
        for(auto c:reader.readUTF())std::cout<<' '<<int(c);
        std::cout<<' '<<reader.read()<<'\n';
    }
}
