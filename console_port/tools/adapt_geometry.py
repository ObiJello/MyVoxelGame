#!/usr/bin/env python3
"""Port original vector/box arithmetic with a native thread-local pool boundary."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1];source=r/'original/Minecraft.World'
destination=Path(sys.argv[1]);destination.mkdir(parents=True,exist_ok=True)
reference='--reference' in sys.argv
for name in ['Vec3','AABB']:
    header=(source/(name+'.h')).read_text().replace('#include "Definitions.h"','')
    header=header.replace('#pragma once','#pragma once\n#include <memory>\n#include <string>')
    body=(source/(name+'.cpp')).read_text()
    if reference:
        body='#include "GeometryReference.h"\n'+body
    else:
        header=header.replace('static DWORD tlsIdx;\n\tstatic ThreadStorage *tlsDefault;',
                              'static thread_local std::unique_ptr<ThreadStorage> tls;\n\tstatic ThreadStorage& storage();')
        start=body.index('DWORD '+name+'::tlsIdx');end=body.index(name+' *'+name+'::newPermanent',start)
        boundary=f'''thread_local std::unique_ptr<{name}::ThreadStorage> {name}::tls;
{name}::ThreadStorage::ThreadStorage():pool(new {name}[POOL_SIZE]),poolPointer(0) {{}}
{name}::ThreadStorage::~ThreadStorage() {{ delete[] pool; }}
{name}::ThreadStorage& {name}::storage() {{
    if(!tls)tls=std::make_unique<ThreadStorage>();
    return *tls;
}}
void {name}::CreateNewThreadStorage() {{ (void)storage(); }}
void {name}::UseDefaultThreadStorage() {{ (void)storage(); }}
void {name}::ReleaseThreadStorage() {{ tls.reset(); }}

'''
        body=body[:start]+boundary+body[end:]
        body=body.replace('ThreadStorage *tls = (ThreadStorage *)TlsGetValue(tlsIdx);','ThreadStorage *tls = &storage();')
        body=body.replace('static wchar_t buf[128];','wchar_t buf[128];')
        body='#include "GeometryStrings.h"\n'+body
        old='// Each new thread that needs to use Vec3 pools will need to call one of the following 2 functions, to either create its own\n\t// local storage, or share the default storage already allocated by the main thread'
        header=header.replace(old,'// Native pools initialize lazily and belong to the calling thread.\n\t// Temp pointers are borrowed and reused after 1024 allocations, as in the source.\n\t// Use newPermanent for values that must survive pool reuse; the caller deletes them.')
    (destination/(name+'.h')).write_text(header)
    (destination/(name+'.cpp')).write_text(body)
header=(source/'HitResult.h').read_text().replace('#include "Vec3.h"','#include "Vec3.h"\nclass Entity;')
if not reference:
    header=header.replace('HitResult(shared_ptr<Entity> entity);','// Entity-dependent operations require the full Entity port.\n\tHitResult(shared_ptr<Entity> entity) = delete;')
    header=header.replace('double distanceTo(shared_ptr<Entity> e);','double distanceTo(shared_ptr<Entity> e) = delete;')
    header=header.replace('Vec3 *pos;','// Borrowed original vector-pool result; copy coordinates before pool reuse.\n\tVec3 *pos;')
s=(source/'HitResult.cpp').read_text();a=s.index('HitResult::HitResult(int ');b=s.index('\nHitResult::HitResult(shared_ptr',a)
(destination/'HitResult.h').write_text(header)
(destination/'HitResult.cpp').write_text('#include "stdafx.h"\n#include "HitResult.h"\n'+s[a:b])
