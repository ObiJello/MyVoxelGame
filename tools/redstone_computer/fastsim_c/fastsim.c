/* fastsim.c — the tick simulator's hot loop in C.
 *
 * A rule-for-rule port of redsim.World's signal rules and redtick.TickWorld's
 * scheduling (see those files for the vanilla references). The Python side
 * builds the world, settles and boots it, then hands every block to this
 * engine and drives ticks here; changed cells are read back after each
 * tick (rs_take_dirty), so the Python Block objects the harness reads stay
 * current. Nothing here places or removes blocks.
 *
 * Deterministic where Python used sets: neighbour notifications after a
 * wire settle go out in first-seen order.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { AIR = 0, WIRE, SOLID, GLOW, REPEATER, COMPARATOR, TORCH, WTORCH, LAMP, LEVER, BUTTON, DISPLAY };
/* DISPLAY (the redstone_plus display block): facing = op_nw, delay = op_se (0 none; then set/clear/show/check x r,g,b; 13 latch),
 * output = colour bits, F_POWERED = the run's check output. See RedstoneComponents.cpp "DisplayBlock". */
enum { DOWN = 0, UP, NORTH, SOUTH, WEST, EAST };
enum { F_LIT = 1, F_POWERED = 2, F_INSTANT = 4, F_SUBTRACT = 8, F_FACE_FLOOR = 16, F_FACE_CEILING = 32 };
enum { PRIO_EXTREMELY_HIGH = -3, PRIO_VERY_HIGH = -2, PRIO_HIGH = -1, PRIO_NORMAL = 0 };
enum { S_NONE = 0, S_SIDE = 1, S_UP = 2 };

static const int DX[6] = {0, 0, 0, 0, -1, 1};
static const int DY[6] = {-1, 1, 0, 0, 0, 0};
static const int DZ[6] = {0, 0, -1, 1, 0, 0};
static const int HORIZ[4] = {NORTH, EAST, SOUTH, WEST};
static inline int opp(int d) { return d ^ 1; }
static inline int cw(int d)  { return d == NORTH ? EAST : d == EAST ? SOUTH : d == SOUTH ? WEST : NORTH; }
static inline int ccw(int d) { return d == NORTH ? WEST : d == WEST ? SOUTH : d == SOUTH ? EAST : NORTH; }

typedef struct {
    uint8_t kind, facing, flags, power, output, delay;
    uint16_t sides;       /* wire: bit 8 valid; 2 bits per HORIZ index (north, east, south, west) in bits 0..7 */
    uint8_t pending;      /* a scheduled tick exists for this cell */
    uint8_t dirty;
    uint8_t flip_count;   /* blue torch: instant flips this tick */
    uint8_t queued;       /* wire: on the resettle worklist */
    uint8_t deferred;     /* delayed component listed for the end-of-cascade re-check */
    int64_t flip_tick;
    int64_t run_tick;     /* == time while the cell's tick is collected but not yet run */
    uint32_t mark;        /* dedupe stamp (notification sets, component membership) */
    int32_t  cidx;        /* index in the current component (valid when mark matches) */
} Cell;

#define CH 16
typedef struct { int cx, cy, cz; Cell cells[CH * CH * CH]; } Chunk;

typedef struct { int64_t time; int64_t seq; int32_t x, y, z; int8_t prio; } Ev;

typedef struct {
    Chunk** table; size_t cap, count;
    int64_t time, seq; int no_decay; int wires_muted; uint32_t mark;
    Ev* heap; size_t hn, hcap;
    int32_t* dirty; size_t dn, dcap;
    int32_t* comp; size_t compn, compcap;      /* component cells (x,y,z triples) */
    int32_t* queue; size_t qcap;               /* worklist ring of component indices */
    uint8_t* oldp; size_t oldcap;
    int32_t* notify; size_t nn, ncap;          /* notification list (x,y,z triples) */
    int32_t* defer; size_t dfn, dfcap;         /* deferred re-checks (x,y,z triples) */
    int64_t stat_notifies, stat_settles;
} Sim;

/* ── chunk map ─────────────────────────────────────────────────────────── */
static inline int fdiv(int a) { return a >= 0 ? a / CH : -((-a + CH - 1) / CH); }
static inline uint64_t ckey(int cx, int cy, int cz) {
    uint64_t h = (uint64_t)(uint32_t)cx * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uint32_t)cy * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)(uint32_t)cz * 0x165667B19E3779F9ull;
    return h ^ (h >> 29);
}
static Chunk* chunk_find(Sim* s, int cx, int cy, int cz, int create) {
    if (s->cap == 0) {
        if (!create) return NULL;
        s->cap = 1024; s->table = (Chunk**)calloc(s->cap, sizeof(Chunk*));
    }
    for (;;) {
        size_t i = (size_t)(ckey(cx, cy, cz) & (s->cap - 1));
        for (;;) {
            Chunk* c = s->table[i];
            if (!c) break;
            if (c->cx == cx && c->cy == cy && c->cz == cz) return c;
            i = (i + 1) & (s->cap - 1);
        }
        if (!create) return NULL;
        if ((s->count + 1) * 2 > s->cap) {
            size_t ncap = s->cap * 2; Chunk** nt = (Chunk**)calloc(ncap, sizeof(Chunk*));
            for (size_t j = 0; j < s->cap; ++j) {
                Chunk* c = s->table[j]; if (!c) continue;
                size_t k = (size_t)(ckey(c->cx, c->cy, c->cz) & (ncap - 1));
                while (nt[k]) k = (k + 1) & (ncap - 1);
                nt[k] = c;
            }
            free(s->table); s->table = nt; s->cap = ncap;
            continue;
        }
        Chunk* c = (Chunk*)calloc(1, sizeof(Chunk)); c->cx = cx; c->cy = cy; c->cz = cz;
        s->table[i] = c; s->count++;
        return c;
    }
}
static inline Cell* cell_at(Sim* s, int x, int y, int z, int create) {
    Chunk* c = chunk_find(s, fdiv(x), fdiv(y), fdiv(z), create);
    if (!c) return NULL;
    int lx = x - c->cx * CH, ly = y - c->cy * CH, lz = z - c->cz * CH;
    Cell* q = &c->cells[(ly * CH + lz) * CH + lx];
    if (!create && q->kind == AIR) return NULL;
    return q;
}
static inline Cell* at(Sim* s, int x, int y, int z) { return cell_at(s, x, y, z, 0); }
static inline int kind_at(Sim* s, int x, int y, int z) { Cell* c = at(s, x, y, z); return c ? c->kind : AIR; }

/* ── dirty ─────────────────────────────────────────────────────────────── */
static void mark_dirty(Sim* s, Cell* c, int x, int y, int z) {
    if (c->dirty) return;
    c->dirty = 1;
    if (s->dn + 3 > s->dcap) { s->dcap = s->dcap ? s->dcap * 2 : 4096; s->dirty = (int32_t*)realloc(s->dirty, s->dcap * sizeof(int32_t)); }
    s->dirty[s->dn++] = x; s->dirty[s->dn++] = y; s->dirty[s->dn++] = z;
}

/* ── classification ────────────────────────────────────────────────────── */
static inline int is_conductor_k(int k) { return k == SOLID || k == LAMP; }
static inline int is_source_k(int k) { return k == WIRE || k == REPEATER || k == COMPARATOR || k == TORCH || k == WTORCH || k == LEVER || k == BUTTON || k == DISPLAY; }
static inline int is_diode_k(int k) { return k == REPEATER || k == COMPARATOR; }
static inline int sturdy_up_k(int k) { return k == SOLID || k == LAMP || k == GLOW || k == DISPLAY; }
static inline int lever_connected_dir(const Cell* c) {
    if (c->flags & F_FACE_CEILING) return DOWN;
    if (c->flags & F_FACE_FLOOR) return UP;
    return c->facing;
}

/* ── wire shape (getConnectionState) ───────────────────────────────────── */
static int should_connect_to(Sim* s, int x, int y, int z, int direction /* -1 = none */) {
    Cell* c = at(s, x, y, z);
    if (!c) return 0;
    if (c->kind == WIRE) return 1;
    if (c->kind == REPEATER) return direction >= 0 && (c->facing == direction || opp(c->facing) == direction);
    return is_source_k(c->kind) && direction >= 0;
}
static int connecting_side(Sim* s, int x, int y, int z, int d, int can_up) {
    int rx = x + DX[d], ry = y + DY[d], rz = z + DZ[d];
    int rk = kind_at(s, rx, ry, rz);
    if (can_up && sturdy_up_k(rk) && should_connect_to(s, rx, ry + 1, rz, -1)) return S_UP;
    if (!should_connect_to(s, rx, ry, rz, d) && (is_conductor_k(rk) || !should_connect_to(s, rx, ry - 1, rz, -1))) return S_NONE;
    return S_SIDE;
}
static inline int horiz_index(int d) { return d == NORTH ? 0 : d == EAST ? 1 : d == SOUTH ? 2 : 3; }
static int wire_side(Sim* s, Cell* c, int x, int y, int z, int d) {
    if (!(c->sides & 0x100)) {
        int can_up = !is_conductor_k(kind_at(s, x, y + 1, z));
        int sd[4];
        for (int i = 0; i < 4; ++i) sd[i] = connecting_side(s, x, y, z, HORIZ[i], can_up);
        int n = sd[0] != S_NONE, e = sd[1] != S_NONE, so = sd[2] != S_NONE, w = sd[3] != S_NONE;
        int ns_empty = !n && !so, ew_empty = !e && !w;
        if (!w && ns_empty) sd[3] = S_SIDE;
        if (!e && ns_empty) sd[1] = S_SIDE;
        if (!n && ew_empty) sd[0] = S_SIDE;
        if (!so && ew_empty) sd[2] = S_SIDE;
        c->sides = (uint16_t)(0x100 | sd[0] | (sd[1] << 2) | (sd[2] << 4) | (sd[3] << 6));   /* the west side once shared bit 7 with the valid flag: every wire read as connected west */
    }
    return (c->sides >> (2 * horiz_index(d))) & 3;
}

/* ── signals ───────────────────────────────────────────────────────────── */
static int block_signal(Sim* s, int x, int y, int z, int direction) {
    Cell* c = at(s, x, y, z);
    if (!c) return 0;
    switch (c->kind) {
        case WIRE:
            if (s->wires_muted || direction == DOWN) return 0;
            if (c->power == 0) return 0;
            if (direction == UP) return c->power;
            return wire_side(s, c, x, y, z, opp(direction)) != S_NONE ? c->power : 0;
        case REPEATER:   return ((c->flags & F_POWERED) && c->facing == direction) ? 15 : 0;
        case COMPARATOR: return c->facing == direction ? c->output : 0;
        case TORCH:      return ((c->flags & F_LIT) && direction != UP) ? 15 : 0;
        case WTORCH:     return ((c->flags & F_LIT) && c->facing != direction) ? 15 : 0;
        case LEVER: case BUTTON: return (c->flags & F_POWERED) ? 15 : 0;
        case DISPLAY:    return ((c->flags & F_POWERED) && direction == DOWN) ? 15 : 0;
        default: return 0;
    }
}
static int block_direct_signal(Sim* s, int x, int y, int z, int direction) {
    Cell* c = at(s, x, y, z);
    if (!c) return 0;
    switch (c->kind) {
        case WIRE: return s->wires_muted ? 0 : block_signal(s, x, y, z, direction);
        case REPEATER: case COMPARATOR: return block_signal(s, x, y, z, direction);
        case TORCH: case WTORCH: return direction == DOWN ? block_signal(s, x, y, z, direction) : 0;
        case LEVER: case BUTTON: return ((c->flags & F_POWERED) && lever_connected_dir(c) == direction) ? 15 : 0;
        case DISPLAY: return direction == DOWN ? block_signal(s, x, y, z, direction) : 0;
        default: return 0;
    }
}
static int direct_signal_to(Sim* s, int x, int y, int z) {
    int best = 0;
    for (int d = 0; d < 6; ++d) {
        int v = block_direct_signal(s, x + DX[d], y + DY[d], z + DZ[d], d);
        if (v > best) best = v;
        if (best >= 15) return best;
    }
    return best;
}
static int sig_at(Sim* s, int x, int y, int z, int direction) {
    int v = block_signal(s, x, y, z, direction);
    if (is_conductor_k(kind_at(s, x, y, z))) { int d = direct_signal_to(s, x, y, z); if (d > v) v = d; }
    return v;
}
static inline int has_signal(Sim* s, int x, int y, int z, int d) { return sig_at(s, x, y, z, d) > 0; }
static int has_neighbor_signal(Sim* s, int x, int y, int z) {
    for (int d = 0; d < 6; ++d) if (sig_at(s, x + DX[d], y + DY[d], z + DZ[d], d) > 0) return 1;
    return 0;
}
static int best_neighbor_signal(Sim* s, int x, int y, int z) {
    int best = 0;
    for (int d = 0; d < 6; ++d) {
        int v = sig_at(s, x + DX[d], y + DY[d], z + DZ[d], d);
        if (v >= 15) return 15;
        if (v > best) best = v;
    }
    return best;
}
static int control_input_signal(Sim* s, int x, int y, int z, int direction, int only_diodes) {
    Cell* c = at(s, x, y, z);
    if (!c) return 0;
    if (only_diodes) return is_diode_k(c->kind) ? block_direct_signal(s, x, y, z, direction) : 0;
    if (c->kind == WIRE) return c->power;
    if (is_source_k(c->kind)) return block_direct_signal(s, x, y, z, direction);
    return 0;
}
static inline int wire_power_of(Sim* s, int x, int y, int z) { Cell* c = at(s, x, y, z); return (c && c->kind == WIRE) ? c->power : 0; }

static int wire_target(Sim* s, int x, int y, int z) {
    s->wires_muted = 1;
    int bs = best_neighbor_signal(s, x, y, z);
    s->wires_muted = 0;
    if (bs == 15) return 15;
    int incoming = 0;
    int above_conductor = is_conductor_k(kind_at(s, x, y + 1, z));
    for (int i = 0; i < 4; ++i) {
        int d = HORIZ[i], nx = x + DX[d], nz = z + DZ[d];
        int v = wire_power_of(s, nx, y, nz); if (v > incoming) incoming = v;
        int nk = kind_at(s, nx, y, nz);
        if (is_conductor_k(nk) && !above_conductor) { v = wire_power_of(s, nx, y + 1, nz); if (v > incoming) incoming = v; }
        else if (!is_conductor_k(nk)) { v = wire_power_of(s, nx, y - 1, nz); if (v > incoming) incoming = v; }
    }
    if (s->no_decay) return bs > incoming ? bs : incoming;
    int dec = incoming - 1; if (dec < 0) dec = 0;
    return bs > dec ? bs : dec;
}

/* ── diodes / torches ──────────────────────────────────────────────────── */
static int diode_input(Sim* s, Cell* c, int x, int y, int z) {
    int f = c->facing, rx = x + DX[f], ry = y + DY[f], rz = z + DZ[f];
    int v = sig_at(s, rx, ry, rz, f);
    if (v >= 15) return v;
    int w = wire_power_of(s, rx, ry, rz);
    return v > w ? v : w;
}
static int diode_alternate(Sim* s, Cell* c, int x, int y, int z) {
    int a = cw(c->facing), b = ccw(c->facing), only = c->kind == REPEATER;
    int va = control_input_signal(s, x + DX[a], y + DY[a], z + DZ[a], a, only);
    int vb = control_input_signal(s, x + DX[b], y + DY[b], z + DZ[b], b, only);
    return va > vb ? va : vb;
}
static int comparator_output(Sim* s, Cell* c, int x, int y, int z) {
    int inp = diode_input(s, c, x, y, z);
    if (inp == 0) return 0;
    int alt = diode_alternate(s, c, x, y, z);
    if (alt > inp) return 0;
    return (c->flags & F_SUBTRACT) ? inp - alt : inp;
}
static int comparator_should_turn_on(Sim* s, Cell* c, int x, int y, int z) {
    int inp = diode_input(s, c, x, y, z);
    if (inp == 0) return 0;
    int side = diode_alternate(s, c, x, y, z);
    return inp > side || (inp == side && !(c->flags & F_SUBTRACT));
}
static int should_prioritize(Sim* s, Cell* c, int x, int y, int z) {
    int o = opp(c->facing);
    Cell* f = at(s, x + DX[o], y + DY[o], z + DZ[o]);
    return f && is_diode_k(f->kind) && f->facing != o;
}
static int torch_has_signal(Sim* s, Cell* c, int x, int y, int z) {
    if (c->kind == TORCH) return has_signal(s, x, y - 1, z, DOWN);
    int o = opp(c->facing);
    return has_signal(s, x + DX[o], y + DY[o], z + DZ[o], o);
}

/* ── scheduling ────────────────────────────────────────────────────────── */
static inline int ev_less(const Ev* a, const Ev* b) {
    if (a->time != b->time) return a->time < b->time;
    if (a->prio != b->prio) return a->prio < b->prio;
    return a->seq < b->seq;
}
static void heap_push(Sim* s, Ev e) {
    if (s->hn == s->hcap) { s->hcap = s->hcap ? s->hcap * 2 : 1024; s->heap = (Ev*)realloc(s->heap, s->hcap * sizeof(Ev)); }
    size_t i = s->hn++;
    s->heap[i] = e;
    while (i > 0) { size_t p = (i - 1) / 2; if (!ev_less(&s->heap[i], &s->heap[p])) break; Ev t = s->heap[i]; s->heap[i] = s->heap[p]; s->heap[p] = t; i = p; }
}
static Ev heap_pop(Sim* s) {
    Ev top = s->heap[0];
    s->heap[0] = s->heap[--s->hn];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < s->hn && ev_less(&s->heap[l], &s->heap[m])) m = l;
        if (r < s->hn && ev_less(&s->heap[r], &s->heap[m])) m = r;
        if (m == i) break;
        Ev t = s->heap[i]; s->heap[i] = s->heap[m]; s->heap[m] = t; i = m;
    }
    return top;
}
static void schedule(Sim* s, Cell* c, int x, int y, int z, int delay, int prio) {
    if (c->pending) return;
    c->pending = 1;
    Ev e; e.time = s->time + delay; e.prio = (int8_t)prio; e.seq = ++s->seq; e.x = x; e.y = y; e.z = z;
    heap_push(s, e);
}
static inline int will_tick_this_tick(Sim* s, Cell* c) { return c->run_tick == s->time; }

/* ── updates ───────────────────────────────────────────────────────────── */
static void neighbor_changed(Sim* s, int x, int y, int z);

static void notify_around(Sim* s, int x, int y, int z) {
    uint32_t m = ++s->mark;
    const int px[7] = {x, x, x, x, x, x - 1, x + 1}, py[7] = {y, y - 1, y + 1, y, y, y, y}, pz[7] = {z, z, z, z - 1, z + 1, z, z};
    /* collect first (marks), then notify: a nested notify_around must not reuse our mark */
    int32_t buf[7 * 6 * 3]; int nb = 0;
    for (int i = 0; i < 7; ++i) for (int d = 0; d < 6; ++d) {
        int qx = px[i] + DX[d], qy = py[i] + DY[d], qz = pz[i] + DZ[d];
        if (qx == x && qy == y && qz == z) continue;
        Cell* c = at(s, qx, qy, qz);
        if (!c || c->mark == m) continue;
        c->mark = m;
        buf[nb++] = qx; buf[nb++] = qy; buf[nb++] = qz;
    }
    for (int i = 0; i < nb; i += 3) neighbor_changed(s, buf[i], buf[i + 1], buf[i + 2]);
}

static void comp_push(Sim* s, int x, int y, int z) {
    if (s->compn + 3 > s->compcap) { s->compcap = s->compcap ? s->compcap * 2 : 4096; s->comp = (int32_t*)realloc(s->comp, s->compcap * sizeof(int32_t)); }
    s->comp[s->compn++] = x; s->comp[s->compn++] = y; s->comp[s->compn++] = z;
}
static void notify_push(Sim* s, int x, int y, int z) {
    if (s->nn + 3 > s->ncap) { s->ncap = s->ncap ? s->ncap * 2 : 4096; s->notify = (int32_t*)realloc(s->notify, s->ncap * sizeof(int32_t)); }
    s->notify[s->nn++] = x; s->notify[s->nn++] = y; s->notify[s->nn++] = z;
}

static const int RDX[4] = {1, -1, 0, 0}, RDZ[4] = {0, 0, 1, -1};   /* wire_readers order */

/* _resettle_wire: the component through wire_readers, least fixpoint from
 * zero, then vanilla's notifications for every changed wire. Re-entrant:
 * nested settles (from a blue torch flipping inside a notification) use
 * their own component buffers because ours are consumed before notifying. */
static void resettle_wire(Sim* s, int sx, int sy, int sz) {
    s->stat_settles++;
    uint32_t m = ++s->mark;
    size_t base = s->compn;                       /* nested calls append after us; we only use [base, end) */
    Cell* sc = at(s, sx, sy, sz); sc->mark = m; sc->cidx = 0; comp_push(s, sx, sy, sz);
    for (size_t i = base; i < s->compn; i += 3) {
        int x = s->comp[i], y = s->comp[i + 1], z = s->comp[i + 2];
        for (int k = 0; k < 4; ++k) for (int dy = -1; dy <= 1; ++dy) {
            int qx = x + RDX[k], qy = y + dy, qz = z + RDZ[k];
            Cell* c = at(s, qx, qy, qz);
            if (c && c->kind == WIRE && c->mark != m) { c->mark = m; c->cidx = (int32_t)((s->compn - base) / 3); comp_push(s, qx, qy, qz); }
        }
    }
    size_t n = (s->compn - base) / 3;
    int32_t* comp = (int32_t*)malloc(n * 3 * sizeof(int32_t));
    memcpy(comp, s->comp + base, n * 3 * sizeof(int32_t));
    s->compn = base;                               /* release the shared buffer for nested calls */
    uint8_t* oldp = (uint8_t*)malloc(n);
    int32_t* queue = (int32_t*)malloc(n * sizeof(int32_t));
    for (size_t i = 0; i < n; ++i) { Cell* c = at(s, comp[3*i], comp[3*i+1], comp[3*i+2]); oldp[i] = c->power; c->power = 0; c->queued = 1; queue[i] = (int32_t)i; }
    size_t qh = 0, qlen = n;
    while (qlen) {
        size_t i = (size_t)queue[qh]; qh = (qh + 1) % n; qlen--;
        int x = comp[3*i], y = comp[3*i+1], z = comp[3*i+2];
        Cell* c = at(s, x, y, z); c->queued = 0;
        int t = wire_target(s, x, y, z);
        if (t != c->power) {
            c->power = (uint8_t)t;
            for (int k = 0; k < 4; ++k) for (int dy = -1; dy <= 1; ++dy) {
                Cell* r = at(s, x + RDX[k], y + dy, z + RDZ[k]);
                if (!r || r->kind != WIRE || r->mark != m || r->queued) continue;
                r->queued = 1; queue[(qh + qlen) % n] = r->cidx; qlen++;
            }
        }
    }
    /* changed wires -> dirty, and the two-level notification set (first-seen order, component excluded) */
    uint32_t nm = ++s->mark;
    size_t nbase = s->nn;
    for (size_t i = 0; i < n; ++i) {
        int x = comp[3*i], y = comp[3*i+1], z = comp[3*i+2];
        Cell* c = at(s, x, y, z);
        if (c->power == oldp[i]) continue;
        mark_dirty(s, c, x, y, z);
        const int px[7] = {x, x, x, x, x, x - 1, x + 1}, py[7] = {y, y - 1, y + 1, y, y, y, y}, pz[7] = {z, z, z, z - 1, z + 1, z, z};
        for (int j = 0; j < 7; ++j) for (int d = 0; d < 6; ++d) {
            int qx = px[j] + DX[d], qy = py[j] + DY[d], qz = pz[j] + DZ[d];
            Cell* q = at(s, qx, qy, qz);
            if (!q || q->mark == m || q->mark == nm) continue;   /* in the component, or already listed */
            q->mark = nm;
            notify_push(s, qx, qy, qz);
        }
    }
    free(queue); free(oldp); free(comp);
    size_t nend = s->nn;
    int32_t* list = (int32_t*)malloc((nend - nbase) * sizeof(int32_t));
    memcpy(list, s->notify + nbase, (nend - nbase) * sizeof(int32_t));
    s->nn = nbase;
    for (size_t i = 0; i < nend - nbase; i += 3) { s->stat_notifies++; neighbor_changed(s, list[i], list[i + 1], list[i + 2]); }
    free(list);
}

/* The vanilla neighborChanged decision of a delayed component. */
/* ── the display block ─────────────────────────────────────────────────── */
static int display_face_in(Sim* s, int x, int y, int z, int d) {
    int nx = x + DX[d], ny = y + DY[d], nz = z + DZ[d];
    Cell* n = at(s, nx, ny, nz);
    if (!n) return 0;
    if (n->kind == WIRE) return n->power > 0;
    return sig_at(s, nx, ny, nz, d) > 0;
}
/* The group (face-connected display blocks) containing (x,y,z), lowest block
 * first (y, then z, then x) — the engine's DisplayGroup. Uses the component
 * buffer (s->comp) and the mark stamp like the wire evaluator. */
static int display_group_less(const void* a, const void* b) {
    const int32_t* p = (const int32_t*)a; const int32_t* q = (const int32_t*)b;
    if (p[1] != q[1]) return p[1] < q[1] ? -1 : 1;
    if (p[2] != q[2]) return p[2] < q[2] ? -1 : 1;
    return p[0] < q[0] ? -1 : (p[0] > q[0]);
}
static int32_t* display_group(Sim* s, int x, int y, int z, size_t* count) {
    uint32_t m = ++s->mark;
    size_t base = s->compn;                        /* nested in a resettle: append after it, release when done */
    Cell* c0 = at(s, x, y, z); c0->mark = m;
    comp_push(s, x, y, z);
    for (size_t i = base; i < s->compn; i += 3) {
        int px = s->comp[i], py = s->comp[i + 1], pz = s->comp[i + 2];
        Cell* pc = at(s, px, py, pz);
        for (int d = 0; d < 6; ++d) {
            int nx = px + DX[d], ny = py + DY[d], nz = pz + DZ[d];
            Cell* n = at(s, nx, ny, nz);
            if (!n || n->kind != DISPLAY || n->mark == m) continue;
            /* a latch's data face (north for op_nw = facing, south for op_se = delay) is a group boundary */
            if ((d == NORTH && pc->facing == 13) || (d == SOUTH && pc->delay == 13) ||
                (d == SOUTH && n->facing == 13) || (d == NORTH && n->delay == 13)) continue;
            n->mark = m;
            comp_push(s, nx, ny, nz);
        }
    }
    size_t n = (s->compn - base) / 3;
    int32_t* g = (int32_t*)malloc(n * 3 * sizeof(int32_t));
    memcpy(g, s->comp + base, n * 3 * sizeof(int32_t));
    s->compn = base;
    qsort(g, n, 3 * sizeof(int32_t), display_group_less);
    *count = n;
    return g;
}
static void display_eval(Sim* s, int x, int y, int z) {
    if (kind_at(s, x, y, z) != DISPLAY) return;
    size_t n = 0;
    int32_t* g = display_group(s, x, y, z, &n);
    int bits = at(s, g[0], g[1], g[2])->output, out = 0;
    for (size_t i = 0; i < n; ++i) {
        if (display_face_in(s, g[3 * i], g[3 * i + 1], g[3 * i + 2], DOWN)) { bits = 0; break; }
    }
    for (size_t i = 0; i < n; ++i) {
        int px = g[3 * i], py = g[3 * i + 1], pz = g[3 * i + 2];
        Cell* c = at(s, px, py, pz);
        int ycol = kind_at(s, px, py + 2, pz) == DISPLAY ? py + 2 : py;
        int ops[2] = { c->facing, c->delay };
        int hit[2] = { display_face_in(s, px, py, pz, NORTH) && display_face_in(s, px, ycol, pz, WEST),
                       display_face_in(s, px, py, pz, SOUTH) && display_face_in(s, px, ycol, pz, EAST) };
        for (int k = 0; k < 2; ++k) {
            int op = ops[k];
            if (op <= 0 || op >= 14) continue;
            if (op == 13) {                      /* latch: clocked by the column face, copies the group across the row face */
                if (!display_face_in(s, px, ycol, pz, k == 0 ? WEST : EAST)) continue;
                int sz = pz + (k == 0 ? -1 : 1);
                Cell* src = at(s, px, py, sz);
                bits = (src && src->kind == DISPLAY) ? src->output : 0;
                continue;
            }
            int mask = 1 << ((op - 1) % 3);
            switch ((op - 1) / 3) {
                case 0: if (hit[k]) bits |= mask; break;
                case 1: if (hit[k]) bits &= ~mask; break;
                case 2: if (hit[k]) bits |= mask; else bits &= ~mask; break;
                default: if (hit[k] && (bits & mask)) out = 1; break;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        int px = g[3 * i], py = g[3 * i + 1], pz = g[3 * i + 2];
        Cell* c = at(s, px, py, pz);
        int pw = out && kind_at(s, px, py + 1, pz) != DISPLAY;
        int changed = (c->output != bits) || (((c->flags & F_POWERED) != 0) != pw);
        if (!changed) continue;
        c->output = (uint8_t)bits;
        if (pw) c->flags |= F_POWERED; else c->flags &= ~F_POWERED;
        mark_dirty(s, c, px, py, pz);
        notify_around(s, px, py, pz);
    }
    free(g);
}

static void check_component(Sim* s, Cell* c, int x, int y, int z) {
    switch (c->kind) {
        case DISPLAY: display_eval(s, x, y, z); break;
        case TORCH: case WTORCH: {
            int lit = (c->flags & F_LIT) != 0;
            if (lit != torch_has_signal(s, c, x, y, z)) return;
            if (!will_tick_this_tick(s, c)) schedule(s, c, x, y, z, 2, PRIO_NORMAL);
            break;
        }
        case REPEATER: {
            if (diode_alternate(s, c, x, y, z) > 0) return;          /* locked */
            int on = (c->flags & F_POWERED) != 0;
            int should = diode_input(s, c, x, y, z) > 0;
            if (on != should && !will_tick_this_tick(s, c)) {
                int pr = PRIO_HIGH;
                if (should_prioritize(s, c, x, y, z)) pr = PRIO_EXTREMELY_HIGH;
                else if (on) pr = PRIO_VERY_HIGH;
                schedule(s, c, x, y, z, c->delay * 2, pr);
            }
            break;
        }
        case COMPARATOR: {
            if (will_tick_this_tick(s, c)) return;
            int nw = comparator_output(s, c, x, y, z);
            if (nw != c->output || ((c->flags & F_POWERED) != 0) != comparator_should_turn_on(s, c, x, y, z)) {
                schedule(s, c, x, y, z, 2, should_prioritize(s, c, x, y, z) ? PRIO_HIGH : PRIO_NORMAL);
            }
            break;
        }
        case LAMP: {
            int has = has_neighbor_signal(s, x, y, z);
            int lit = (c->flags & F_LIT) != 0;
            if (lit != has) {
                if (lit) schedule(s, c, x, y, z, 4, PRIO_NORMAL);
                else { c->flags |= F_LIT; mark_dirty(s, c, x, y, z); }
            }
            break;
        }
        default: break;
    }
}

/* redstone_plus: delayed components re-check once the cascade has settled
 * (end of tick / after an input event) — order-independent and glitch-free;
 * mirrors RedstoneComponents.cpp's deferred re-checks. */
static void defer(Sim* s, Cell* c, int x, int y, int z) {
    if (c->deferred) return;
    c->deferred = 1;
    if (s->dfn + 3 > s->dfcap) { s->dfcap = s->dfcap ? s->dfcap * 2 : 4096; s->defer = (int32_t*)realloc(s->defer, s->dfcap * sizeof(int32_t)); }
    s->defer[s->dfn++] = x; s->defer[s->dfn++] = y; s->defer[s->dfn++] = z;
}
static void flush_deferred(Sim* s) {
    for (int round = 0; round < 8 && s->dfn; ++round) {
        size_t n = s->dfn;
        int32_t* list = (int32_t*)malloc(n * sizeof(int32_t));
        memcpy(list, s->defer, n * sizeof(int32_t));
        s->dfn = 0;
        for (size_t i = 0; i < n; i += 3) {
            Cell* c = at(s, list[i], list[i + 1], list[i + 2]);
            if (!c) continue;
            c->deferred = 0;
            check_component(s, c, list[i], list[i + 1], list[i + 2]);
        }
        free(list);
    }
}

static void neighbor_changed(Sim* s, int x, int y, int z) {
    Cell* c = at(s, x, y, z);
    if (!c) return;
    switch (c->kind) {
        case WIRE: resettle_wire(s, x, y, z); break;
        case TORCH: case WTORCH: {
            if ((c->flags & F_INSTANT) && s->no_decay) {
                int lit = (c->flags & F_LIT) != 0;
                if (lit != torch_has_signal(s, c, x, y, z)) return;
                if (c->flip_tick != s->time) { c->flip_tick = s->time; c->flip_count = 0; }
                if (c->flip_count < 8) {
                    c->flip_count++;
                    c->flags ^= F_LIT; mark_dirty(s, c, x, y, z);
                    notify_around(s, x, y, z);
                    return;
                }
            }
            if (s->no_decay) { defer(s, c, x, y, z); return; }
            check_component(s, c, x, y, z);
            break;
        }
        case REPEATER: case COMPARATOR: case LAMP: case DISPLAY:
            if (s->no_decay) { defer(s, c, x, y, z); return; }
            check_component(s, c, x, y, z);
            break;
        default: break;
    }
}

static void block_tick(Sim* s, Cell* c, int x, int y, int z) {
    switch (c->kind) {
        case TORCH: case WTORCH: {
            int sig = torch_has_signal(s, c, x, y, z);
            int lit = (c->flags & F_LIT) != 0;
            if (lit && sig) { c->flags &= ~F_LIT; mark_dirty(s, c, x, y, z); notify_around(s, x, y, z); }
            else if (!lit && !sig) { c->flags |= F_LIT; mark_dirty(s, c, x, y, z); notify_around(s, x, y, z); }
            break;
        }
        case REPEATER: {
            if (diode_alternate(s, c, x, y, z) > 0) return;
            int on = (c->flags & F_POWERED) != 0;
            int should = diode_input(s, c, x, y, z) > 0;
            if (on && !should) { c->flags &= ~F_POWERED; mark_dirty(s, c, x, y, z); notify_around(s, x, y, z); }
            else if (!on) {
                c->flags |= F_POWERED; mark_dirty(s, c, x, y, z); notify_around(s, x, y, z);
                if (!should) schedule(s, c, x, y, z, c->delay * 2, PRIO_VERY_HIGH);
            }
            break;
        }
        case COMPARATOR: {
            int nw = comparator_output(s, c, x, y, z), old = c->output;
            c->output = (uint8_t)nw;
            if (old != nw || !(c->flags & F_SUBTRACT)) {
                int should = comparator_should_turn_on(s, c, x, y, z);
                if (((c->flags & F_POWERED) != 0) != should) { if (should) c->flags |= F_POWERED; else c->flags &= ~F_POWERED; }
                mark_dirty(s, c, x, y, z);
                notify_around(s, x, y, z);
            } else if (old != nw) mark_dirty(s, c, x, y, z);
            break;
        }
        case LAMP:
            if ((c->flags & F_LIT) && !has_neighbor_signal(s, x, y, z)) { c->flags &= ~F_LIT; mark_dirty(s, c, x, y, z); }
            break;
        case BUTTON:
            if (c->flags & F_POWERED) { c->flags &= ~F_POWERED; mark_dirty(s, c, x, y, z); notify_around(s, x, y, z); }
            break;
        default: break;
    }
}

/* ── API ───────────────────────────────────────────────────────────────── */
Sim* rs_create(int no_decay) { Sim* s = (Sim*)calloc(1, sizeof(Sim)); s->no_decay = no_decay; s->mark = 1; return s; }
void rs_destroy(Sim* s) {
    if (!s) return;
    for (size_t i = 0; i < s->cap; ++i) free(s->table[i]);
    free(s->table); free(s->heap); free(s->dirty); free(s->comp); free(s->queue); free(s->oldp); free(s->notify); free(s->defer); free(s);
}
void rs_add_cells(Sim* s, int n, const int32_t* xyz, const uint8_t* kind, const uint8_t* facing, const uint8_t* flags,
                  const uint8_t* power, const uint8_t* output, const uint8_t* delay) {
    for (int i = 0; i < n; ++i) {
        Cell* c = cell_at(s, xyz[3*i], xyz[3*i+1], xyz[3*i+2], 1);
        c->kind = kind[i]; c->facing = facing[i]; c->flags = flags[i]; c->power = power[i]; c->output = output[i]; c->delay = delay[i];
        c->sides = 0; c->pending = 0; c->dirty = 0; c->flip_count = 0; c->queued = 0; c->deferred = 0; c->flip_tick = -1; c->run_tick = -1; c->mark = 0; c->cidx = 0;
    }
}
void rs_set_clock(Sim* s, int64_t time, int64_t seq) { s->time = time; s->seq = seq; }
int64_t rs_time(Sim* s) { return s->time; }
void rs_schedule_at(Sim* s, int32_t x, int32_t y, int32_t z, int64_t time, int prio, int64_t seq) {
    Cell* c = at(s, x, y, z);
    if (!c || c->pending) return;
    c->pending = 1;
    Ev e; e.time = time; e.prio = (int8_t)prio; e.seq = seq; e.x = x; e.y = y; e.z = z;
    heap_push(s, e);
    if (seq > s->seq) s->seq = seq;
}
void rs_tick(Sim* s, int n) {
    for (int t = 0; t < n; ++t) {
        s->time++;
        /* collect everything due, in heap order */
        size_t base = s->compn;      /* borrow the comp buffer as the due list (x,y,z triples) */
        while (s->hn && s->heap[0].time <= s->time) {
            Ev e = heap_pop(s);
            Cell* c = at(s, e.x, e.y, e.z);
            if (c) c->run_tick = s->time;
            comp_push(s, e.x, e.y, e.z);
        }
        size_t end = s->compn;
        int32_t* due = (int32_t*)malloc((end - base) * sizeof(int32_t));
        memcpy(due, s->comp + base, (end - base) * sizeof(int32_t));
        s->compn = base;
        for (size_t i = 0; i < end - base; i += 3) {
            Cell* c = at(s, due[i], due[i + 1], due[i + 2]);
            if (!c) continue;
            c->run_tick = -1;             /* runCollectedTicks drops it from the this-tick set as it runs */
            c->pending = 0;
            block_tick(s, c, due[i], due[i + 1], due[i + 2]);
        }
        free(due);
        flush_deferred(s);
    }
}
int rs_dirty_count(Sim* s) { return (int)(s->dn / 3); }
/* out arrays sized >= rs_dirty_count entries; returns the count and clears the list */
int rs_take_dirty(Sim* s, int32_t* xyz, uint8_t* power, uint8_t* flags, uint8_t* output) {
    int n = (int)(s->dn / 3);
    for (int i = 0; i < n; ++i) {
        int x = s->dirty[3*i], y = s->dirty[3*i+1], z = s->dirty[3*i+2];
        Cell* c = at(s, x, y, z);
        xyz[3*i] = x; xyz[3*i+1] = y; xyz[3*i+2] = z;
        power[i] = c ? c->power : 0; flags[i] = c ? c->flags : 0; output[i] = c ? c->output : 0;
        if (c) c->dirty = 0;
    }
    s->dn = 0;
    return n;
}
void rs_toggle_lever(Sim* s, int32_t x, int32_t y, int32_t z, int on) {
    Cell* c = at(s, x, y, z);
    if (!c) return;
    if (on) c->flags |= F_POWERED; else c->flags &= ~F_POWERED;
    mark_dirty(s, c, x, y, z);
    notify_around(s, x, y, z);
    flush_deferred(s);
}
void rs_press_button(Sim* s, int32_t x, int32_t y, int32_t z) {
    Cell* c = at(s, x, y, z);
    if (!c || (c->flags & F_POWERED)) return;
    c->flags |= F_POWERED; mark_dirty(s, c, x, y, z);
    notify_around(s, x, y, z);
    schedule(s, c, x, y, z, 20, PRIO_NORMAL);
    flush_deferred(s);
}
int rs_pending_count(Sim* s) { return (int)s->hn; }
void rs_pending(Sim* s, int64_t* time, int8_t* prio, int64_t* seq, int32_t* xyz) {
    for (size_t i = 0; i < s->hn; ++i) { time[i] = s->heap[i].time; prio[i] = s->heap[i].prio; seq[i] = s->heap[i].seq; xyz[3*i] = s->heap[i].x; xyz[3*i+1] = s->heap[i].y; xyz[3*i+2] = s->heap[i].z; }
}
int rs_peek(Sim* s, int32_t x, int32_t y, int32_t z, uint8_t* power, uint8_t* flags, uint8_t* output) {
    Cell* c = at(s, x, y, z);
    if (!c) return 0;
    *power = c->power; *flags = c->flags; *output = c->output;
    return 1;
}
void rs_stats(Sim* s, int64_t* settles, int64_t* notifies) { *settles = s->stat_settles; *notifies = s->stat_notifies; }
