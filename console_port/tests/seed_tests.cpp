#include "ConsoleSeed.h"
#include "BiomeGenerator.h"
#include <iostream>
#include <limits>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>static void rejects(F action){try{action();}catch(const std::invalid_argument&){return;}throw std::runtime_error("Invalid seed input accepted");}
int main(){try{
    using namespace console;
    require(!parseConsoleSeed(u""),"Blank seed requests console search");
    require(parseConsoleSeed(u"12345")==12345 && parseConsoleSeed(u"-42")==-42,"Signed numeric seeds");
    require(parseConsoleSeed(u"0")==48 && parseConsoleSeed(u"-0")==1443,"Original zero text is hashed");
    require(parseConsoleSeed(u"hello")==99162322 && parseConsoleSeed(u"Minecraft")==-1595926131,"Original signed Java text hash");
    require(parseConsoleSeed(u"+1")==1382,"Plus-prefixed seed is text in the console menu");
    require(parseConsoleSeed(u"9223372036854775807")==std::numeric_limits<std::int64_t>::max() && parseConsoleSeed(u"-9223372036854775808")==std::numeric_limits<std::int64_t>::min(),"64-bit numeric boundaries");
    rejects([]{parseConsoleSeed(u"9223372036854775808");});rejects([]{parseConsoleSeed(u"-");});
    rejects([]{parseConsoleSeed(std::u16string(61,u'a'));});
    std::array<float,23> fractions{};
    for(int id:{0,1,2,4,5,6,14,21,7})fractions[id]=.01f;
    require(consoleSeedMatches(fractions),"All critical biomes and a ninth type match");
    auto missing=fractions;missing[7]=0;require(!consoleSeedMatches(missing),"Eight types are insufficient");
    auto boundary=fractions;boundary[0]=.15f;require(consoleSeedMatches(boundary),"Exactly 15 percent ocean is allowed");
    boundary[0]=.15001f;require(!consoleSeedMatches(boundary),"More than 15 percent ocean is rejected");
    boundary=fractions;boundary[14]=.001f;require(!consoleSeedMatches(boundary),"Presence threshold is strict");
    boundary[15]=.002f;require(consoleSeedMatches(boundary),"Mushroom shore can satisfy mushroom presence");
    boundary=fractions;boundary[2]=.0006f;boundary[17]=.0006f;require(!consoleSeedMatches(boundary),"Biome variants merge by maximum, not sum");
    std::vector<std::uint8_t> ids{0,0,1,2};auto shares=consoleBiomeFractions(ids);
    require(shares[0]==.5f && shares[1]==.25f && shares[2]==.25f,"Original biome fractions");
    rejects([]{consoleBiomeFractions({});});rejects([]{consoleBiomeFractions(std::array<std::uint8_t,1>{255});});
    ConsoleSeedSearch search(1234);Random reference(1234);
    for(int i=0;i<8;++i){auto seed=reference.nextLong();BiomeGenerator generator(seed);
        bool expected=consoleSeedMatches(consoleBiomeFractions(generator.area(-100,-100,200,200,true)));
        auto result=search.step();require(result.has_value()==expected && (!result || *result==seed),"Incremental search retains original candidate sequence and sample region");}
    std::optional<std::int64_t> found;
    for(int i=8;i<=83;++i){found=search.step();require(found.has_value()==(i==83),"Original candidate history reaches its first accepted seed at attempt 84");}
    require(found==std::int64_t(-5138114651764625079ll),"Deterministic accepted seed from the original search stream");
    std::cout<<"Console text seeds and original biome search rules passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
