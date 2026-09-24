#pragma once
#include "ConsoleCompression.h"
#include <array>
#include <optional>
namespace console {
// CPU storage adapter for the original RegionFile. A region holds 32x32 chunks;
// its archive entry is still managed by the save owner. Returned spans are borrowed.
// Mutations are transactional; callers must serialize access to each instance.
class ConsoleRegionFile {
    ESavePlatform platform;
    SaveByteOrder endian;
    std::array<std::uint32_t,1024> offsets{},timestamps{};
    std::vector<bool> sectorFree{false,false};
    std::vector<unsigned char> bytes;
    std::size_t sizeDelta=8192;
    static unsigned index(int x,int z);
    void store(unsigned slot,std::span<const unsigned char> compressed,std::size_t decodedSize,std::uint32_t timestamp);
public:
    static constexpr unsigned sectorBytes=4096,headerBytes=8192,chunkHeaderBytes=8;
    explicit ConsoleRegionFile(ESavePlatform sourcePlatform=SAVE_FILE_PLATFORM_PS3);
    static ConsoleRegionFile read(std::span<const unsigned char> data,ESavePlatform sourcePlatform=SAVE_FILE_PLATFORM_PS3);
    bool hasChunk(int x,int z)const;
    std::uint32_t timestamp(int x,int z)const;
    std::optional<std::vector<unsigned char>> chunk(int x,int z)const;
    void put(int x,int z,std::span<const unsigned char> data,std::uint32_t timestamp=static_cast<std::uint32_t>(SaveWire::now()/1000));
    std::span<const unsigned char> serialize()const{return bytes;}
    std::size_t takeSizeDelta(){auto value=sizeDelta;sizeDelta=0;return value;}
};
}
