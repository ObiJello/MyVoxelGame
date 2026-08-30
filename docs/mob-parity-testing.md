# Mob parity — in-game verification recipes

How to check, in a running world, that the spawning/AI systems match Minecraft's
numbers. Each recipe names the MC rule it exercises; the constants come from
`minecraft_code/decompiled_net/` (world version 4764).

Run `python3 tools/mob_audit.py --all` after any mob change — no gap count may
increase.

## Natural spawning

**Dark platform (the farm core).** Build a roofed platform (light 0 inside) at
least 25 blocks from where you stand and within 128. Monsters must appear in
packs of up to 4 of one type, never within 24 blocks of you (walk the ring:
spawns at 24.5 blocks, silence at 23.5), and never within 24 blocks of the
world spawn point.

**The cap.** Stand still with the full 17×17 chunk square loaded. Steady-state
hostile count must saturate at exactly 70 (`F3`-style counters read
`MobManager::CountForCategory`). Kill ten — the count refills. Mobs made
persistent by `/summon` must NOT consume cap (census skips persistence).

**Categories.** Oceans: squid in schools of 4–6 (WaterCreature cap 5), glow
squid only below y = 30 in darkness. Caves: bats (Ambient cap 15). Passive
animals only top up on the 400-tick pass — killing a cow does not refill it
within 20 seconds.

**Despawn rings.** Tag a hostile visually, then: beyond 128 blocks it vanishes
instantly; parked at ~40 blocks it goes on a 1-in-800-per-tick coin after 30
idle seconds (expect a ~40 s half-life); inside 32 blocks it never despawns and
its idle timer resets.

**Slime chunks.** Below y = 40, slimes spawn only in the seeded 1-in-10 chunk
grid (`seedSlimeChunk`, salt 987234911) regardless of light; swamp surface
spawns need y 50–70 at night, scaling with the moon.

## Combat / kill mechanics

**Fall damage.** `floor(fallDistance + 1e-6 − 3)`: a zombie (20 HP) dropped
22 blocks survives at half a heart; 23 blocks kills. Chickens never take fall
damage (flap damping). A chasing creeper walks off drops that leave it at
1 HP; its landing advances the fuse by 1.5 × distance.

**Skeleton archery.** Closes to 15 blocks, strafes (direction flips ~1/3 every
second), releases after a 1 s draw, cooldown 2 s (1 s on Hard). Arrows arc at
0.05/tick gravity, stick into blocks for 60 s, fall out if the block is
broken, and never hit the shooter. A stray arrow into a zombie starts a
skeleton-zombie fight (damage attributes to the shooter).

**Enderman.** Look at its head from beyond 4 blocks: it freezes and stares.
Break the gaze: it charges. Stare from inside 4 blocks: it teleports away.
Arrows never connect. It burns nothing in daylight — it teleports away
instead — and takes 1/tick in water or rain-free water contact.

**Slime split.** Kill a big slime: 2–4 mediums appear half a block up; mediums
split to smalls; smalls (harmless) drop slimeballs. Magma cubes hop a quarter
as often, higher, hurt at every size, and take no fall damage.

## Movement

Fish school behind a leader and flop when beached. Drowned chase into water.
Bees/parrots/allays fly with FLYING_SPEED (0.4 base) and hold altitude.
Spiders climb walls when their path fails and go passive above light 12
(magic value ≥ 0.5, not raw light 8).

## Missing assets (user action)

Copy from the Minecraft jar into `assets/textures/entity/`:
`camel/camel_husk.png`, `nautilus/nautilus.png`, `skeleton/parched.png`,
`copper_golem/copper_golem.png`.
