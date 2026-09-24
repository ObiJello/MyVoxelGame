"""Placement helpers on top of redsim.World — a tiny DSL for laying out circuits."""
from redsim import World, Block, add, DIRS, OPP

Y = 0   # wire level; the floor is at Y-1 (the generator picks the absolute y later)

# redstone_plus (the engine rule, docs/redstone.md): dust carries its full
# signal any distance and torches never burn out. With it on, the wire
# runners lay no transport repeaters (a lane is one net, 0 ticks end to
# end) and the sim's wires do not decay. Set through set_plus() BEFORE
# building; a pickled machine remembers it on its World (no_decay).
PLUS = False
BLUE = False      # gates use the blue (zero-delay) torch; needs PLUS


def set_plus(on=True, blue=None):
    global PLUS, BLUE
    PLUS = bool(on)
    BLUE = bool(on if blue is None else blue) and PLUS


def red(fn):
    """Decorator form of red_torches for a whole macro builder."""
    def wrapped(*a, **kw):
        with red_torches():
            return fn(*a, **kw)
    wrapped.__name__ = fn.__name__; wrapped.__doc__ = fn.__doc__
    return wrapped


class red_torches:
    """`with build.red_torches():` — sequential macros (counters, latches,
    FIFOs, the clock) keep the delayed red torch: their flip-flops use the
    torch delay as master/slave separation and race with instant gates."""
    def __enter__(self):
        global BLUE
        self.saved = BLUE; BLUE = False
    def __exit__(self, *exc):
        global BLUE
        BLUE = self.saved


# Overwrites that are part of normal construction: a lane re-laid cell by
# cell, a repeater dropped onto a lane wire by build_lanes, a lamp replacing
# a floor block. Anything else is two pieces of circuit fighting for a cell
# and gets recorded in Builder.collisions.
# (glow -> wire/solid: a via built after the row it ends replaces the row's
# glowstone under its last two cells with the ramp's mid wire and support.)
_OVERWRITE_OK = {('wire', 'wire'), ('wire', 'repeater'), ('solid', 'solid'), ('solid', 'lamp'),
                 ('glow', 'glow'), ('lamp', 'lamp'), ('solid', 'glow'),
                 ('glow', 'wire'), ('glow', 'solid')}


class Builder:
    def __init__(self, world=None):
        self.w = world or World()
        self.floor = set()      # (x, z) columns that need a floor block at Y-1
        self.collisions = []    # (pos, old kind, new kind)

    def _set(self, pos, block):
        old = self.w.blocks.get(pos)
        if old is not None:
            pair = (old.kind, block.kind)
            same = (old.kind == block.kind and old.facing == block.facing
                    and old.delay == block.delay and old.mode == block.mode)
            if not same and pair not in _OVERWRITE_OK:
                self.collisions.append((pos, old.kind, block.kind))
        self.w.set(pos, block)

    # basic blocks --------------------------------------------------------
    def solid(self, x, y, z, name='minecraft:stone'):
        self._set((x, y, z), Block('solid', name=name))

    def glow(self, x, y, z):
        self._set((x, y, z), Block('glow'))

    def wire(self, x, y, z):
        self._set((x, y, z), Block('wire'))
        self._support(x, y, z)

    def wires(self, cells):
        for (x, y, z) in cells:
            self.wire(x, y, z)

    def repeater(self, x, y, z, facing, delay=1):
        """`facing` is the INPUT side (vanilla FACING = direction of the rear)."""
        self._set((x, y, z), Block('repeater', facing=facing, delay=delay))
        self._support(x, y, z)

    def comparator(self, x, y, z, facing, mode='compare'):
        self._set((x, y, z), Block('comparator', facing=facing, mode=mode))
        self._support(x, y, z)

    def torch(self, x, y, z):
        self._set((x, y, z), Block('torch', lit=True, instant=BLUE))
        self._support(x, y, z, hard=True)

    def wtorch(self, x, y, z, facing):
        """Wall torch; `facing` is the direction it points, away from its block."""
        self._set((x, y, z), Block('wtorch', facing=facing, lit=True, instant=BLUE))
        att = add((x, y, z), OPP[facing])
        if self.w.kind(att) not in ('solid', 'lamp'):
            self.solid(*att)

    def lever(self, x, y, z, facing='north'):
        self._set((x, y, z), Block('lever', face='floor', facing=facing))
        self._support(x, y, z, hard=True)

    def lamp(self, x, y, z):
        self._set((x, y, z), Block('lamp'))

    def display(self, x, y, z, op_nw='none', op_se='none'):
        """The redstone_plus display block (see redsim.World.display_eval)."""
        self._set((x, y, z), Block('display', op_nw=op_nw, op_se=op_se))

    def button(self, x, y, z, facing='north'):
        self._set((x, y, z), Block('button', face='floor', facing=facing))
        self._support(x, y, z, hard=True)

    def _support(self, x, y, z, hard=False):
        below = (x, y - 1, z)
        k = self.w.kind(below)
        if k == 'air' or (hard and k == 'glow'):
            self.solid(x, y - 1, z)

    # composite pieces ------------------------------------------------------
    def line(self, x0, z0, x1, z1, y=Y):
        """Straight wire run (inclusive) along x or z."""
        if x0 == x1:
            for z in range(min(z0, z1), max(z0, z1) + 1):
                self.wire(x0, y, z)
        elif z0 == z1:
            for x in range(min(x0, x1), max(x0, x1) + 1):
                self.wire(x, y, z0)
        else:
            raise ValueError('line must be axis-aligned')

    def path(self, pts, y=Y):
        """Wire along a polyline of (x, z) corners."""
        for (a, b) in zip(pts, pts[1:]):
            self.line(a[0], a[1], b[0], b[1], y)

    def bridge(self, x, z, axis, flow, y=Y):
        """A crossing that carries a signal OVER the wire at (x, y, z).

        Runs along `axis` ('x' or 'z') in the direction `flow` (a direction
        name) and occupies the 7 cells from -3 to +3 around the crossing:
        ramps at ±2 (y+1) and ±1 (y+2), the crossing cell itself at y+2 on
        glowstone directly above the crossed wire, and REPEATERS at ±3.
        The entry repeater drives the first ramp block (a repeater strongly
        powers the block in front of it, and the wire on top reads it); the
        exit repeater reads the last ramp block the same way, so the six
        cells of dust never cost the signal more than they must. Nothing in
        the crossed wire's row is touched.
        """
        assert axis in ('x', 'z')
        sign = 1 if flow in ('east', 'south') else -1
        assert (axis == 'x') == (flow in ('east', 'west'))
        def cell(o):
            o *= sign
            return (x + o, z) if axis == 'x' else (x, z + o)
        back = {'east': 'west', 'west': 'east', 'south': 'north', 'north': 'south'}[flow]
        cx, cz = cell(-3); self.repeater(cx, y, cz, back)   # entry: input from behind
        cx, cz = cell(3);  self.repeater(cx, y, cz, back)   # exit: input from the ramp block
        for o in (-2, 2):
            cx, cz = cell(o)
            self.solid(cx, y, cz)
            self.wire(cx, y + 1, cz)
        for o in (-1, 1):
            cx, cz = cell(o)
            self.solid(cx, y + 1, cz)
            self.wire(cx, y + 2, cz)
        self.glow(x, y + 1, z)
        self.wire(x, y + 2, z)

    # NOT: wire enters block at (x,z) from `frm` side; torch on the far side.
    def not_gate(self, x, z, out_dir, y=Y):
        """Solid block at (x, y, z); wall torch on its `out_dir` face."""
        self.solid(x, y, z)
        tx, ty, tz = add((x, y, z), out_dir)
        self.wtorch(tx, ty, tz, out_dir)
        return (tx, ty, tz)


def dump(b, y=Y, x0=None, x1=None, z0=None, z1=None):
    """ASCII map of one level. Wire=digit(power), R/C=diode(+ if on), T/t torch,
    L lever, # solid, g glow, o lamp(lit O), . air."""
    w = b.w
    keys = [p for p in w.blocks if p[1] == y]
    if not keys:
        print('(empty level)'); return
    xs = [p[0] for p in keys]; zs = [p[2] for p in keys]
    x0 = min(xs) if x0 is None else x0; x1 = max(xs) if x1 is None else x1
    z0 = min(zs) if z0 is None else z0; z1 = max(zs) if z1 is None else z1
    arrow = {'north': '^', 'south': 'v', 'east': '>', 'west': '<'}
    print('   x=' + ''.join(str(x % 10) for x in range(x0, x1 + 1)))
    for z in range(z0, z1 + 1):
        row = ''
        for x in range(x0, x1 + 1):
            bl = w.get((x, y, z))
            if bl is None: ch = '.'
            elif bl.kind == 'wire': ch = format(bl.power, 'x')
            elif bl.kind == 'repeater': ch = arrow[bl.facing] if bl.powered else arrow[bl.facing].lower() if False else ('R' if bl.powered else 'r')
            elif bl.kind == 'comparator': ch = 'C' if bl.output else 'c'
            elif bl.kind == 'torch': ch = 'T' if bl.lit else 't'
            elif bl.kind == 'wtorch': ch = 'W' if bl.lit else 'w'
            elif bl.kind == 'lever': ch = 'L' if bl.powered else 'l'
            elif bl.kind == 'lamp': ch = 'O' if bl.lit else 'o'
            elif bl.kind == 'glow': ch = 'g'
            else: ch = '#'
            row += ch
        print(f'z={z:3d} ' + row)
