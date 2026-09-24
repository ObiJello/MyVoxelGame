#pragma once
#include "ArrayWithLength.h"
#include <algorithm>
#include <chrono>
class System {
public:
    static void ReverseUSHORT(unsigned short* value){*value=static_cast<unsigned short>((*value>>8)|(*value<<8));}
    static std::int64_t currentTimeMillis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    template<class T> static void arraycopy(arrayWithLength<T> source,int start,
        arrayWithLength<T>* destination,int offset,int count) {
        if(start<0 || offset<0 || count<0 || static_cast<unsigned>(start+count)>source.length ||
            static_cast<unsigned>(offset+count)>destination->length)
            throw std::out_of_range("Original layer arraycopy outside buffer");
        std::copy_n(source.data+start,count,destination->data+offset);
    }
};
