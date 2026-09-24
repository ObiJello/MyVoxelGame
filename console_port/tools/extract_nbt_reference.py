#!/usr/bin/env python3
"""Build the archived NBT wire implementation with only C++20 host syntax fixes.
Debug printing uses wide stream references; no serialization body is substituted.
"""
from pathlib import Path
import re, sys
root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
for name in ['Tag.h','Tag.cpp','NbtIo.h','NbtIo.cpp']+[n+'Tag.h' for n in ['End','Byte','Short','Int','Long','Float','Double','ByteArray','IntArray','String','List','Compound']]:
    s=(root/'original/Minecraft.World'/name).read_text()
    s=re.sub(r'(?<![:\w])byte\b','::byte',s)
    s=s.replace('wchar_t *getTagName','const wchar_t *getTagName').replace('wchar_t *Tag::getTagName','const wchar_t *Tag::getTagName')
    s=re.sub(r'(?<!const )wchar_t \*\s*(name|string)',r'const wchar_t *\1',s)
    s=s.replace('ostream out','wostream& out').replace('wwostream& out','wostream& out')
    s=s.replace('char *prefix','const char *prefix')
    if name=='Tag.cpp':
        s=s.replace('app.DebugPrintf("readNamedTag read a type of 255\\n");','').replace('__debugbreak();','')
    (out/name).write_text(s)
