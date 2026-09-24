#pragma once
#include <string_view>
namespace console {
int sourceExperienceOrbValue(int remaining);
int sourceExperienceOrbIcon(int value);
int sourceMobExperienceReward(std::wstring_view id,int slimeSize,int animalRoll);
}
