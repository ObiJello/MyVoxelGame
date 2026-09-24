#pragma once
#include "stdafx.h"
#include "../worldgen/ArrayWithLength.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <climits>
#include <cfloat>
struct IoError:std::runtime_error{using std::runtime_error::runtime_error;};
struct EndOfStream:IoError{EndOfStream():IoError("Unexpected end of binary stream"){};};
struct InvalidUtf:IoError{using IoError::IoError;};
using std::min;
using std::max;
inline void XMemCpy(void* destination,const void* source,std::size_t size){std::memcpy(destination,source,size);}
inline void validateByteRange(byteArray bytes,unsigned offset,unsigned length){
    if(offset>bytes.length || length>bytes.length-offset || (length && !bytes.data))
        throw std::out_of_range("Byte stream buffer range is invalid");
}
