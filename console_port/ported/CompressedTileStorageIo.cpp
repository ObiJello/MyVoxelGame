// Original wire format: big-endian byte count, little-endian indices, packed data.
#include "CompressedTileStorage.h"
void CompressedTileStorage::write(DataOutputStream* dos){
    std::lock_guard guard(cs_write);
    if(!dos)throw IoError("Null compressed chunk output");
    dos->writeInt(allocatedSize);if(!allocatedSize)return;
    const auto* indices=reinterpret_cast<const unsigned short*>(indicesAndData);
    for(int i=0;i<512;++i){dos->writeByte(indices[i]&255);dos->writeByte(indices[i]>>8);}
    dos->write(byteArray(indicesAndData+1024,allocatedSize-1024));
}
void CompressedTileStorage::read(DataInputStream* dis){
    if(!dis)throw IoError("Null compressed chunk input");
    int size=dis->readInt();
    if(size<0 || size>33792 || (size>0 && size<1024))throw IoError("Invalid compressed chunk byte count");
    CompressedTileStorage next;
    if(size){
        next.indicesAndData=static_cast<unsigned char*>(XPhysicalAlloc(size,0,4096,0));next.allocatedSize=size;
        auto* indices=reinterpret_cast<unsigned short*>(next.indicesAndData);
        for(int i=0;i<512;++i){unsigned low=dis->readUnsignedByte();unsigned high=dis->readUnsignedByte();indices[i]=static_cast<unsigned short>(low|(high<<8));}
        if(!dis->readFully(byteArray(next.indicesAndData+1024,size-1024)))throw EndOfStream();
        std::array<bool,32768> occupied{};
        for(int block=0;block<512;++block){
            unsigned index=indices[block],type=index&3;
            if(type==3 && (index&4)){if((index>>8)==255)throw IoError("Reserved palette tile");continue;}
            unsigned offset=(index>>1)&0x7ffe,bytes=type==3?64:(8u<<type)+(1u<<(1u<<type));
            if(offset>static_cast<unsigned>(size-1024) || bytes>static_cast<unsigned>(size-1024)-offset)throw IoError("Compressed palette offset outside payload");
            for(unsigned i=offset;i<offset+bytes;++i){if(occupied[i])throw IoError("Overlapping compressed palettes");occupied[i]=true;}
            auto* packed=next.indicesAndData+1024+offset;
            for(unsigned tile=0;tile<64;++tile){
                unsigned value;
                if(type==3)value=packed[tile];
                else {unsigned bits=1u<<type,colors=1u<<bits;unsigned position=tile*bits;value=packed[(packed[colors+position/8]>>(position%8))&(colors-1)];}
                if(value==255)throw IoError("Reserved palette tile");
            }
        }
        next.compress();
    }
    std::lock_guard guard(cs_write);std::swap(indicesAndData,next.indicesAndData);std::swap(allocatedSize,next.allocatedSize);
}
