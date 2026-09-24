#pragma once
#include <atomic>
#include <vector>
#include <chrono>
#include <cstdint>
struct LARGE_INTEGER { std::int64_t value; };
inline void QueryPerformanceCounter(LARGE_INTEGER* value){value->value=std::chrono::steady_clock::now().time_since_epoch().count();}
using DWORD=unsigned long;
inline DWORD TlsAlloc(){static std::atomic<DWORD> next{0};return next.fetch_add(1);}
inline std::vector<void*>& referenceTls(){static thread_local std::vector<void*> slots;return slots;}
inline void* TlsGetValue(DWORD index){auto& slots=referenceTls();return index<slots.size()?slots[index]:nullptr;}
inline bool TlsSetValue(DWORD index,void* value){auto& slots=referenceTls();if(index>=slots.size())slots.resize(index+1);slots[index]=value;return true;}
