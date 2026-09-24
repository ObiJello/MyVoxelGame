from build import Builder, Y
from redtick import TickWorld
from seq import counter_up
from engine_check import check

b = Builder(TickWorld()); w = b.w
c = counter_up(b, 10, 10, 3)
ix, iz = c['IN']
btn = (ix - 4, Y, iz)
b.button(*btn, 'south'); b.line(ix - 3, iz, ix - 1, iz)
w.boot(); w.run_until_idle()
qs = [(q[0], Y, q[1]) for q in c['Q']]
start = sum((w.get(q).power > 0) << i for i, q in enumerate(qs))
print('start value', start)
script = [(i * 3.0, ('button', btn)) for i in range(5)]
mism, actual = check('RC Check', w, script)
val = sum((actual[(q[0], q[1] - 60, q[2])][1].get('lit') == 'true') << i for i, q in enumerate(qs))
print('engine count', val, 'sim count', sum((w.get(q).power > 0) << i for i, q in enumerate(qs)), 'expected', (start + 5) % 8)
