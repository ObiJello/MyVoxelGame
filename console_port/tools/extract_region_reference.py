#!/usr/bin/env python3
"""Keep original sector allocation/writes; substitute only the storage backend."""
from pathlib import Path
import re,sys
r=Path(__file__).resolve().parents[1];s=(r/'original/reference-only/RegionFile.cpp').read_text()
# Ignore braces in the source's commented-out try/catch blocks.
s=re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',lambda m:m[0] if m[0].startswith(chr(34)) else '\n'*m[0].count('\n'),s,flags=re.S)
out='#include "OriginalRegion.h"\nbyteArray OriginalRegion::emptySector(SECTOR_BYTES);\n'
methods=['int RegionFile::getSizeDelta()', 'void RegionFile::write(int x, int z, byte *data, int length)', 'void RegionFile::write(int sectorNumber, byte *data, int length, unsigned int compLength)', 'void RegionFile::zero(', 'bool RegionFile::outOfBounds(', 'int RegionFile::getOffset(', 'bool RegionFile::hasChunk(', 'void RegionFile::insertInitialSectors(', 'void RegionFile::setOffset(', 'void RegionFile::setTimestamp(']
for method in methods:
 a=s.index(method);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 body=s[a:b].replace('RegionFile::','OriginalRegion::')
 body=re.sub(r'\bbyte\b','::byte',body)
 body=body.replace('System::currentTimeMillis()', 'referenceTimeMillis')
 out+=body+'\n'
Path(sys.argv[1]).write_text(out)
