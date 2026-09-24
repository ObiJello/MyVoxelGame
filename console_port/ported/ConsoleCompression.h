#pragma once
#include "SaveFormat.h"
namespace console::compression {
// The 4J byte-run format and the platform-selected second compression stage.
std::vector<unsigned char> encodeRle(std::span<const unsigned char> source);
std::vector<unsigned char> decodeRle(std::span<const unsigned char> source,std::size_t expectedBytes);
std::vector<unsigned char> compressChunk(std::span<const unsigned char> source,ESavePlatform platform);
std::vector<unsigned char> decompressChunk(std::span<const unsigned char> source,std::size_t expectedBytes,ESavePlatform platform,bool useRle=true);
// PS3 content-package streams may have one zero byte of EdgeZLib alignment.
std::vector<unsigned char> decompressPs3Package(std::span<const unsigned char> source,std::size_t expectedBytes);
}
