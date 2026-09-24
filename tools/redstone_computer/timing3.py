"""Per-cycle timing report from a play2 edge file.

    python3 timing3.py edges.txt

For every rising edge of the PA lane (phase A at the sequencer) it prints
the offsets, within that cycle, of the first edge of each signal of
interest: the other phases, the head step (STEPX_L / STEPY_L), the head
register bits, GROW, the tail step, the pulse lines at the board (row:*),
the column select (col:*) and the death path.
"""
import sys
from collections import defaultdict

WATCH = ['P1', 'P2', 'P3', 'P4', 'P5', 'PA_L', 'PB_L', 'PC_L', 'PD_L', 'PE_L', 'STEPX_L', 'STEPY_L', 'HXQ0', 'HXQ1', 'HXQ2', 'HYQ0',
         'EQX', 'EQY', 'GROW', 'GROW_L', 'GROWP', 'GROWPL', 'PERASE', 'STEPTX_L', 'STEPTY_L', 'TXQ0', 'TYQ0',
         'YPERASE', 'YPCHECK', 'YPSET', 'row:TYE3', 'row:HYC3', 'row:HYS3', 'col:HX3', 'col:HX4',
         'WFOOD', 'LCNT', 'WALL', 'NC', 'DEAD', 'DEADFB', 'G', 'TD0', 'TD1', 'TDD.E_L', 'DIR0', 'DIR1', 'HD.E_L']


def main(path):
    edges = defaultdict(list)
    with open(path) as fh:
        for line in fh:
            t, n, v = line.rstrip('\n').split('\t')
            edges[n].append((int(t), int(v)))
    a_edges = [t for t, v in edges.get('P1', []) if v == 1]
    if not a_edges:
        print('no PA edges'); return
    print('PA rising edges:', a_edges, 'period', [b - a for a, b in zip(a_edges, a_edges[1:])])
    for ci, t0 in enumerate(a_edges):
        t1 = a_edges[ci + 1] if ci + 1 < len(a_edges) else t0 + 10 ** 9
        print(f'--- cycle {ci + 1} (A at {t0})')
        rows = []
        for n in WATCH:
            es = [(t - t0, v) for t, v in edges.get(n, []) if t0 <= t < t1]
            if es:
                rows.append((es[0][0], n, ' '.join(f'{"+" if v else "-"}{t}' for t, v in es[:6])))
        for off, n, s in sorted(rows):
            print(f'   {n:10s} {s}')


if __name__ == '__main__':
    main(sys.argv[1])
