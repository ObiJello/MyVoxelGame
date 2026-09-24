#pragma once
#include <mutex>
#include <vector>
#include <cstdlib>
// Native replacement for the original deferred-free stack; owns queued buffers.
// The outer storage lock serializes queue rotation in the port.
template<class T>class XLockFreeStack {
    std::mutex mutex_;
    std::vector<T*> entries_;
public:
    ~XLockFreeStack(){for(auto* entry:entries_)std::free(entry);}
    void Initialize(){}
    // On allocation failure, locked CPU readers permit immediate reclamation.
    void Push(T* entry){std::lock_guard guard(mutex_);try{entries_.push_back(entry);}catch(const std::bad_alloc&){std::free(entry);}}
    T* Pop(){std::lock_guard guard(mutex_);if(entries_.empty())return nullptr;auto* result=entries_.back();entries_.pop_back();return result;}
};
