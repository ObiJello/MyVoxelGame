#pragma once
#include <cstdint>

namespace console {

struct SpawnEggColors {
    std::uint32_t base = 0xffffff;
    std::uint32_t spots = 0xffffff;
    bool valid = false;
    const wchar_t* entityName = nullptr;
};

SpawnEggColors consoleSourceEggColors(int entityId);

}
