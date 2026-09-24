"""ctypes driver for fastsim_c/fastsim.c — the tick simulator's hot loop in C.

    fastsim.attach(world)      # after settle()/boot(): every block goes to C
    world.tick(n)              # now runs in C; changed cells come back
    world.blocks[p].power      # ...so the Python objects stay readable

attach() replaces the world's tick / toggle_lever / press_button with C-backed
versions and adds pending_ticks() (what writer.py exports). The .so is rebuilt
from fastsim.c when it is older than the source.
"""
import ctypes, os, subprocess
import numpy as _np

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fastsim_c')
SRC = os.path.join(HERE, 'fastsim.c'); SO = os.path.join(HERE, 'fastsim.so')

KIND = {'wire': 1, 'solid': 2, 'glow': 3, 'repeater': 4, 'comparator': 5, 'torch': 6, 'wtorch': 7, 'lamp': 8, 'lever': 9, 'button': 10,
        'display': 11}
DIR = {'down': 0, 'up': 1, 'north': 2, 'south': 3, 'west': 4, 'east': 5}
F_LIT, F_POWERED, F_INSTANT, F_SUBTRACT, F_FACE_FLOOR, F_FACE_CEILING = 1, 2, 4, 8, 16, 32
_lib = None


def lib():
    global _lib
    if _lib is not None:
        return _lib
    if not os.path.exists(SO) or os.path.getmtime(SO) < os.path.getmtime(SRC):
        subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-o', SO, SRC])
    L = ctypes.CDLL(SO)
    P = ctypes.c_void_p
    L.rs_create.restype = P; L.rs_create.argtypes = [ctypes.c_int]
    L.rs_destroy.argtypes = [P]
    L.rs_add_cells.argtypes = [P, ctypes.c_int] + [ctypes.c_void_p] * 7
    L.rs_set_clock.argtypes = [P, ctypes.c_int64, ctypes.c_int64]
    L.rs_time.restype = ctypes.c_int64; L.rs_time.argtypes = [P]
    L.rs_schedule_at.argtypes = [P, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int64, ctypes.c_int, ctypes.c_int64]
    L.rs_tick.argtypes = [P, ctypes.c_int]
    L.rs_dirty_count.restype = ctypes.c_int; L.rs_dirty_count.argtypes = [P]
    L.rs_take_dirty.restype = ctypes.c_int; L.rs_take_dirty.argtypes = [P] + [ctypes.c_void_p] * 4
    L.rs_toggle_lever.argtypes = [P, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int]
    L.rs_press_button.argtypes = [P, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32]
    L.rs_pending_count.restype = ctypes.c_int; L.rs_pending_count.argtypes = [P]
    L.rs_pending.argtypes = [P] + [ctypes.c_void_p] * 4
    L.rs_peek.restype = ctypes.c_int; L.rs_peek.argtypes = [P, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32] + [ctypes.c_void_p] * 3
    L.rs_stats.argtypes = [P, ctypes.c_void_p, ctypes.c_void_p]
    _lib = L
    return L


class Fast:
    def __init__(self, world):
        L = lib()
        self.w = world
        self.h = L.rs_create(1 if getattr(world, 'no_decay', False) else 0)
        blocks = world.blocks
        n = len(blocks)
        xyz = _np.empty((n, 3), dtype=_np.int32)
        kind = _np.zeros(n, _np.uint8); facing = _np.zeros(n, _np.uint8); flags = _np.zeros(n, _np.uint8)
        power = _np.zeros(n, _np.uint8); output = _np.zeros(n, _np.uint8); delay = _np.ones(n, _np.uint8)
        for i, (p, b) in enumerate(blocks.items()):
            xyz[i] = p
            kind[i] = KIND[b.kind]
            facing[i] = DIR.get(b.facing, 0) if b.facing else 0
            f = 0
            if b.lit: f |= F_LIT
            if b.powered: f |= F_POWERED
            if getattr(b, 'instant', False): f |= F_INSTANT
            if b.kind == 'comparator' and b.mode == 'subtract': f |= F_SUBTRACT
            if b.kind in ('lever', 'button'):
                face = getattr(b, 'face', 'floor')
                f |= F_FACE_FLOOR if face == 'floor' else (F_FACE_CEILING if face == 'ceiling' else 0)
            flags[i] = f; power[i] = b.power; output[i] = b.output; delay[i] = b.delay
            if b.kind == 'display':             # facing = op_nw, delay = op_se, output = colour bits
                from redsim import DISPLAY_OPS
                facing[i] = DISPLAY_OPS.index(b.op_nw); delay[i] = DISPLAY_OPS.index(b.op_se); output[i] = b.rgb
        L.rs_add_cells(self.h, n, xyz.ctypes.data, kind.ctypes.data, facing.ctypes.data, flags.ctypes.data,
                       power.ctypes.data, output.ctypes.data, delay.ctypes.data)
        L.rs_set_clock(self.h, int(getattr(world, 'time', 0)), int(getattr(world, '_seq', 0)))
        for (t, prio, seq, pos, k) in sorted(getattr(world, '_heap', [])):
            L.rs_schedule_at(self.h, pos[0], pos[1], pos[2], int(t), int(prio), int(seq))
        world._heap = []; world._pending = set()

    def __del__(self):
        try:
            if self.h: lib().rs_destroy(self.h); self.h = None
        except Exception:
            pass

    def sync(self):
        L = lib(); n = L.rs_dirty_count(self.h)
        if n == 0:
            return 0
        xyz = _np.empty((n, 3), _np.int32); power = _np.empty(n, _np.uint8); flags = _np.empty(n, _np.uint8); output = _np.empty(n, _np.uint8)
        n = L.rs_take_dirty(self.h, xyz.ctypes.data, power.ctypes.data, flags.ctypes.data, output.ctypes.data)
        blocks = self.w.blocks
        xs = xyz.tolist(); ps = power.tolist(); fs = flags.tolist(); os_ = output.tolist()
        for i in range(n):
            b = blocks.get(tuple(xs[i]))
            if b is None: continue
            f = fs[i]
            if b.kind == 'wire': b.power = ps[i]
            elif b.kind in ('torch', 'wtorch', 'lamp'): b.lit = bool(f & F_LIT)
            elif b.kind == 'comparator': b.output = os_[i]; b.powered = bool(f & F_POWERED)
            elif b.kind == 'display': b.rgb = os_[i]; b.powered = bool(f & F_POWERED)
            else: b.powered = bool(f & F_POWERED)
        return n

    def tick(self, n=1):
        L = lib(); L.rs_tick(self.h, int(n)); self.w.time = L.rs_time(self.h); self.sync()

    def toggle_lever(self, pos, on=None):
        b = self.w.blocks[pos]
        v = (not b.powered) if on is None else bool(on)
        lib().rs_toggle_lever(self.h, pos[0], pos[1], pos[2], 1 if v else 0); self.sync()

    def press_button(self, pos):
        lib().rs_press_button(self.h, pos[0], pos[1], pos[2]); self.sync()

    def pending_ticks(self):
        """[(time, prio, seq, pos, kind)] like TickWorld._heap."""
        L = lib(); n = L.rs_pending_count(self.h)
        if n == 0: return []
        t = _np.empty(n, _np.int64); p = _np.empty(n, _np.int8); s = _np.empty(n, _np.int64); xyz = _np.empty((n, 3), _np.int32)
        L.rs_pending(self.h, t.ctypes.data, p.ctypes.data, s.ctypes.data, xyz.ctypes.data)
        out = []
        for i in range(n):
            pos = (int(xyz[i, 0]), int(xyz[i, 1]), int(xyz[i, 2])); b = self.w.blocks.get(pos)
            out.append((int(t[i]), int(p[i]), int(s[i]), pos, b.kind if b else '?'))
        return out

    def stats(self):
        a = _np.zeros(1, _np.int64); b = _np.zeros(1, _np.int64); lib().rs_stats(self.h, a.ctypes.data, b.ctypes.data)
        return {'settles': int(a[0]), 'notifies': int(b[0])}


def attach(world):
    """Move the world's ticking to C. Call after settle()/boot()."""
    if getattr(world, '_fast', None) is not None:
        return world._fast
    f = Fast(world)
    world._fast = f
    world.tick = f.tick
    world.toggle_lever = f.toggle_lever
    world.press_button = f.press_button
    world.pending_ticks = f.pending_ticks
    return f


def detach(world):
    f = getattr(world, '_fast', None)
    if f is None: return
    import heapq
    world._heap = list(f.pending_ticks()); heapq.heapify(world._heap)
    world._pending = {(pos, k) for _, _, _, pos, k in world._heap}
    for name in ('tick', 'toggle_lever', 'press_button', 'pending_ticks'):
        world.__dict__.pop(name, None)
    world._fast = None
