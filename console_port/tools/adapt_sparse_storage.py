#!/usr/bin/env python3
"""Retain original plane packing loops, replacing packed pointers and platform I/O."""
from pathlib import Path
import re
r=Path(__file__).resolve().parents[1]
for module in ['SparseDataStorage','SparseLightStorage']:
    h=(r/f'original/Minecraft.World/{module}.h').read_text()
    h=h.replace('#include "xmcore.h"','#include "xmcore.h"\n#include "PlaneStorageState.h"')
    h=re.sub(r'__int64\s+dataAndCount;', 'PlaneStorageState dataAndCount;\n    static std::recursive_mutex storageMutex;',h)
    h=h.replace('void updateDataAndCount(__int64 newDataAndCount);','void updateDataAndCount(PlaneStorageState newDataAndCount);')
    h=h.replace('public:\n','public:\n    '+module+'(const '+module+'&)=delete;\n    '+module+'& operator=(const '+module+'&)=delete;\n',1)
    h=h.replace('\tvoid addNewPlane(int y);','private:\n\tvoid addNewPlane(int y);').replace('\tint  compress();','public:\n\tint  compress();')
    (r/f'ported/{module}.h').write_text('// Native locked CPU port; packed-pointer RCU comments below describe the archive.\n'+h)
    s=(r/f'original/Minecraft.World/{module}.cpp').read_text()
    s=s.replace('#include "'+module+'.h"','#include "'+module+'.h"\nstd::recursive_mutex '+module+'::storageMutex;')
    s=re.sub(r'^#pragma warning.*\n','',s,flags=re.M)
    # Decode pointer/count accesses before replacing the scalar state declarations.
    s=re.sub(r'\(unsigned char \*\)\((\w+) & 0x0000ffffffffffff\)',r'\1.pointer',s)
    s=re.sub(r'\(\s*(\w+) >> 48\s*\) & 0xffff',r'\1.count',s)
    s=re.sub(r'dataAndCount = 0x007F000000000000L .*?;', 'dataAndCount = {planeIndices,127};',s)
    s=re.sub(r'dataAndCount = 0x0000000000000000L .*?;', 'dataAndCount = {planeIndices,0};',s)
    s=re.sub(r'dataAndCount = \( sourceDataAndCount & 0xffff000000000000L \).*?;', 'dataAndCount = {destIndicesAndData,sourceDataAndCount.count};',s)
    s=re.sub(r'__int64 newDataAndCount = .*?\(\s*\(__int64\)\s*(\w+)\s*\) & 0x0000ffffffffffffL;',r'PlaneStorageState newDataAndCount{\1,0};',s)
    s=re.sub(r'newDataAndCount \|= \(\(__int64\)(\w+)\) << 48;',r'newDataAndCount.count=\1;',s)
    s=re.sub(r'__int64 (sourceDataAndCount|lastDataAndCount2?|newDataAndCount)',r'PlaneStorageState \1',s)
    s=s.replace('InterlockedCompareExchangeRelease64','exchangePlaneState')
    s=re.sub(r'\bmalloc\(', 'planeAlloc(',s)
    def span(signature):
        a=s.index('{',s.index(signature));b=a+1;depth=1
        while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
        return a,b
    def insert(signature,text):
        global s
        a,b=span(signature);s=s[:a+1]+'\n'+text+s[a+1:]
    # Lock all member operations except constructors, destructors and pointer views;
    # callers of the now-private view already hold the lock.
    methods=['setData','getData','get','set','setDataRegion','getDataRegion','addNewPlane','updateDataAndCount','compress','isCompressed','write','read','queueForDelete','tick']
    if module=='SparseLightStorage':methods.append('setAllBright')
    for method in methods:insert(module+'::'+method+'(', '    std::lock_guard guard(storageMutex);')
    insert(module+'::'+module+'('+module+' *copyFrom)', '    std::lock_guard guard(storageMutex);if(!copyFrom)throw IoError("Null sparse storage copy");')
    insert(module+'::setData(', '    validateByteRange(dataIn,inOffset,16384);')
    insert(module+'::getData(', '    validateByteRange(retArray,retOffset,16384);')
    insert(module+'::get(', '    planePosition(x,y,z);')
    insert(module+'::set(', '    planePosition(x,y,z);if(val<0 || val>15)throw IoError("Invalid sparse nibble value");')
    insert(module+'::addNewPlane(', '    if(y<0 || y>=128)throw IoError("Invalid sparse plane");')
    for method,array in [('setDataRegion','dataIn'),('getDataRegion','dataInOut')]:insert(module+'::'+method+'(', f'    if(!planeRegion({array},x0,y0,z0,x1,y1,z1,offset))return 0;')
    for method in ['write','read']:
        signature='void '+module+'::'+method+'(';start=s.index(signature);a,b=span(signature);s=s[:start]+s[b:]
    # Every live numeric packed-pointer operation should now be gone.
    for line in s.splitlines():
        if not line.lstrip().startswith('//') and ('0x0000ffffffffffff' in line or '__int64' in line or '>> 48' in line):raise RuntimeError(module+': unconverted line: '+line)
    (r/f'ported/{module}.cpp').write_text('// Original plane packing loops with full-width pointer state and native locking.\n'+s)
