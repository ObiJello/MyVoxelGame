# Fluids

Water and lava are a port of Minecraft's fluid stack: `FluidState` /
`FlowingFluid` / `WaterFluid` / `LavaFluid` / `LiquidBlock` on the server,
`EntityFluidInteraction` on entities, and `FluidRenderer` on the client.
This note is the map of where each piece landed and what was deliberately
left out. Vanilla class and line references in the code are to
`minecraft_code_26.3-pre-2`.

## The model (common)

`src/common/world/fluid/FluidState.hpp`

- A `FluidState` is a **value computed from a `BlockState`**, never stored,
  exactly as `BlockState.getFluidState()` is a lookup. `FluidStateOf(state)`
  reads water/lava's `level` property (0 = source, 1..7 = flowing with
  amount `8 - level`, 8..15 = falling) and answers a water source for any
  waterlogged or always-water block (kelp, seagrass, bubble column).
- `FluidHeight` is MC `getHeight`: `amount / 9`, or 1.0 when the same fluid
  is directly above. `FluidHeightForCamera` adds the "source under a sturdy
  ceiling fills the cell" rule the eye test uses.
- `FluidFlow` is `FlowingFluid.getFlow`: the height gradient across the
  four horizontal neighbours (looking one cell down over an open edge), with
  the `-6 Y` term for a falling column against a solid face. It is the one
  input to both the mesher's flowing-sprite rotation and the entity current.
- The block tags the rules read (`#blocks_motion`, `#blocks_fluid_flow`,
  `#washed_away_by_fluids`, `#soul_fire_base_blocks`) come from the data pack
  through `DataTags`, resolved once per `BlockID` at registry init.

## Spreading (server)

`src/common/world/fluid/FlowingFluid.cpp` — `FlowingFluid.tick`, `spread`,
`getNewLiquid`, `getSpread`, `getSlopeDistance`, `canPassThroughWall`,
`isWaterHole`, `spreadTo`, and the `LiquidBlock` hooks, ported function for
function. Parameters: water ticks every 5, drop-off 1, slope search 4; lava
ticks every 30 (10 in the nether), drop-off 2 (1), slope search 2 (4), and a
rising lava cell is given four times the delay three times in four.

How it plugs into the engine:

- **Fluid ticks ride the block-tick queue**, keyed on `BlockID::Water` /
  `BlockID::Lava`. That is the identity vanilla's `LevelTicks<Fluid>` has —
  a waterlogged fence books a `water` tick on its own cell — and
  `World::ProcessBlockUpdates` dispatches those two ids on the cell's
  **fluid** state (`ServerLevel.tickFluid`'s `fluidState.is(type)` guard),
  not its block. The Anvil serializer writes them to `fluid_ticks`, named
  `minecraft:water` / `minecraft:flowing_water` by the cell's current state,
  and reads both lists back.
- `LiquidBlock.onPlace` / `neighborChanged` / `updateShape` book the tick;
  `BlockRegistry_RegisterFluids` installs them (after `InitBlockStates`,
  before the random-tick table is published, because lava random-ticks).
- The line every `SimpleWaterloggedBlock.updateShape` shares — schedule a
  water tick when waterlogged — is done **once**, in
  `World::ExecuteShapeUpdate`, for every block that holds water.
- `canPassThroughWall` / face culling use `ShapeOcclusion.hpp`
  (`Shapes.mergedFaceOccludes` / `blockOccludes` on box lists), so a fluid
  flows through exactly the gaps it is drawn through.
- A **container** (any waterloggable block) accepts only the *source* fluid
  (`canPlaceLiquid: type == Fluids.WATER`), so a stream runs over a fence dry
  and an infinite-source spring or a bucket waterlogs it. Placement follows
  the same rule: a block set down in flowing water is dry and displaces it.
- Lava/water: lava beside water becomes obsidian (source) or cobblestone
  (`LiquidBlock.shouldSpreadLiquid`); lava falling onto a water block makes
  stone (`LavaFluid.spreadTo`); lava on soul soil beside blue ice makes
  basalt. Fizz plays `block.lava.extinguish` through the sound seam.
- Lava's random tick starts fires next to `ignitedByLava` blocks
  (`LavaFluid.randomTick`), placing `fire` or `soul_fire` by the base block.
- Buckets: pickup only from a **source** (the clip stops on sources of either
  fluid), pouring writes a source with flag 11 and drops the displaced
  replaceable block, water evaporates in the nether.

Game rules `water_source_conversion` / `lava_source_conversion` gate
infinite-source formation and are now implemented.

## Entities

`Entity::UpdateInWaterStateAndDoFluidPushing` is `EntityFluidInteraction`:
one scan per tick over the overlapped cells fills a `FluidContact` snapshot
(per-fluid height above the feet, eye submersion, summed current), and
`IsInWater` / `IsInLava` / `IsEyeInWater` / `GetFluidHeight` read it. The
current push (`applyCurrentTo`, 0.014 water, 0.0023 / 0.007 lava, players
averaged and mobs normalised, the 0.0045 minimum nudge) goes through
`AddDeltaMovement` when `IsPushedByFluid()`. Lava contact ignites for 15 s
and hurts 4 (`MobDamageSource::Lava`), water puts fire out.

`LivingEntity::Travel` is `travelInWater` / `travelInLava` with the shallow
and deep lava branches, `getFluidFallingAdjustedMovement`'s -0.003 snap and
`jumpOutOfFluid`; the jump block is MC's fluid-height decision tree
(swim above `getFluidJumpThreshold`, real jump off the floor below it).

The **player** runs MC's own `travelInWater` / `travelInLava` in
`Physics.cpp`: each axis's per-tick map `v' = d·(v + a) − g` is evaluated at
fractional ticks (exact at every tick boundary), with `moveRelative(0.02)`,
the 0.8 / 0.9 / 0.5 drags, gravity/16 and gravity/4, `jumpInLiquid`,
`goDownInWater`, the ten-tick ground-jump delay in shallow fluid, the
current impulse, the sprint-swim cancel outside deep water, and
`Player.travel`'s look-pitch swim steering. `UpdateWaterState` is the same
scan as the entity one. The server's
`ServerPlayer::tick` rescans the body for lava (4 damage, 15 s fire, 1 fire
damage per 20 ticks) and water (extinguish). The player view refreshes its
snapshot in `TickCombatState` and is not pushed (its client is).

## Rendering

`src/client/renderer/mesh/FluidMeshBuilder.cpp` is `FluidRenderer`:

- corner heights from `calculateAverageHeight` over the 3x3;
- still sprite when the flow is zero, flowing sprite rotated by
  `atan2(flow) - 90°` otherwise; sides sample the flow sprite's top-left
  quarter cut to each corner's height;
- water's `water_overlay` sprite against glass, stained glass, ice, blue
  ice, slime, honey and leaves (and no back face there);
- `shouldRenderFace` / `isFaceOccludedByNeighbor` / `isFaceOccludedBySelf`
  on the shape-set occlusion helper, so a waterlogged slab's water has no
  face along the slab.
- water vertex alpha is 1.0; the sprite carries the translucency (MC).
- The still-fluid greedy merge is unchanged: a flat, still, full-cell top or
  bottom still tiles; anything sloped or rotated stays per block.

Underwater: `EnvironmentState::SetCameraFluid` swaps the fog for the
biome's `WATER_FOG_COLOR` (-8..96 blocks scaled by `getWaterVision`) or
lava's flat orange (0.25..1 block), and `HudRenderer` draws
`textures/misc/underwater.png` scrolled by the view angles at alpha 0.1.
`isEyeInWater` reaches shader packs as 1 (water) or 2 (lava).

## Resonant water (Aurelith's river)

The river Vesper that runs through Aurelith (`docs/the-hush.md`,
`docs/hush-lore.md`) is `resonant_water` (`BlockID::ResonantWater`): water
in every rule, different only in how it looks. The design is MC's own
**always-water block** — the bubble column's — rather than a second fluid:

- **Model.** No properties (it is always a source, so it has no `level`).
  `gen_waterlogged.py` lists it with bubble_column as unconditional water,
  so `BlockRegistry::ContainsWater` is true and `FluidStateOf` answers a
  WATER SOURCE. Everything that reads fluid state — swimming, drowning,
  the current, `isEyeInWater`, the camera height rule, heightmaps
  (`IsAlwaysWaterlogged`), waterlogging a block placed into it — is
  therefore water's, with no code of its own. `FluidType` stays two-valued:
  no fluid registry, tick queue or wire format changes.
- **It never changes itself.** `FlowingFluid::Tick` leaves a source alone
  and only spreads, so a river cell is never rewritten. A cell beside a hole
  spreads plain flowing water into it exactly as a kelp or bubble-column cell
  does (and two river cells round a hole make a plain water source there —
  infinite source formation), which is MC behaviour; the river itself keeps
  its block. Lava falling onto it fizzes and is not placed (only a plain
  water BLOCK turns to stone), lava beside it becomes obsidian as beside
  water. No generator schedules ticks for it, so a sealed channel never
  changes at all.
- **Block semantics.** Replaceable (`kReplaceableSlugs`) and collision-free
  like water; not a raycast target (`Raycast::IsBlockSolid`), not solid to
  `IsBlockSolid` (AI, navigation), hardness water's (unbreakable, 100).
  Placing a block into it replaces it (waterlogging the block when it can
  hold water, as water does). A **bucket** picks it up the way MC's bubble
  column is picked up (`BubbleColumnBlock.pickupBlock`): the cell becomes
  air and the bucket holds ordinary water — the glow stays in the river.
  Pouring water into it replaces it with plain water (it is replaceable).
- **Rendering.** `FluidMeshBuilder` draws it as water (same heights, flow
  and culling — resonant and plain water are one fluid, so the face between
  them is culled) with `block/resonant_water_still` / `_flow` (authored in
  colour, violet with aurora-cyan highlights, like lava's sprites — falling
  back to water's if missing), a white tint instead of the biome water
  colour, its own flowing sprite behind glass instead of the greyscale
  `water_overlay`, and every quad **emissive** (the alpha-byte marker —
  `engineering-notes.md`, "Emissive quads"), so the river glows at night.
  Greedy plates never mix the two waters: the merge key is sprite + colour.
  The block model is particle-only and the mesher skips it like water's
  (bubble column's INVISIBLE render shape).
- **Underwater.** `PlatformMain` checks the eye's cell; in resonant water
  `EnvironmentState::SetCameraFluid(..., resonantWater)` swaps the water fog
  colour for a luminous violet (#5E46BC), applied after the Hush's teal
  refinement so it wins there too; MC's distance curve is unchanged.
- **Motes.** `AurelithBlocks.cpp` gives it an animateTick: now and then a
  mote lifts off a surface cell.

## Not ported, on purpose

- **Light.** There is no light engine, so lava emits nothing and fluid
  vertices carry no lightmap; the fluid faces take the directional shade
  only, and the underwater overlay's brightness is the sky's day/night value.
- **Sounds and particles.** `doWaterSplashEffect`, ambient underwater and
  lava sounds, `animateTick`'s underwater/lava-pop/drip particles, and the
  eight smoke puffs of level event 1501 have no sound system or particle
  types to land in.
- **Bubble columns** and `dismountsUnderwater`: no bubble column behaviour.
- `WATER_MOVEMENT_EFFICIENCY` (depth strider) and `DOLPHINS_GRACE`: neither
  the attribute nor the effect exists yet.
- `canSpreadFireAround`'s player-radius gate for lava fire: `World` has no
  player list; random ticks already run only in simulating chunks.
- Player drowning / air HUD: unchanged by this work (the bubble bar is still
  a placeholder).
- A burning player has no client-side fire overlay; fire ticks are
  server-only state.
