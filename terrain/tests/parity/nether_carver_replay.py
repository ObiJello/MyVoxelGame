#!/usr/bin/env python3
"""Replay Java's expected nether-carver RNG draw stream (LegacyRandomSource).

Models NoiseBasedChunkGenerator.applyCarvers -> NetherWorldCarver(CaveWorldCarver.carve)
for target chunk (0,0), seed 12345, carverIndex 0, nether config.
Compare against C++ CARVER_DEBUG=1 [cd] stderr lines.
"""
MASK = (1 << 48) - 1
MULT = 0x5DEECE66D
ADD = 0xB


def i32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= (1 << 31) else v


def i64(v):
    v &= (1 << 64) - 1
    return v - (1 << 64) if v >= (1 << 63) else v


class Legacy:
    def __init__(self, seed):
        self.set_seed(seed)

    def set_seed(self, seed):
        self.seed = (seed ^ MULT) & MASK

    def next(self, bits):
        self.seed = (self.seed * MULT + ADD) & MASK
        return i32(self.seed >> (48 - bits))

    def next_int(self, bound):
        if (bound & -bound) == bound:
            return i64(bound * self.next(31)) >> 31
        while True:
            bits = self.next(31)
            val = bits % bound
            if bits - val + (bound - 1) >= 0:
                return val

    def next_long(self):
        return i64((self.next(32) << 32) + self.next(32))

    def next_float(self):
        return self.next(24) / float(1 << 24)

    def set_large_feature_seed(self, base, cx, cz):
        self.set_seed(base)
        l = self.next_long()
        m = self.next_long()
        self.set_seed(i64(cx * l) ^ i64(cz * m) ^ base)


SEED = 12345
MAX_DIST = ((4 * 2 - 1) << 4)  # sectionToBlockCoord(getRange()*2-1) = 112
CAVE_BOUND = 10  # nether


def replay_source(sx, sz):
    r = Legacy(0)
    r.set_large_feature_seed(SEED + 0, sx, sz)
    if r.next_float() > 0.2:  # isStartChunk
        return None
    lines = []
    cave_count = r.next_int(r.next_int(r.next_int(CAVE_BOUND) + 1) + 1)
    lines.append(f"[cd] src={sx},{sz} caveCount={cave_count}")
    for cave in range(cave_count):
        x = sx * 16 + r.next_int(16)
        # UniformHeight absolute(0)..belowTop(1). CRITICAL: the carving context
        # genDepth is min(dimension height 256, noise settings height 128) = 128,
        # so belowTop(1) resolves to 126 -> nextInt(127). (Was the C5 bug.)
        y = r.next_int(127)
        z = sz * 16 + r.next_int(16)
        # h/v multiplier + floorLevel: ConstantFloat, no draws
        room = r.next_int(4) == 0
        tunnels = 1
        if room:
            # yScale ConstantFloat(0.5): no draw
            _room_thickness = 1.0 + r.next_float() * 6.0
            tunnels += r.next_int(4)
        lines.append(f"[cd]  cave={cave} pos={x:.1f},{y:.1f},{z:.1f} room={int(room)} tunnels={tunnels}")
        for t in range(tunnels):
            h_rot = r.next_float() * (3.1415927410125732 * 2.0)  # (float)Math.PI * 2F
            v_rot = (r.next_float() - 0.5) / 4.0
            r1 = r.next_float()
            r2 = r.next_float()
            thick = (r1 * 2.0 + r2) * 2.0  # NetherWorldCarver.getThickness
            dist = MAX_DIST - r.next_int(MAX_DIST // 4)
            seed = r.next_long()
            lines.append(f"[cd]   tun={t} hRot={h_rot:.6f} vRot={v_rot:.6f} thick={thick:.6f} dist={dist} seed={seed}")
    return lines


if __name__ == "__main__":
    for sz in range(-8, 9):
        for sx in range(-8, 9):
            out = replay_source(sx, sz)
            if out:
                for ln in out:
                    print(ln)
