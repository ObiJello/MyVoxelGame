#include "ConsoleSeed.h"
#include "BiomeGenerator.h"
#include <bit>
#include <charconv>
#include <stdexcept>
#include <string>
namespace console {
std::optional<std::int64_t> parseConsoleSeed(std::u16string_view text){
    if(text.size()>60)throw std::invalid_argument("Seed must contain at most 60 characters");
    if(text.empty())return std::nullopt;
    bool number=true;
    for(std::size_t i=0;i<text.size();++i)if((text[i]<u'0' || text[i]>u'9') && !(i==0 && text[i]==u'-')){number=false;break;}
    std::int64_t value=0;
    if(number){
        std::string ascii(text.begin(),text.end());
        auto parsed=std::from_chars(ascii.data(),ascii.data()+ascii.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=ascii.data()+ascii.size())throw std::invalid_argument("Numeric seed is outside the signed 64-bit range");
    }
    if(value!=0)return value;
    // Original console menu hashes non-numeric input and numeric zero as Java UTF-16.
    std::uint32_t hash=0;for(char16_t c:text)hash=31u*hash+c;
    return std::bit_cast<std::int32_t>(hash);
}
std::optional<std::int64_t> ConsoleSeedSearch::step(){
    auto seed=random_.nextLong();BiomeGenerator generator(seed);
    auto indices=generator.area(-100,-100,200,200,true);
    if(consoleSeedMatches(consoleBiomeFractions(indices)))return seed;
    return std::nullopt;
}
}
