#pragma once
// Extracted from LevelStorage, McRegionChunkStorage and McRegionLevelStorage.
#include <array>
namespace console::savepath {
inline constexpr auto nether=L"DIM-1",end=L"DIM1/";
inline constexpr int regionVersion=0x4abc;
inline constexpr std::array<const wchar_t*,12> initialRegions{L"DIM-1r.-1.-1.mcr",L"DIM-1r.0.-1.mcr",L"DIM-1r.0.0.mcr",L"DIM-1r.-1.0.mcr",L"DIM1/r.-1.-1.mcr",L"DIM1/r.0.-1.mcr",L"DIM1/r.0.0.mcr",L"DIM1/r.-1.0.mcr",L"r.-1.-1.mcr",L"r.0.-1.mcr",L"r.0.0.mcr",L"r.-1.0.mcr"};
}
