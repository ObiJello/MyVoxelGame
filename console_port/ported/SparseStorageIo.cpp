#include "SparseDataStorage.h"
#include "SparseLightStorage.h"
namespace {
PlaneStorageState readPlanes(DataInputStream* dis,unsigned lastSentinel){
    if(!dis)throw IoError("Null sparse storage input");
    int count=dis->readInt();if(count<0 || count>128)throw IoError("Invalid sparse plane count");
    unsigned bytes=128+128*count;
    std::unique_ptr<unsigned char,decltype(&std::free)> memory(static_cast<unsigned char*>(planeAlloc(bytes)),&std::free);
    if(!dis->readFully(byteArray(memory.get(),bytes)))throw EndOfStream();
    std::array<bool,128> occupied{};
    for(unsigned y=0;y<128;++y){
        unsigned index=memory.get()[y];
        if(index>=128){if(index>lastSentinel)throw IoError("Invalid uniform plane index");}
        else {if(index>=static_cast<unsigned>(count) || occupied[index])throw IoError("Invalid or aliased sparse plane index");occupied[index]=true;}
    }
    return {memory.release(),static_cast<unsigned>(count)};
}
void writePlanes(DataOutputStream* dos,PlaneStorageState state){
    if(!dos)throw IoError("Null sparse storage output");
    dos->writeInt(state.count);dos->write(byteArray(state.pointer,128+128*state.count));
}
}
void SparseDataStorage::write(DataOutputStream* dos){std::lock_guard guard(storageMutex);writePlanes(dos,dataAndCount);}
void SparseLightStorage::write(DataOutputStream* dos){std::lock_guard guard(storageMutex);writePlanes(dos,dataAndCount);}
void SparseDataStorage::read(DataInputStream* dis){auto state=readPlanes(dis,128);std::lock_guard guard(storageMutex);updateDataAndCount(state);}
void SparseLightStorage::read(DataInputStream* dis){auto state=readPlanes(dis,129);std::lock_guard guard(storageMutex);updateDataAndCount(state);}
