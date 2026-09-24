# Redstone computer world generator

Generates the "Redstone Computer" save: a 4-bit ripple-carry adder with lever
inputs and lamp outputs, feeding a 4-to-16 decoder, a 16x7 diode-matrix ROM
and a seven-segment hex display of the sum. Every gate is vanilla redstone
(comparator XORs, torch ANDs/NOTs, repeater diodes, dust bridges), so the same
world works in Minecraft.

    python3 make_world.py            # writes ~/Library/Application Support/obeycraft/saves/Redstone Computer
    python3 make_world.py --test 24  # simulate random inputs through the whole machine
    python3 test_adder.py 4          # exhaustive 4-bit adder truth table (512 cases)
    python3 test_display.py --all    # all 16 digits

Files: `redsim.py` is a steady-state port of the vanilla signal rules
(SignalGetter, redstone wire, diodes, torches, lever, lamp); `build.py` places
blocks and the bridge template; `cells.py` holds the XOR and AND cells;
`adder.py` the full-adder slice; `display.py` the decoder, ROM and digit;
`writer.py` packs the settled circuit into Anvil regions plus level.dat.
Only the sim's pending block ticks are exported (t = delay, p = priority);
never give every component a tick — a repeater's tick always turns it on,
so an idle machine would fire thousands of pulses at load.
To release a lever from a script use `/setblock ... air` then place it off:
setting a lever's `powered` state directly notifies neither the block it
powers nor that block's neighbours (vanilla behaviour).

## The C tick engine (fastsim)

`fastsim.py` moves a settled, booted world's ticking into `fastsim_c/fastsim.c`
(a rule-for-rule port of redsim/redtick, compiled on first use with `cc`).
`play2.py` and `write_snake_world.py` attach it after `init_state`; pass
`--slow` to play2 to keep the Python ticker. Changed cells are copied back
after every `tick()`, so `w.blocks[p]` reads stay valid; `world.pending_ticks()`
replaces `_heap` for the writer. Verified bit-identical to the Python engine
on the Snake machine (every dynamic cell and the pending list after 300 and
700 ticks, with a button press); ~64x faster (a ten-step seed in ~20 s
instead of ten minutes). Building and settling (`settle`, `boot`) stay in
Python.

## redstone_plus mode

With the engine rule `redstone_plus` on (docs/redstone.md) dust carries its
full signal any distance, so `Machine3(plus=True)` / `write_snake_world.py
--plus` lays no transport repeaters (lanes, rows, screen lines), the sim's
wires do not decay (`World.no_decay`), the phases use `SPACING_PLUS` with a
`CLOCK_N_PLUS` oscillator, and the writer sets the rule in level.dat. The
harness read points come from `m.timing` (`TIMING_PLUS`). Re-measure with
`play2 --rec` before tightening anything further.
With `blue=True` (the default under plus) every gate torch is the blue
zero-delay torch (`Block.instant`, written as `blue_redstone_torch`); the sim
flips it inside the update, 8 times a tick at most, like the engine.

### Restart (plus machines)

The start lever restarts the game: flipping it ON SETs the RSTH latch
(`_build_reset_logic`, rows south of the board at `ZSOUTH`, the latch in a
strip west of the length counter). While RSTH is on the clock gate is held
shut (HOLD = PAUSE | RSTH), every register's write is held open (`.wl` =
NOR(W, RSTH)) and its D path forced (food registers: RSTX = RSTH stretched
8 ticks ORed onto D; DIR: the request decode reads RSTH as "west" and the
REQ latches drive DIR), the direction FIFOs shift "west" in with their
writes open (E_ctl reads RSTH), the head/tail counters walk to 7 on their
STEP lanes (`_walk_join`: NOR(RSTH_L, AT7, RCLK_L) joined onto
STEPX/STEPY/STEPTX/STEPTY), the DEAD latch is reset, every pixel is erased
by the board's CLEAR lines (`board.clear_lines`, driven by YCLEAR), and the
length counter is pulsed until it reads 1 (L.INR). The walk clock is a
4-tick pulse on each rising edge of RNG bit 0 (period 40): the toggle
flip-flops need a write pulse shorter than their own toggle, a 10-tick
RCLK pulse toggles them twice. RSTH only RESETs when the lever is off
again AND L == 1 AND every counter reads 7, so a lever flipped on and
straight off still completes the reset (about 300-700 ticks). The clock
gate then reopens only while CLK is low (HOLDS = HOLD | (HOLDS & CLK)):
a gate opened mid-pulse gives the sequencer a late first cycle whose E
collides with the next A. The initial state is `INIT_PLUS`: head (7,7)
heading west, food (0,0), length 1 (`m.init`; `play2.py restart` plays a
game, resets, and plays a second one against the model). Paused means "in
reset": `test_screen.py` pins the RSTH latch off to hold its patterns.
The plus wall check and DEAD latch live south of the board too (the
head-direction bits and the collision line get inverted copies on columns
west of the Y matrix, which no lane may cross past z = 0).

### The display-block panel (plus machines)

With the rule on the board is `panel.py`: 8x8 pixels of the engine's
`display_block` (docs/redstone.md), each a 20-block pillar at pitch 12 (the lines stop at level 10, so the upper half is a clean coloured column)
(body = green, food = red), driven by one dust line per matrix row and
column instead of a latch and taps per pixel. The matrix's active-low
select lines are inverted by a blue torch at the panel edge and shifted to
their level (rows: HYS/TYE at level 0, HYC/FY at level 4; columns: HX/TX
at 2, a copy of HX and FX at 6), every line on glowstone; the check
read-back is dust on top of each pillar, merged south of the panel into
the NCOL lane; the restart's CLEAR is a third tier (level 8/10) whose row
and column lines are one net. The screen itself is a 32x32 wall south-east
of the panel (`WALL_*` in panel.py), a FRAME BUFFER: each 3x3 face is its
own display group with a `latch` block in a stub behind it; a display-block
chain (one group with the pillar, so it carries the whole colour) leaves
each pillar above its lines, one level per board column and one lane per
board row so the 64 chains never touch, and ends at the latch's data face.
Eight PRESENT dust lines clock the latches, driven once per cycle at E
(after the tail erase at B and the head set at D) and held during the
restart, so the wall only ever shows complete frames: the tail vanishes
and the new head appears in the same instant. The deck stands east of
the wall looking west (`DECK_ORIGIN`); board column 0 is on the left, row
0 at the top, matching the D-pad. `play2.py` compares the wall with the
board one cycle late and reads the pixels'
bits (`World.display_bits`); `test_screen.py`/`check_nets.py` are for the
lamp-wall (vanilla) machine only. `test_panel.py` drives the panel's
matrix-side lines with levers and checks every pixel in isolation.

## Loading only the machine

`writer.py` also writes `data/chunks.dat` (vanilla /forceload) listing every
chunk the build touches, so the world opens with exactly those chunks loaded
and ticking at any simulation distance; the engine's `redstone_chunks` rule
does the same automatically for any saved chunk that holds redstone.

## Redstone Snake (v3)

`write_snake_world.py` writes an 8x8 Snake machine as a world ("Redstone Snake").
With redstone_plus (the default) the board is a panel of display blocks
(green pillars = the snake, red = the food; see "The display-block panel")
and you spawn on a raised viewing deck east of it, looking west over it.
The vanilla machine instead has a 39x33 wall of redstone lamps that
mirrors the board (a body cell shows as a plus, the food as a small L in
the cell's corner). The console desk in front of you
has the start lever (ON = paused; flip it OFF to play), four stone buttons
in a D-pad (far = up, near = down, and right/left as you face the screen)
and, in the floor west of the desk, a lamp that lights when the snake
dies. One step takes about 30 seconds at 20 ticks per second; `/tick rate
60` makes it about 10 seconds. The screen lags the board by a few seconds.
Reload the world to play again; a plus machine (below) restarts instead:
flip the lever ON and OFF again and a fresh game starts from the initial
position, as often as you like.

How it works (all in this directory):

- `pixel.py`, `board.py` — 64 pixels of 16x16 blocks, seven levels high.
  Each pixel is an RS latch (body lamp) plus NOR taps off four row lines
  (set, check, erase, food) and three column lines (head X, tail X, food X)
  with a collision output on a per-column level-4 line.
- `matrix.py` — the select decoders are dust-ORs of repeater taps: a board
  column is low exactly when the register's bits match its index; rows the
  same, with the phase pulse merged in the same way.
- `tailmux.py` — the tail's direction: a 12-stage FIFO of the head's
  directions, tapped at stage L-1 by a matrix of NOR blocks.
- `snake3.py` — the machine: head/tail X-Y up/down counters, food
  registers fed from a 6-bit RNG counter, GROW = (head == food) as pure
  logic, a five-phase sequencer (A head step + wall check, B tail erase
  unless GROW and DIR <- REQ, C collision check + length++ if GROW, D head
  set, E tail step unless GROW / new food / FIFO shift), death latch that
  gates the board pulses. Control logic is placed on the fabric
  (`fabric.py`) with an explicit floor plan; the counters' T gates are
  placed next to their counter (a step lands in ~70 ticks). `SPACING` and
  `clock_n` come from traces (`timing3.py`, `play2.py play --rec`); every
  phase gap covers the slowest transport it waits for (DIR to the FIFO
  input, the FIFO to the tail gates, GROW to the erase gate).
- `screen.py` — the lamp wall: each pixel's body lamp and food block are
  tapped, carried north over the board on two levels (body 15, food 10)
  with per-pixel jogs at level 8, up torch ladders (one per signal, packed
  at pitch 2 in one plane per board column) and east along links to the
  wall; every adjacency rule the routing relies on is in the module
  docstring, and `check_nets.py`-style topology checks plus pattern tests
  were used to prove the 128 chains isolated.
- `deck.py` — the viewing deck and console; the buttons and lever are wired
  under the floor, down a staircase and across the machine at level 16 to
  the original button lanes (torch drops at the end), the death signal is
  tapped from the DEADFB lane and comes back up two torch ladders.
- `play2.py` — initialises the machine (pinned steady state) and plays it
  against `snakemodel.py` in the tick simulator (`--greedy` steers towards
  the food, `--rec STEP FILE` records every lane edge of one step); the
  head, board and death lamp are read 585 ticks after the A pulse, the
  tail, length and food 250 ticks into the next cycle. `timing3.py` prints
  per-cycle signal timing from a trace.
