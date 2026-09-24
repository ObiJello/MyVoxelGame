#pragma once
#include "IoCompat.h"
#include "DataInputStream.h"
#include "DataOutputStream.h"
#include "System.h"
#include <bit>
#include <mutex>
#include <cstdlib>
using __uint64=std::uint64_t;
using CRITICAL_SECTION=std::recursive_mutex;
inline void EnterCriticalSection(CRITICAL_SECTION* mutex){mutex->lock();}
inline void LeaveCriticalSection(CRITICAL_SECTION* mutex){mutex->unlock();}
inline void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION*,int){}
inline constexpr int MAXULONG_PTR=0,PAGE_READWRITE=0;
inline void* XPhysicalAlloc(std::size_t bytes,int,int,int){
    // CPU storage needs ordinary aligned heap memory. Zero padding so unused
    // alignment bytes cannot leak heap contents into serialized chunks.
    auto* data=std::calloc(1,bytes);if(!data)throw std::bad_alloc();return data;
}
inline void XPhysicalFree(void* data){std::free(data);}
inline void XMemSet(void* data,int value,std::size_t size){std::memset(data,value,size);}
inline constexpr int BIGENDIAN=1,LOCALSYTEM_ENDIAN=std::endian::native==std::endian::big?1:0;
#ifdef CONSOLE_CODEC_REFERENCE
// Unreachable original allocation diagnostic: the host allocator throws on failure.
using DWORD=unsigned long;
inline DWORD GetLastError(){return 0;}
struct MEMORYSTATUS{};
inline void GlobalMemoryStatus(MEMORYSTATUS*){}
inline void __debugbreak(){std::abort();}
#endif
#ifdef CONSOLE_SPARSE_REFERENCE
inline __int64 InterlockedCompareExchangeRelease64(__int64* target,__int64 value,__int64 expected){
    __atomic_compare_exchange_n(target,&expected,value,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);return expected;
}
#endif
