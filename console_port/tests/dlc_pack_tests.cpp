#include "ConsoleDlcPack.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void rejects(std::span<const unsigned char> bytes){try{console::ConsoleDlcPack::read(bytes);}catch(const std::runtime_error&){return;}throw std::runtime_error("Invalid DLC accepted");}
int main(int argc,char** argv){try{
    require(argc==2,"Original tutorial path required");std::ifstream input(argv[1],std::ios::binary);
    require(bool(input),"Original tutorial package opens");std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});
    auto tutorial=console::ConsoleDlcPack::read(bytes);
    require(tutorial.parameterNames.empty() && tutorial.entries.size()==2,"Original tutorial entry table");
    require(tutorial.entries[0].type==6 && tutorial.entries[0].name==u"languages.loc" && tutorial.entries[0].bytes.size()==2138,"Original PS3 language payload");
    require(tutorial.entries[1].type==7 && tutorial.entries[1].name==u"GameRules.grf" && tutorial.entries[1].bytes.size()==97434,"Original PS3 game-rule payload");
    require(tutorial.entries[1].bytes[0]==0 && tutorial.entries[1].bytes[1]==1 && tutorial.entries[1].bytes[2]==4,"PS3 game-rule version and compression header retained");
    for(bool big:{false,true}){
        std::vector<unsigned char> wire;
        auto integer=[&](std::uint32_t n){for(int i=0;i<4;++i)wire.push_back(n>>(8*(big?3-i:i)));};
        auto string=[&](std::u16string value){integer(value.size());for(char16_t c:value){wire.push_back(big?c>>8:c);wire.push_back(big?c:c>>8);}integer(0);};
        integer(3);integer(1);integer(42);string(u"odd");integer(2);
        integer(3);integer(7);string(u"rules");integer(0);integer(6);string(u"empty");
        integer(1);integer(42);string(u"value");wire.insert(wire.end(),{0,128,255});integer(0);
        auto pack=console::ConsoleDlcPack::read(wire);
        require(pack.parameterNames.at(42)==u"odd" && pack.entries[0].parameters.at(42)==u"value","Unaligned UTF-16 parameter tables on both endian platforms");
        require(pack.entries[0].bytes==std::vector<unsigned char>({0,128,255}) && pack.entries[1].bytes.empty(),"Binary payload and zero-sized entry");
        for(std::size_t size=0;size<wire.size();++size)rejects(std::span(wire).first(size));
        wire.push_back(0);rejects(wire);wire.pop_back();wire[0]=9;rejects(wire);
    }
    auto oversized=bytes;oversized[8]=255;rejects(oversized);
    auto corruptName=bytes;corruptName[24]=corruptName[25]=0;rejects(corruptName);
    auto shortPayload=bytes;shortPayload.pop_back();rejects(shortPayload);
    std::cout<<"Original tutorial DLC, endian tables and malformed boundaries passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
