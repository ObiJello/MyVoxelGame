#!/usr/bin/env python3
"""Original native-endian footer writer with the original 16-bit filename ABI."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1];source=(r/'original/reference-only/FileHeader.cpp').read_text();out='#include "FileHeaderWire.h"\n'
for method in ['WriteHeader','GetStartOfNextData','GetFileSize']:
    a=source.index('FileHeader::'+method+'(');a=source.rfind('\n',0,a)+1;b=source.index('{',a)+1;depth=1
    while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
    out+=source[a:b].replace('FileHeader::','FileHeaderWire::')+'\n'
Path(sys.argv[1]).write_text(out)
