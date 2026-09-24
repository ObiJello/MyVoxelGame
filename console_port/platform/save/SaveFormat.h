#pragma once
#include "IoCompat.h"
#include "ModifiedUtf.h"
#include "ConsoleSaveVersion.h"
#include <span>
#include <bit>
#include <chrono>
constexpr std::uint32_t saveFourCC(char a,char b,char c,char d){return static_cast<unsigned char>(a)|(static_cast<unsigned char>(b)<<8)|(static_cast<unsigned char>(c)<<16)|(static_cast<std::uint32_t>(static_cast<unsigned char>(d))<<24);}
enum ESavePlatform:std::uint32_t {
    SAVE_FILE_PLATFORM_NONE=saveFourCC('N','O','N','E'),SAVE_FILE_PLATFORM_X360=saveFourCC('X','3','6','0'),
    SAVE_FILE_PLATFORM_XBONE=saveFourCC('X','B','1','_'),SAVE_FILE_PLATFORM_PS3=saveFourCC('P','S','3','_'),
    SAVE_FILE_PLATFORM_PS4=saveFourCC('P','S','4','_'),SAVE_FILE_PLATFORM_PSVITA=saveFourCC('P','S','V','_'),
    SAVE_FILE_PLATFORM_WIN64=saveFourCC('W','I','N','_'),SAVE_FILE_PLATFORM_LOCAL=SAVE_FILE_PLATFORM_WIN64
};
enum class SaveByteOrder {Little,Big};
inline constexpr unsigned SAVE_FILE_HEADER_SIZE=12, SAVE_FILE_ENTRY_V1_SIZE=136,SAVE_FILE_ENTRY_V2_SIZE=144;
inline constexpr std::size_t PS3_MAX_SAVE_BYTES=64u*1024u*1024u;
namespace SaveWire {
inline void range(std::size_t length,std::size_t offset,std::size_t size){if(offset>length || size>length-offset)throw IoError("Save field outside buffer");}
inline std::uint64_t read(std::span<const unsigned char> bytes,std::size_t at,unsigned size,SaveByteOrder endian){
    range(bytes.size(),at,size);std::uint64_t value=0;for(unsigned i=0;i<size;++i){unsigned shift=endian==SaveByteOrder::Big?(size-1-i)*8:i*8;value|=static_cast<std::uint64_t>(bytes[at+i])<<shift;}return value;
}
inline void write(std::span<unsigned char> bytes,std::size_t at,unsigned size,std::uint64_t value,SaveByteOrder endian){
    range(bytes.size(),at,size);for(unsigned i=0;i<size;++i){unsigned shift=endian==SaveByteOrder::Big?(size-1-i)*8:i*8;bytes[at+i]=static_cast<unsigned char>(value>>shift);}
}
inline std::int64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
}
