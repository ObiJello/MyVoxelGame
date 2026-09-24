#include "ExperienceOrbRules.h"
#include <array>
namespace console {
namespace {
constexpr std::array<int,10> orbValues{2477,1237,617,307,149,73,37,17,7,3};
}
int sourceExperienceOrbValue(int remaining){
    if(remaining<=0)return 0;
    for(const int value:orbValues)if(remaining>=value)return value;
    return 1;
}
int sourceExperienceOrbIcon(int value){
    for(int i=0;i<int(orbValues.size());++i)
        if(value>=orbValues[i])return 10-i;
    return 0;
}
int sourceMobExperienceReward(std::wstring_view id,int slimeSize,int animalRoll){
    if(id==L"Cow" || id==L"Pig" || id==L"Sheep" || id==L"Chicken" ||
       id==L"Ozelot" || id==L"Wolf" || id==L"MushroomCow" || id==L"Squid")
        return 1+animalRoll%3;
    if(id==L"Blaze")return 10;
    if(id==L"Slime" || id==L"LavaSlime")return slimeSize;
    if(id==L"Zombie" || id==L"PigZombie" || id==L"Skeleton" ||
       id==L"Spider" || id==L"CaveSpider" || id==L"Silverfish" ||
       id==L"Creeper" || id==L"Ghast" || id==L"Enderman")return 5;
    return 0;
}
}
