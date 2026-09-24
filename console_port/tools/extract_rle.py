#!/usr/bin/env python3
"""Retain the original byte-run encoder; provide dynamic storage for the host port."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1];s=(r/'original/reference-only/compression.cpp').read_text();a=s.index('HRESULT Compression::CompressRLE(');a=s.index('\tdo\n',a);b=s.index('unsigned int rleSize',a)
loop=s[a:b]
out='''#include "ConsoleCompression.h"
namespace console::compression {
std::vector<unsigned char> encodeRle(std::span<const unsigned char> source){
    if(source.size()>PS3_MAX_SAVE_BYTES)throw IoError("Compression input exceeds save capacity");
    if(source.empty())return {};
    std::vector<unsigned char> buffer(source.size()*2);
    const unsigned char* pucIn=source.data();
    const unsigned char* pucEnd=pucIn+source.size();
    unsigned char* pucOut=buffer.data();
'''+loop+'''
    buffer.resize(static_cast<std::size_t>(pucOut-buffer.data()));return buffer;
}
}
'''
Path(sys.argv[1]).write_text('// Original CompressRLE run-selection loop, with an empty-input guard and dynamic buffer.\n'+out)
