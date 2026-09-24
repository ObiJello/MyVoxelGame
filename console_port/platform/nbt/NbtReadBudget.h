#pragma once
#include "IoCompat.h"
// Host parser limits, shared by every nested container in one read operation.
// Prevent a corrupt count from allocating unbounded memory before reaching EOF.
class NbtReadScope {
    struct Budget {unsigned depth=0; std::size_t bytes=0, nodes=0;};
    static Budget& budget(){static thread_local Budget value; return value;}
public:
    static constexpr unsigned maxDepth=512;
    static constexpr std::size_t maxBytes=256u*1024u*1024u, maxNodes=1024u*1024u;
    NbtReadScope(){auto& b=budget(); if(!b.depth)b=Budget{}; if(b.depth>=maxDepth)throw IoError("NBT nesting limit exceeded"); ++b.depth;}
    ~NbtReadScope(){--budget().depth;}
    NbtReadScope(const NbtReadScope&)=delete;
    static void charge(std::size_t bytes, std::size_t nodes=0){
        auto& b=budget();
        if(bytes>maxBytes-b.bytes || nodes>maxNodes-b.nodes)throw IoError("NBT allocation limit exceeded");
        b.bytes+=bytes; b.nodes+=nodes;
    }
    static unsigned arrayLength(int length, std::size_t elementSize){
        if(length<0)throw IoError("Negative NBT collection length");
        if(static_cast<std::size_t>(length)>maxBytes/elementSize)throw IoError("NBT collection too large");
        charge(static_cast<std::size_t>(length)*elementSize);
        return static_cast<unsigned>(length);
    }
};
