#pragma once
#include <span>
#include <vector>
namespace console {
std::vector<unsigned char> decodeConsoleLzx(std::span<const unsigned char> input,std::size_t maxOutput);
}
