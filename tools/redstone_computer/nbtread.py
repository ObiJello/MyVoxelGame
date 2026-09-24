import gzip, zlib, struct
def rd(b, o, fmt):
    v = struct.unpack_from('>'+fmt, b, o); return v[0], o+struct.calcsize(fmt)
def rname(b,o):
    n,o=rd(b,o,'H'); return b[o:o+n].decode('utf8','replace'), o+n
class Tag:
    def __init__(s,t,v): s.t=t; s.v=v
    def __repr__(s): return 'Tag(%d,%r)'%(s.t,s.v)
def payload(b,o,t):
    if t==1: return rd(b,o,'b')
    if t==2: return rd(b,o,'h')
    if t==3: return rd(b,o,'i')
    if t==4: return rd(b,o,'q')
    if t==5: return rd(b,o,'f')
    if t==6: return rd(b,o,'d')
    if t==7:
        n,o=rd(b,o,'i'); return list(struct.unpack_from('>%db'%n,b,o)), o+n
    if t==8: return rname(b,o)
    if t==9:
        et,o=rd(b,o,'b'); n,o=rd(b,o,'i'); out=[]
        for _ in range(n):
            v,o=payload(b,o,et); out.append(v)
        return (et,out),o
    if t==10:
        d={}
        while True:
            tt,o=rd(b,o,'b')
            if tt==0: break
            nm,o=rname(b,o); v,o=payload(b,o,tt); d[nm]=Tag(tt,v)
        return d,o
    if t==11:
        n,o=rd(b,o,'i'); return list(struct.unpack_from('>%di'%n,b,o)), o+4*n
    if t==12:
        n,o=rd(b,o,'i'); return list(struct.unpack_from('>%dq'%n,b,o)), o+8*n
    raise Exception('tag %d'%t)
def parse(b):
    t,o=rd(b,0,'b'); nm,o=rname(b,o); v,o=payload(b,o,t); return nm,Tag(t,v)
# writer
def wname(s): e=s.encode('utf8'); return struct.pack('>H',len(e))+e
def wpayload(tag):
    t,v=tag.t,tag.v
    if t==1: return struct.pack('>b',v)
    if t==2: return struct.pack('>h',v)
    if t==3: return struct.pack('>i',v)
    if t==4: return struct.pack('>q',v)
    if t==5: return struct.pack('>f',v)
    if t==6: return struct.pack('>d',v)
    if t==7: return struct.pack('>i',len(v))+struct.pack('>%db'%len(v),*v)
    if t==8: return wname(v)
    if t==9:
        et,items=v; return struct.pack('>bi',et,len(items))+b''.join(wpayload(Tag(et,i)) for i in items)
    if t==10:
        out=b''
        for k,tg in v.items(): out+=struct.pack('>b',tg.t)+wname(k)+wpayload(tg)
        return out+b'\x00'
    if t==11: return struct.pack('>i',len(v))+struct.pack('>%di'%len(v),*v)
    if t==12: return struct.pack('>i',len(v))+struct.pack('>%dq'%len(v),*v)
    raise Exception(t)
def serialize(name, tag): return struct.pack('>b',tag.t)+wname(name)+wpayload(tag)
def read_region_chunk(path, cx, cz):
    b=open(path,'rb').read()
    i=4*((cx&31)+(cz&31)*32)
    off=int.from_bytes(b[i:i+3],'big')*4096; cnt=b[i+3]
    if off==0: return None
    ln=int.from_bytes(b[off:off+4],'big'); comp=b[off+4]
    data=b[off+5:off+4+ln]
    raw = zlib.decompress(data) if comp==2 else gzip.decompress(data)
    return parse(raw)
def show(tag, depth=0, maxlist=4):
    pad='  '*depth
    if tag.t==10:
        for k,v in tag.v.items():
            if v.t in (10,9): print(pad+k+':'); show(v,depth+1,maxlist)
            else:
                s=repr(v.v); print(pad+k+' = '+(s[:100]+('...' if len(s)>100 else '')))
    elif tag.t==9:
        et,items=tag.v; print(pad+'[list of %d, type %d]'%(len(items),et))
        for it in items[:maxlist]:
            if et in (10,9): show(Tag(et,it),depth+1,maxlist); print(pad+'  --')
            else: print(pad+'  '+repr(it)[:100])
