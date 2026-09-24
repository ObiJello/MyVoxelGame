#pragma once
#include <array>
namespace console {
enum class TintKind { Grass,Foliage,Water };
int consoleBiomeTint(TintKind kind,const std::array<int,9>& biomes,int leafData=0);
}
