#pragma once
#include <algorithm>
#include <memory>
#include <stdexcept>
// The layer graph and original IntCache explicitly manage raw array ownership.
// Keep this calling convention separate from the owning noise doubleArray adapter.
template<class T> struct arrayWithLength {
    T* data=nullptr;
    unsigned int length=0;
    arrayWithLength()=default;
    explicit arrayWithLength(unsigned int n,bool clear=true):data(new T[n]),length(n) {
        if(clear)std::fill_n(data,n,T{});
    }
    arrayWithLength(T* pointer,unsigned int n):data(pointer),length(n){}
    // Original growth/ownership convention, with typed initialization instead
    // of memset so nontrivial element types remain valid C++ objects.
    void resize(unsigned int n){
        if(n<=length)throw std::invalid_argument("Array resize must grow the buffer");
        std::unique_ptr<T[]> next(new T[n]());
        if(data)std::copy_n(data,length,next.get());
        delete[] data;data=next.release();length=n;
    }
    T& operator[](unsigned int i){return data[i];}
    const T& operator[](unsigned int i)const{return data[i];}
};
class Biome;
class Layer;
using intArray=arrayWithLength<int>;
using byteArray=arrayWithLength<unsigned char>;
using charArray=arrayWithLength<char>;
using floatArray=arrayWithLength<float>;
using BiomeArray=arrayWithLength<Biome*>;
using LayerArray=arrayWithLength<std::shared_ptr<Layer>>;
