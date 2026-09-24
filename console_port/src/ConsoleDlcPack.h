#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>
namespace console {
struct ConsoleDlcEntry {
    std::uint32_t type;
    std::u16string name;
    std::map<std::uint32_t,std::u16string> parameters;
    std::vector<unsigned char> bytes;
};
// Original version-3 DLC wire container. Platform mounting/entitlements are separate.
class ConsoleDlcPack {
public:
    static ConsoleDlcPack read(std::span<const unsigned char> bytes);
    std::map<std::uint32_t,std::u16string> parameterNames;
    std::vector<ConsoleDlcEntry> entries;
};
}
