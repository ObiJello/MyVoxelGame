#!/usr/bin/env python3
"""Resolve original item constructor stack limits and registration overrides."""
from pathlib import Path
import re,json,hashlib,sys
r=Path(__file__).resolve().parents[1];manifest=r/'docs/source_manifest.json';m=json.loads(manifest.read_text());src=Path(m['source_root'])/'Minecraft.World'
def read(name):
 p=src/name
 if not p.exists():return ''
 data=p.read_bytes();dest='original/reference-only/'+name;(r/dest).write_bytes(data)
 if not any(f['destination']==dest for f in m['files']):m['files'].append(dict(source='Minecraft.World/'+name,destination=dest,sha256=hashlib.sha256(data).hexdigest()))
 return data.decode('utf-8-sig',errors='replace')
def clean(s):return re.sub(r'//[^\n]*|/\*.*?\*/','',s,flags=re.S)
cache={'Item':64}
def limit(cls):
 if cls in cache:return cache[cls]
 h=clean(read(cls+'.h'));body=clean(read(cls+'.cpp'))
 values=re.findall(r'(?:maxStackSize\s*=\s*|setMaxStackSize\(\s*)(\d+)',body)
 if values:value=int(values[0]);assert all(int(v)==value for v in values),cls
 else:
  base=re.search(r'class\s+'+cls+r'\s*:\s*public\s+(\w+)',h)
  if not base:raise ValueError('Unresolved item base '+cls)
  value=limit(base[1])
 cache[cls]=value;return value
rows={}
for statement in clean(read('Item.cpp')).split(';'):
 ctor=re.search(r'new\s+(\w+)\(\s*(\d+)',statement)
 if not ctor:continue
 cls,index=ctor[1],int(ctor[2])
 if not cls.endswith('Item'):continue
 value=limit(cls);override=re.findall(r'setMaxStackSize\(\s*(\d+)',statement)
 if override:value=int(override[-1])
 rows[index+256]=value
out='// Generated from original Item registrations and constructor inheritance.\n#include "ContainerItems.h"\nnamespace console {\nint consoleItemStackLimit(int id){\n if(id>0 && id<256)return 64;\n switch(id){\n'
for id,value in sorted(rows.items()):out+=f' case {id}:return {value};\n'
out+=' default:return 1; // Unknown registry entries are kept separate.\n }\n}\n}\n'
Path(sys.argv[1]).write_text(out);manifest.write_text(json.dumps(m,indent=2)+'\n')
