#pragma once
#include "IoCompat.h"
namespace console::io {
inline std::vector<std::uint16_t> utf16Units(const std::wstring& text){
    std::vector<std::uint16_t> units;units.reserve(text.size());
    for(wchar_t c:text){
        auto scalar=static_cast<std::uint32_t>(c);
        if(scalar<=0xffff)units.push_back(static_cast<std::uint16_t>(scalar));
        else if(scalar<=0x10ffff){scalar-=0x10000;units.push_back(0xd800+(scalar>>10));units.push_back(0xdc00+(scalar&1023));}
        else throw InvalidUtf("Wide string contains an invalid Unicode value");
    }
    return units;
}
template<class Next> std::uint16_t decodeUnit(unsigned first,Next next){
    if(first<0x80)return static_cast<std::uint16_t>(first);
    auto continuation=[&]{unsigned b=next();if((b&0xc0)!=0x80)throw InvalidUtf("Invalid modified UTF continuation");return b&63;};
    if((first&0xe0)==0xc0)return static_cast<std::uint16_t>(((first&31)<<6)|continuation());
    if((first&0xf0)==0xe0){unsigned b=continuation();return static_cast<std::uint16_t>(((first&15)<<12)|(b<<6)|continuation());}
    throw InvalidUtf("Invalid modified UTF lead byte");
}
inline std::wstring nativeString(const std::vector<std::uint16_t>& units){
    std::wstring result;
    for(std::size_t i=0;i<units.size();++i){
        auto value=std::uint32_t(units[i]);
        if constexpr(sizeof(wchar_t)>2){
            if(value>=0xd800 && value<=0xdbff && i+1<units.size() && units[i+1]>=0xdc00 && units[i+1]<=0xdfff){
                value=0x10000+((value-0xd800)<<10)+(units[++i]-0xdc00);
            }
        }
        result.push_back(static_cast<wchar_t>(value));
    }
    return result;
}
}
