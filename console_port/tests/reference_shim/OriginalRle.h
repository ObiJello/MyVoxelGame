#pragma once
#include <cstring>
#include <cassert>
using HRESULT=int;
constexpr int S_OK=0;
inline void EnterCriticalSection(int*){}
inline void LeaveCriticalSection(int*){}
#define PIXBeginNamedEvent(...)
#define PIXEndNamedEvent(...)
class OriginalRle {
    int rleCompressLock=0,rleDecompressLock=0;
    unsigned char rleCompressBuf[1024*100]{};
public:
    HRESULT CompressRLE(void*,unsigned*,void*,unsigned);
    HRESULT DecompressRLE(void*,unsigned*,void*,unsigned);
};
