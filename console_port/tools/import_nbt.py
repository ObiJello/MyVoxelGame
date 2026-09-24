#!/usr/bin/env python3
"""Record the supplied console NBT source; never overwrite ported adaptations."""
from pathlib import Path
import hashlib, json, shutil
root = Path(__file__).resolve().parents[1]
manifest_path = root / 'docs/source_manifest.json'
manifest = json.loads(manifest_path.read_text())
source = Path(manifest['source_root'])
names = ['Tag.h', 'Tag.cpp', 'NbtIo.h', 'NbtIo.cpp'] + [n+'Tag.h' for n in ['End','Byte','Short','Int','Long','Float','Double','ByteArray','IntArray','String','List','Compound']]
for name in names:
    relative = 'Minecraft.World/'+name
    dest = 'original/'+relative
    payload = (source / relative).read_bytes()
    (root / dest).write_bytes(payload)
    if not (root/'ported'/name).exists(): shutil.copyfile(root/dest, root/'ported'/name)
    if not any(e['destination']==dest for e in manifest['files']):
        manifest['files'].append(dict(source=relative,destination=dest,sha256=hashlib.sha256(payload).hexdigest()))
manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
