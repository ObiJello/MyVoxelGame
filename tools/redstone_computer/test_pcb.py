import itertools
from build import Builder, Y, dump
from redtick import TickWorld
from pcb import Pcb, UP

def build():
    b = Builder(TickWorld()); p = Pcb(b); w = b.w
    b.lever(0, Y, -1, 'south'); b.lever(3, Y, -1, 'south'); b.lever(6, Y, -1, 'south')
    p.lane(0, 0, 20); p.lane(3, 0, 20); p.lane(6, 0, 20)
    # row A: via on lane 0 at z=10, east over lanes 3 and 6 to the gate's west face
    p.via_up(0, 10); p.row(4, 11, 10)
    (tx, tz), faces = p.gate(12, 10, 'east')
    # row B: via on lane 3 at z=14, east, then north into the gate's south face
    p.via_up(3, 14); p.row(7, 12, 14); p.upper_wire(12, 13); p.upper_wire(12, 12); p.upper_wire(12, 11)
    # row C: via on lane 6 at z=6, east, then south into the gate's north face
    p.via_up(6, 6); p.row(10, 12, 6); p.upper_wire(12, 7); p.upper_wire(12, 8); p.upper_wire(12, 9)
    # output: torch at (13,10) -> upper (14,10) -> ramp down to lane cell (12,10) -> south to a lamp
    p.torch_out((tx, tz), 'east', 1)
    p.via_down(12, 10)
    p.lane(12, 10, 16); b.lamp(12, Y - 1, 16)
    # a fourth lane passing under all rows, untouched, with its own lamp
    b.lever(9, Y, -1, 'south'); p.lane(9, 0, 20); b.lamp(9, Y - 1, 20)
    return b, w

def test():
    b, w = build(); w.boot(); w.run_until_idle()
    levers = [(0, Y, -1), (3, Y, -1), (6, Y, -1), (9, Y, -1)]
    for bits in itertools.product([0, 1], repeat=4):
        for l, v in zip(levers, bits): w.toggle_lever(l, v)
        w.run_until_idle()
        gate = int(w.lamp((12, Y - 1, 16))); passthru = int(w.lamp((9, Y - 1, 20)))
        assert gate == int(not (bits[0] or bits[1] or bits[2])), (bits, gate)
        assert passthru == bits[3], (bits, passthru)
    print('ok pcb: NOR of three lanes via rows; crossing lane isolated')

if __name__ == "__main__":
    test()
