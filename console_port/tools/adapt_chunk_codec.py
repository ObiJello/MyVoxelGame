#!/usr/bin/env python3
"""Adapt archived CPU tile packing; platform I/O is in CompressedTileStorageIo.cpp."""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
s=(r/'original/Minecraft.World/CompressedTileStorage.cpp').read_text()
def body_span(signature):
    a=s.index('{',s.index(signature));b=a+1;d=1
    while d:d+=(s[b]=='{')-(s[b]=='}');b+=1
    return a,b
def replace_body(signature,body):
    global s
    a,b=body_span(signature);s=s[:a]+body+s[b:]
def insert(signature,text):
    global s
    a,b=body_span(signature);s=s[:a+1]+'\n'+text+s[a+1:]
replace_body('bool CompressedTileStorage::isSameAs(','''{
    std::lock_guard guard(cs_write);
    if(!other)throw IoError("Null compressed chunk comparison");
    return allocatedSize==other->allocatedSize && (!allocatedSize || std::memcmp(indicesAndData,other->indicesAndData,allocatedSize)==0);
}''')
s=s.replace('EnterCriticalSection(&cs_write);','std::lock_guard storageGuard(cs_write);').replace('LeaveCriticalSection(&cs_write);','')
s=s.replace('__int64 i64_1 = 1;','std::uint64_t i64_1 = 1;')
s=s.replace('__uint64 *comp = (__uint64 *)&blockIndices[block];','std::uint64_t comp;std::memcpy(&comp,&blockIndices[block],sizeof(comp));').replace('( *comp )','comp')
a=s.index('\n\t\tif( newIndicesAndData == NULL )');b=s.index('\n\t\tunsigned char *pucData',a);s=s[:a]+s[b:]
s=s.replace('#include "CompressedTileStorage.h"','#include "CompressedTileStorage.h"\n'+'''
namespace {
void codecPosition(int x,int y,int z){if(x<0 || x>=16 || y<0 || y>=128 || z<0 || z>=16)throw IoError("Compressed chunk position out of range");}
void codecTiles(byteArray bytes,unsigned offset,unsigned count){validateByteRange(bytes,offset,count);for(unsigned i=0;i<count;++i)if(bytes[offset+i]==255)throw IoError("Tile 255 is reserved by the console palette");}
unsigned codecRegion(byteArray bytes,int x0,int y0,int z0,int x1,int y1,int z1,int offset){
    if(x0<0 || y0<0 || z0<0 || x1<x0 || y1<y0 || z1<z0 || x1>16 || y1>128 || z1>16 || offset<0)throw IoError("Invalid compressed chunk region");
    unsigned count=(x1-x0)*(y1-y0)*(z1-z0);validateByteRange(bytes,static_cast<unsigned>(offset),count);return count;
}
}
''')
insert('CompressedTileStorage::CompressedTileStorage(CompressedTileStorage *copyFrom)','    if(!copyFrom)throw IoError("Null compressed chunk copy");')
insert('CompressedTileStorage::CompressedTileStorage(byteArray','    if(initFrom.data)codecTiles(initFrom,initOffset,32768);')
insert('void CompressedTileStorage::setData(', '    codecTiles(dataIn,inOffset,32768);')
s=s.replace('void CompressedTileStorage::getData(byteArray retArray, unsigned int retOffset)\n{','void CompressedTileStorage::getData(byteArray retArray, unsigned int retOffset)\n{\n    std::lock_guard guard(cs_write);validateByteRange(retArray,retOffset,32768);\n    if(!indicesAndData){std::fill_n(retArray.data+retOffset,32768,0);return;}')
insert('int  CompressedTileStorage::get(', '    codecPosition(x,y,z);std::lock_guard guard(cs_write);if(!indicesAndData)return 0;')
insert('void CompressedTileStorage::set(', '''    codecPosition(x,y,z);if(val<0 || val>=255)throw IoError("Invalid tile value");
    std::lock_guard outerGuard(cs_write);
    if(!indicesAndData){CompressedTileStorage empty(true);std::swap(indicesAndData,empty.indicesAndData);std::swap(allocatedSize,empty.allocatedSize);}''')
insert('bool CompressedTileStorage::isRenderChunkEmpty(', '    if(y<0 || y>=128 || y%16)throw IoError("Invalid render section");std::lock_guard guard(cs_write);if(!indicesAndData)return true;')
insert('bool CompressedTileStorage::isCompressed()', '    std::lock_guard guard(cs_write);')
insert('int CompressedTileStorage::getHighestNonEmptyY()', '    std::lock_guard guard(cs_write);if(!indicesAndData)return -1;')
insert('int CompressedTileStorage::getAllocatedSize(', '    std::lock_guard guard(cs_write);if(!count0 || !count1 || !count2 || !count4 || !count8)throw IoError("Null palette statistics");\n    if(!indicesAndData){*count0=512;*count1=*count2=*count4=*count8=0;return 0;}')
insert('void  CompressedTileStorage::compress(int', '    std::lock_guard outerGuard(cs_write);if(upgradeBlock< -1 || upgradeBlock>=512)throw IoError("Invalid palette upgrade block");if(!indicesAndData)return;')
insert('void CompressedTileStorage::tick()', '    std::lock_guard guard(cs_write);')
insert('void CompressedTileStorage::queueForDelete(', '    std::lock_guard guard(cs_write);')
for signature,arr,write in [('int  CompressedTileStorage::setDataRegion(', 'dataIn',True),('bool  CompressedTileStorage::testSetDataRegion(','dataIn',True),('int  CompressedTileStorage::getDataRegion(','dataInOut',False)]:
    text=f'    unsigned countToCopy=codecRegion({arr},x0,y0,z0,x1,y1,z1,offset);\n'
    if write:text+='    codecTiles(dataIn,static_cast<unsigned>(offset),countToCopy);\n'
    text+='    if(!countToCopy)return 0;'
    insert(signature,text)
for signature in ['void CompressedTileStorage::write(','void CompressedTileStorage::read(']:
    start=s.index(signature);a,b=body_span(signature);s=s[:start]+s[b:]
(r/'ported/CompressedTileStorage.cpp').write_text('// CPU packing adapted from the preserved console source.\n'+s)
