#!/usr/bin/env python3
"""Score a visible-section dump against the world's heightmaps.

    tools/underground_share.py "<saves>/OG with Structs" [logs/visible-dump.txt]

The game writes the dump with OBEY_DUMP_VISIBLE=1 (ChunkRenderer::
DumpVisibleSections): once a second, the real view's camera position and
every section key it drew. For each sample this script reads the
OCEAN_FLOOR heightmap of every drawn section's chunk straight from the
Anvil region files and reports how many drawn sections sit ENTIRELY below
the surface — every one of their 256 columns has terrain above the
section's top. Those are the sections a section-level occlusion pass could
remove at most (an upper bound: a cave section visible through its mouth
is underground too, and would stay). It also reports the camera's height
above the surface, so above-ground and in-cave samples read separately.

Only the overworld's region/ folder is read; a section whose chunk has no
heightmap on disk (unsaved, another dimension) counts as unknown.

Anvil/NBT reading is self-contained (stdlib only): region header ->
chunk offset -> zlib -> NBT compound -> Heightmaps.MOTION_BLOCKING,
9-bit entries packed 7 per long without straddling (1.16+ layout).
"""
import math
import os
import re
import struct
import sys
import zlib
import statistics as st

MIN_Y = -64          # world floor: section index -4 is y -64..-49


class NBT:
    """Minimal NBT reader: walks a named-tag stream, returns nested dicts;
    long arrays come back as lists of unsigned 64-bit ints."""

    def __init__(self, data):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def i16(self):
        v = struct.unpack_from('>h', self.d, self.p)[0]; self.p += 2; return v

    def i32(self):
        v = struct.unpack_from('>i', self.d, self.p)[0]; self.p += 4; return v

    def i64(self):
        v = struct.unpack_from('>q', self.d, self.p)[0]; self.p += 8; return v

    def name(self):
        n = struct.unpack_from('>H', self.d, self.p)[0]; self.p += 2
        s = self.d[self.p:self.p + n].decode('utf-8', 'replace'); self.p += n
        return s

    def payload(self, t):
        if t == 1: return self.u8()
        if t == 2: return self.i16()
        if t == 3: return self.i32()
        if t == 4: return self.i64()
        if t == 5: v = struct.unpack_from('>f', self.d, self.p)[0]; self.p += 4; return v
        if t == 6: v = struct.unpack_from('>d', self.d, self.p)[0]; self.p += 8; return v
        if t == 7:
            n = self.i32(); v = self.d[self.p:self.p + n]; self.p += n; return v
        if t == 8: return self.name()
        if t == 9:
            et = self.u8(); n = self.i32()
            return [self.payload(et) for _ in range(n)]
        if t == 10:
            out = {}
            while True:
                et = self.u8()
                if et == 0: return out
                nm = self.name()
                out[nm] = self.payload(et)
        if t == 11:
            n = self.i32(); v = list(struct.unpack_from('>%di' % n, self.d, self.p)); self.p += 4 * n; return v
        if t == 12:
            n = self.i32(); v = list(struct.unpack_from('>%dQ' % n, self.d, self.p)); self.p += 8 * n; return v
        raise ValueError('bad tag %d' % t)

    def root(self):
        t = self.u8(); self.name()
        return self.payload(t)


def unpack_heightmap(longs):
    """1.16+ packing: 9 bits per entry, 7 entries per long, no straddling."""
    out = []
    for q in longs:
        for k in range(7):
            out.append((q >> (9 * k)) & 0x1FF)
            if len(out) == 256: return out
    return out


class Regions:
    def __init__(self, region_dir):
        self.dir = region_dir
        self.files = {}
        self.cache = {}      # (cx, cz) -> list of 256 ints (first free y) or None

    def _file(self, rx, rz):
        key = (rx, rz)
        if key not in self.files:
            path = os.path.join(self.dir, 'r.%d.%d.mca' % (rx, rz))
            self.files[key] = open(path, 'rb').read() if os.path.exists(path) else None
        return self.files[key]

    def heightmap(self, cx, cz):
        key = (cx, cz)
        if key in self.cache: return self.cache[key]
        data = self._file(cx >> 5, cz >> 5)
        hm = None
        if data:
            idx = 4 * ((cx & 31) + 32 * (cz & 31))
            off = struct.unpack_from('>I', data, idx)[0]
            sectors, count = off >> 8, off & 0xFF
            if sectors and count:
                base = sectors * 4096
                length = struct.unpack_from('>I', data, base)[0]
                comp = data[base + 4]
                raw = data[base + 5: base + 4 + length]
                try:
                    if comp == 2: raw = zlib.decompress(raw)
                    elif comp == 1:
                        import gzip; raw = gzip.decompress(raw)
                    tag = NBT(raw).root()
                    hms = tag.get('Heightmaps') or tag.get('Level', {}).get('Heightmaps') or {}
                    # OCEAN_FLOOR = highest motion-blocking NON-water block:
                    # water must not count as an occluder (you see the sea
                    # floor from a boat). Leaves do count here, a small
                    # overestimate in forests.
                    longs = hms.get('OCEAN_FLOOR') or hms.get('MOTION_BLOCKING_NO_LEAVES') or hms.get('MOTION_BLOCKING')
                    if longs:
                        hm = [MIN_Y + v for v in unpack_heightmap(longs)]   # first air y per column
                except Exception as e:      # noqa: BLE001 — a bad chunk is just unknown
                    hm = None
        self.cache[key] = hm
        return hm


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    world = sys.argv[1]
    dump = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(
        '~/Library/Application Support/obeycraft/logs/visible-dump.txt')
    regions = Regions(os.path.join(world, 'region'))

    samples = []
    for line in open(dump):
        head, _, keys = line.partition('|')
        m = re.match(r'\s*t=(\S+)\s+cam=(\S+)\s+(\S+)\s+(\S+)\s+n=(\d+)', head)
        if not m: continue
        cam = [float(m.group(2)), float(m.group(3)), float(m.group(4))]
        secs = [tuple(int(v) for v in k.split(',')) for k in keys.split()]
        if not secs: continue
        samples.append((float(m.group(1)), cam, secs))
    if not samples: sys.exit('empty dump')

    print('%6s %8s %8s %6s %6s %6s %6s   %s' % ('t', 'camY', 'surfY', 'drawn', 'under', 'deep', 'unk', 'under% (deep% = 16+ blocks below)'))
    tot_drawn = tot_under = tot_deep = tot_unknown = 0
    above = []   # (under share) for samples with camera above ground
    for t, cam, secs in samples:
        bx, bz = math.floor(cam[0]), math.floor(cam[2])
        camhm = regions.heightmap(bx >> 4, bz >> 4)
        surf = camhm[(bx & 15) + 16 * (bz & 15)] if camhm else float('nan')
        under = deep = unknown = 0
        for cx, cz, sy in secs:
            hm = regions.heightmap(cx, cz)
            if hm is None:
                unknown += 1; continue
            # The game's section index counts from the world floor
            # (Config::MinY + sectionY * 16), not MC's signed index.
            top = MIN_Y + sy * 16 + 16  # exclusive top of the section
            lowest_surface = min(hm)    # first air y of the lowest column
            if top <= lowest_surface:
                under += 1
                if top <= lowest_surface - 16: deep += 1
        n = len(secs)
        tot_drawn += n; tot_under += under; tot_deep += deep; tot_unknown += unknown
        share = 100.0 * under / max(1, n)
        if surf == surf and cam[1] > surf: above.append(share)
        print('%6.1f %8.1f %8.1f %6d %6d %6d %6d   %5.1f%% (%.1f%%)' %
              (t, cam[1], surf, n, under, deep, unknown, share, 100.0 * deep / max(1, n)))
    print('-' * 90)
    print('all samples: %d sections drawn, %.1f%% entirely below the surface (%.1f%% by 16+ blocks), %.1f%% unknown' %
          (tot_drawn, 100.0 * tot_under / max(1, tot_drawn), 100.0 * tot_deep / max(1, tot_drawn),
           100.0 * tot_unknown / max(1, tot_drawn)))
    if above:
        print('camera above ground (%d samples): underground share mean %.1f%%, median %.1f%%, max %.1f%%' %
              (len(above), st.mean(above), st.median(above), max(above)))


if __name__ == '__main__':
    main()
