#pragma once
#include "ChunkCodecCompat.h"
// Full-width native pointer and count replace the console's packed 48-bit pointer.
// Storage methods hold their class's recursive mutex during access and exchange.
struct PlaneStorageState {
    unsigned char* pointer=nullptr;
    unsigned count=0;
    bool operator==(const PlaneStorageState&)const=default;
};
inline PlaneStorageState exchangePlaneState(PlaneStorageState* target,PlaneStorageState value,PlaneStorageState expected){auto prior=*target;if(prior==expected)*target=value;return prior;}
inline void* planeAlloc(std::size_t bytes){return XPhysicalAlloc(bytes,0,0,0);}
inline void planePosition(int x,int y,int z){if(x<0 || x>=16 || y<0 || y>=128 || z<0 || z>=16)throw IoError("Sparse plane position out of range");}
inline unsigned planeRegion(byteArray bytes,int x0,int y0,int z0,int x1,int y1,int z1,int offset){
    if(x0<0 || y0<0 || z0<0 || x1<x0 || y1<y0 || z1<z0 || x1>16 || y1>128 || z1>16 || offset<0)throw IoError("Invalid sparse plane region");
    // Preserve the original paired-Y transfer convention, including odd bounds.
    unsigned count=(x1-x0)*(z1-z0)*((y1-y0)/2);validateByteRange(bytes,offset,count);return count;
}
