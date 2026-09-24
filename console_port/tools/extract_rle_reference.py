#!/usr/bin/env python3
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1];s=(r/'original/reference-only/compression.cpp').read_text();out='#include "OriginalRle.h"\n'
for method in ['CompressRLE','DecompressRLE']:
    a=s.index('HRESULT Compression::'+method+'(');b=s.index('{',a)+1;depth=1
    while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
    out+=s[a:b].replace('Compression::','OriginalRle::')+'\n'
Path(sys.argv[1]).write_text(out)
