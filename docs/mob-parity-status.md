# Mob parity — remaining gaps and what each waits on

The audit (`python3 tools/mob_audit.py --all`) is the source of truth; this
document maps its remaining findings to the MISSING SYSTEM each one waits on,
so a gap is a deliberate dependency rather than an oversight. When a system
below lands, search this file for it and the affected mobs fall out.

State as of 2026-08-23 (the deep-dive verification wave): **71 of 89 mobs
match on everything the audit checks, 0 BUG-class gaps** (2026-08-22 ended at
69 + 1 BUG-class). Every remaining audit line traces to villages/raids/trading
(structure-side systems: villager, wandering trader, the raid goals).

## 2026-08-23 wave — the exactness verification pass

Four adversarial audits (render, farm-technicals, AI core, animation wiring)
were run against the decompile and everything they surfaced was fixed or
documented. The headline items, each verified against its MC source:

- **Mesh scales carried on PartPose, not baked into geometry.** MC computes
  every cube's UV layout from the UNSCALED pixel size, so baking
  MeshTransformer.scaling into cube sizes remapped every scaled mob's texels
  by its factor — the smeared wither skeleton (1.2), cave spider (0.7),
  elder guardian (2.35), ghast (4.5), cat/horse/husk/villager-family/giant/
  polar bear textures. GenPart/PartPose now carry MC's pose scale (root
  part + the 24.016·(1−f) re-anchor); cube data is back to the literal
  decompile values. Also fixes setupAnim position-writes into pre-scaled
  meshes (horse family).
- **MC-exact babies** — generated `<slug>_baby` meshes from LayerDefinitions'
  `*_BABY` rows via BabyModelTransform (big head, half body, per-axis
  CubeDeformation for the equine legs), replacing the uniform 0.5 shrink;
  37 mobs baby-exact, hand-written five included. Bonus: the adult rabbit's
  dropped ADULT_TRANSFORMER scaling(0.6) — rabbits rendered 67% too large.
  Skipped: nautilus baby (texture not in assets).
- **The melee swing now exists client-side** — Swing() broadcasts entity
  event 103 (the Animate-packet stand-in), the renderer lerps getAttackAnim,
  and the restart threshold is duration/2. Every melee mob's whack animates
  for the first time.
- **AI core (every mob):** input decay moved BEFORE serverAiStep (all AI mobs
  ran at 98% speed), TargetingConditions rewritten to MC's real
  combat/non-combat split (was backwards: stares through walls, Peaceful
  hunts), lava travel constants (0.5/−g/4, was water's 0.8/−g/16),
  jumpOutOfFluid (bank-edge escape), pushEntities + Entity.push with the
  24-cramming valve (mobs no longer stack; cramming farms work), soul
  sand/honey 0.4 speed factor, friction sampled at getOnPos(0.500001),
  RandomPos direction = MC's lerp(√u)·√2 with box rejection + the home
  bias/rejection chain, Brain erases empty entity-list memories,
  noActionTime zeroed on hit, MC's knockback jitter loop.
- **Navigation/controls (18 items):** ground mobs no longer shortcut
  waypoints (CanMoveDirectly is not a GroundPathNavigation thing), stuck
  detection matches (timer not zeroed per node; isStuck clears), createPath
  guards + surface projection + stored-target recompute, strafe walkability
  redirect, Manhattan path distance, PathFinder best-node fix, rails
  unpassable to non-rail mobs, isPassenger gates, WallClimber cluster,
  fence-gate/wall auto-jump suppression removed.
- **Goals/sensing (16 items):** PanicGoal fires on attackerless damage
  (burning mobs run to water) and picks the CLOSEST water, HurtBy alert box
  + the zombie family alert matrix (a zombie wakes husks/drowned, skips
  zombified piglins), tempt = nearest HOLDER, spherical breed range, LOS
  128-block cap, lazy per-query visibility for brain sensors, HurtBySensor
  to MC's shape, enderman +0.15 attack speed + stare scan fix, creeper fuse
  gated on IsAlive (no posthumous explosions) + explosion exposure/knockback
  (shared with the wither burst), zombie reinforcements spawn getType() with
  distinct caller/callee charge modifiers + on-fire melee transfer + the
  turtle-egg hunt.
- **Drops/XP (the farm half):** mob-kill XP awards (100-tick player credit
  window; real ExperienceOrb entities since 2026-08 — value splitting,
  40-group merging, player pull + takeXpDelay pickup, billboard renderer,
  SetExperienceS2C HUD sync; per-mob values fixed at the generator incl.
  blaze 10/ravager 20/slime=size/baby zombie 12; breeding + turtle/panda
  awards), loot pools
  evaluate MC's weighted-roll algorithm (guardian/witch/polar bear rates
  exact), killed_by_player + random_chance + slime-size conditions supported
  (slimeball, magma cream, blaze rod, phantom membrane, shulker shell,
  drowned copper, ZP gold now drop), negative-minimum counts clamp at sample
  time (wither skeleton coal 1/3), cluster sizes (wolf 8, horse 6, pillager
  1, happy ghast 1, tropical fish loner roll), loot drops at the DEATH
  moment (chunk-edge kills no longer lose drops), hoglin/camel-husk despawn
  + allay persistence per source.
- **Render layers:** eyes (spider/cave spider, enderman, phantom, creaking
  flicker, warden bioluminescent/pulsating/heart — tendrils wait on
  vibrations), drowned outer layer (+baby), stray/bogged clothing, mooshroom
  mushrooms (red — no variant sync), iron golem cracks + offered poppy,
  sheep wool undercoat, pufferfish mid/big inflation models + bob, fire on
  burning mobs (FlameFeatureRenderer, mcmeta frame order), drowned swim
  tilt (client swimAmount ramp), wither 2.0 scale + 1.5→2.0 spawn ramp +
  the 5-tick blue flicker (tick count rides the anim byte at exactly the
  flicker's grain), 180° death flip for spiders/silverfish/endermites,
  red-beats-white overlay priority, dragon/creaking red-overlay overrides,
  enderman creepy jitter, magma cube without the slime's 0.999, bee wings
  (isOnGround was never set), hurt flinch (walk speed 1.5), dead mobs relax
  to rest pose, flying animals count vertical walk distance, ocelot trust
  particles.
- **Copper golem chest transport** — see the updated determination below;
  the audit's last BUG-class item, closed.
- **Documented as deviations, not faked:** blob shadows, per-mob world
  lighting (see MobRenderer.hpp's NOTE), warden tendrils, mooshroom variant,
  the GetSpecialMultiplier steady-state choice (EntityLevel.hpp), the
  fluid-jump threshold (sub-block fluid heights cannot exist here, so MC's
  ≤0.4-deep branch is unrepresentable — current behavior matches every
  representable state).

## Item-render layers (2026-08-22, closing pass)

MC's ItemInHandLayer + collar layers, landed:

- **Held items** through a generalized pipeline: `GeneratedModel::
  RightHandMatrix` composes the root→right_arm chain of ANY generated mesh
  (with the SkeletonModel family's one-pixel arm shove), and the renderer
  applies each item JSON's display.thirdperson_righthand verbatim. In hand
  now: bows (skeleton, stray, bogged, parched, illusioner), the wither
  skeleton's stone sword (+ ITEM arm pose), the vindicator's iron axe
  (shown while ATTACKING, hidden while CROSSED — MC's visible behavior),
  the pillager's crossbow (uncharged sprite; charge states wait on a mob
  item-use clock), and the drowned's trident as the real MODEL via the
  trident_in_hand display block (+ THROW_TRIDENT arm pose while
  aggressive, ITEM otherwise). VERIFY IN GAME: the trident's in-hand
  orientation was derived, not observed.
- **Armed animation branches went live**: `hasMainHandItem` is a real
  render input (the vindicator's ATTACKING pose now runs MC's
  swingWeaponDown, not the empty-hand zombie arms), IllagerRenderState's
  `attackAnim` maps to the swing clock, and constant-argument ModelPart
  ternaries resolve statically — that one unlock took the compiled
  setupAnim coverage from 78% to **88%** (crossbow-hold/charge arm poses
  across every humanoid).
- **Collars**: tamed wolves and cats re-render through the collar sheet
  tinted DyeColor.RED (no collar dyeing yet — every collar is the
  default), exactly MC's renderColoredCutoutModel.
- **Wolf**: tame/angry texture swap (wolf_tame/wolf_angry; the biome wolf
  variant sets sit on disk for a future variant roll), the real
  getTailAngle (tame health wag included), and **BegGoal** — the
  interested head-tilt rides anim-byte bit 2 into the model's
  headRollAngle.
- Still open in this layer: beg-worthy held-item RENDER on the player
  side is inherent (players render items already); fox/allay mouth items
  (need item pickup), witch drinking potion visual, piglin/zombie rare
  equipment rolls (need the equipment system).

## Particle system (2026-08-22) — LANDED, with scope

`Render::MobParticleSystem` (`src/client/renderer/particle/`) — MC's client
particle engine reduced to the types mob code spawns, physics constants
verbatim from `$MC/client/particle/` (Particle/SingleQuadParticle base:
20 Hz tick, 0.04·gravity, per-type friction, per-particle block collision
via the entity mover's CollideAxis; HeartParticle, Smoke/LargeSmoke
(BaseAshSmokeParticle), ExplodeParticle, HugeExplosionParticle + the
EXPLOSION_EMITTER seed, SpellParticle). Sprites are MC's own
(`assets/textures/particle/`), sheet frame order per the particle JSONs
(generic/spell play DOWN, explosion plays up). Seam: `EntityLevel::
AddParticle`/`AddColorParticle` (client bridge queues → drained per frame in
PlatformMain; server bridge no-ops, exactly MC's Level.addParticle split).

**What emits now** (each ported verbatim from its Java source):
- Taming: 7 hearts / 7 smoke on entity events 7/6
  (`TamableAnimal::SpawnTamingParticles`) — wolf, cat, parrot.
- Breeding: 7-heart burst on event 18 (feed AND breed completion — the
  finalizeSpawnChildFromBreeding broadcast was added), plus MC's 1-heart-
  per-10-ticks courtship cadence. DEVIATION: the cadence travels as custom
  event byte 102 (MC spawns it client-locally off the unsynced inLove).
- Death/despawn poof: event 60 → `LivingEntity::MakePoofParticles` (20 POOF).
- Wither: head smoke ×3/tick, powered aura (ENTITY_EFFECT 0.7/0.7/0.5,
  1-in-4), spawn charge-up column (0.7/0.7/0.9 ×3) — Wither::AiStep client
  tail; spawn-completion explosion visual.
- Explosions: MC sends ClientboundExplodePacket; this port has no explosion
  packet, so sources broadcast custom event bytes 100 (EXPLOSION_EMITTER —
  creeper r=3, wither burst r=7) / 101 (single EXPLOSION — wither skull,
  ghast fireball), documented in EntityLevel.hpp. The client handler ports
  handleExplosion + ClientExplosionTracker (cbrt radius spread, air check,
  50/50 poof/smoke debris) with blockCount approximated as radius³ (no
  blocks are destroyed here). The tracker flush was reordered server-side
  (ServerEntityTracker::FlushEntityEvents before removals) so same-tick
  discard events survive.
- Witch: event 15 ambient WITCH burst (10–44 purple spell particles).

**Still not emitting** (each waits on its system / texture):
squid ink (needs squid_ink sprite + its own SquidInkParticle tick),
sniffer digging, AreaEffectCloud's ambient swirl + dragon-breath cloud fill,
potion swirls on effected mobs (needs effect→client sync), witch drinking
smoke (MC has none — the drink visual is the held potion item), zombified
piglin aggro (MC spawns no particles for it — verified), portal/end-rod
ambient, drip/splash/bubble environmental particles. Render deviations
(single alpha-blended pass, no per-particle world light, no distance sort)
are listed in MobParticleSystem.hpp. The ENTITY_EFFECT sheet uses spell_0..7
as a stand-in for MC's effect_0..7 (not in the repo's sprite set).

## Brain-driven mobs (2026-08-22 wave two)

Every mob MC drives with a Brain now runs the ported Brain except the
villager. Wave one: frog, tadpole, camel (+husk), goat, hoglin, warden,
breeze, creaking, sniffer. Wave two added, each `<Mob>Ai.{hpp,cpp}` in
`ai/brain/` with skips commented at their MC slots:

- **Zoglin** — MC's idle/fight brain verbatim (StartAttacking over the
  visible set minus zoglins and creepers, the 40/15 adult/baby melee
  interval, the hurt→200-tick ATTACK_TARGET retaliation with the
  much-further-away check), plus the 20% baby FinalizeSpawn and baby
  attack-damage drop.
- **Axolotl** (promoted to a class) — PLAY_DEAD as a real activity
  (ValidatePlayDead in core counting the memory down, the hurt-roll arming
  it, self-regen on entry, the synched playing-dead flag on the anim byte),
  hunting (AxolotlAttackablesSensor: hostiles drowned/guardian/elder
  guardian, hunt targets the fish/squid/tadpole tag, gated on the 2400-tick
  hunting cooldown armed on leaving FIGHT), the kill reward (regen 100→2400
  stacking + mining-fatigue strip for a helping player within 20),
  amphibious idle (swim/stroll gate with the water-line look predicate,
  TryFindWater), the 6000-tick land dry-out on the air machinery, the
  five-variant spawn packs (two common variants per pack, third member
  onward a baby) and breeding variant inheritance (1/1200 blue). The
  play-dead-frozen move/look controls are MC's AxolotlMoveControl/
  LookControl. Skipped: bucketing (items), rain in isInWaterOrRain.
- **Piglin** — idle/fight/celebrate/avoid/ride on MC's activity order
  (ADMIRE_ITEM stays unregistered — items). Ported: hoglin hunting with the
  pack broadcast + shared 30-120 s hunt cooldown, celebration (dance roll
  seeded on game time, CELEBRATE_LOCATION walk, dancing on the anim byte
  for the renderer's arm pose), retreat from zombified (zombified piglin +
  zoglin) and from hoglins when outnumbered (the visible-count bookkeeping),
  the soul-block repellent scan (soul torch/wall torch/lantern/campfire/
  fire, 8x4 box) with SetWalkTargetAwayFrom.pos, the baby's nemesis flight
  and baby-hoglin riding games (Mount/Dismount with MC's stack-3 rule via
  StartRiding), the jealous stare at players holding gold (PIGLIN_LOVED
  flattened over held-item ids), piglin-to-piglin socialising (InteractWith),
  anger with pack broadcast, and the overworld zombification clock
  (300 ticks → ZombifiedPiglin, this engine's one dimension being the
  overworld). Skipped at their sites: admiring/bartering/pickup (items),
  crossbow + spear combat (weapons), gold-armor truce (equipment), doors,
  sounds, UNIVERSAL_ANGER (game rule defaults off).
- **PiglinBrute** — the simpler always-hostile brain: ANGRY_AT → player →
  nemesis targeting, HOME memory patrols (StrollToPoi/StrollAroundPoi),
  piglin/brute socialising, retaliation via the shared MaybeRetaliate, the
  same zombification clock. Golden axe skipped (equipment).
- **Nautilus + ZombieNautilus** (promoted, `Fish.{hpp,cpp}`) — swim-wander,
  temptation (fish foods), nautilus breeding, the ChargeAttack ram (velocity
  lock, 12-block charge cap, line-of-sight, attack damage + speed-scaled
  knockback, 80-tick charge cooldown), the 2400-3600-tick unprovoked-attack
  cooldown with the 50% coin flip over swimming pufferfish, anger-on-hurt
  (400 ticks), poison immunity, the 300-tick dry-out air rule. Skipped:
  taming/riding/shell inventory (items+riding input), the zombie's biome
  texture variant (one texture shipped).
- **Allay** (promoted) — the honest core: float, panic 2.5, and the flying
  idle wander (RandomStroll::Fly = AirAndWaterRandomPos ahead of the view
  vector) with player glances. The item-courier loop, jukebox dance and
  duplication are named-skipped (items / jukebox events / vibrations).
- **HappyGhast** — MC's real split ported: the ADULT stays goal-driven
  (float + def set), the BABY ghastling runs HappyGhastAi (tempt-follow,
  trailing the nearest player and any followable adult via
  AdultSensorAnyType, flying wander, empty PANIC activity over the core
  AnimalPanic); the class swaps setups at the age boundary by polling
  (no ageBoundaryReached hook).
- **Armadillo** — the goal-approximated roll-up moved onto the real brain:
  the MobSensor scare detector (scan 5, isScaredBy over the raw
  nearest-living list, canStayRolledUp erase), ArmadilloBallUp alone in the
  PANIC activity (peek event 64 on the 100-400-tick jitter, the
  Scared↔Unrolling flicker against the 80-tick danger TTL read via
  getTimeUntilExpiry), ArmadilloPanic (roll out first), the scared-frozen
  MoveToTargetSink, the rolling-out core one-shot, and MC's idle set
  (RandomLookAround 150-250 included).

**Villager stays goal-approximated** — its brain is the schedule/POI/
gossip/trade stack (villages, job sites, beds, economy), none of which
exists; it keeps the generic PathfinderMob set and is the audit's one
remaining `brain` finding, deliberately.

## Systems that gate the remaining gaps

**Taming / ownership** — LANDED 2026-08-22, on the player→mob interaction
system (MC `mobInteract`). The dispatch was already in
`IntegratedServer::HandleInteract` (entity-first, item hook second, creative
count restore, inventory push on consume); this wave filled in the entity
side. Core: `src/common/entity/TamableAnimal.{hpp,cpp}` — a mixin base (the
NeutralMob pattern, because Wolf sits on GenericAnimal) carrying tame flag +
owner + orderedToSit/sitting-pose, feed-heal, the ±3-ring teleport-to-owner,
and the wire byte (bit 0 sitting pose, bit 1 tame, on each implementer's anim
state byte; the renderer's TamableAnimal block drives `state.isSitting`, which
the cat/wolf/parrot setupAnim programs already keyed on). Owner is the player
ENTITY id (= connection id) — no UUID persistence, commented in the header.
Shared goals in `ai/goals/TamableGoals.{hpp,cpp}`: SitWhenOrderedToGoal,
FollowOwnerGoal (start/stop distances + the 12-block teleport, water malus
zeroed), TamableAnimalPanicGoal, OwnerHurtByTargetGoal / OwnerHurtTargetGoal
(a lastHurtMob timestamp was added to LivingEntity for the latter).
`GoalSelector::RemoveGoal` landed for MC's reassess pattern. Per mob:

- *Animal base feeding* (`Animal::MobInteract`): food ages a baby up 10%
  (ageUp forced — forcedAge quirk ported verbatim) and courts an adult
  (SetInLove). Covers chicken/rabbit/turtle/fox/pig/cow/sheep and every
  generated animal via their isFood lists.
- *Wolf*: bone → 1/3 tame, tame health 40, sit/stand toggle, feed-heal (2×),
  full MC target table incl. the owner-defence pair and the
  NonTameRandomTargetGoal prey hunts (sheep/rabbit/fox + beached baby
  turtles), wantsToAttack rules, tame-gated breeding with pup inheritance.
  Skips at their sites: BegGoal (held-item render), WolfAvoidEntityGoal
  (llama strength), dye collar + tame texture (per-type texture table),
  wolf armor.
- *Cat*: cod/salmon → 1/3 tame (sits on success), reassessTameGoals swaps
  the avoid-players goal out, feed-heal, sit toggle, tame-gated canMate.
  Still bed-gated: CatRelaxOnOwnerGoal / CatLieOnBedGoal / CatSitOnBlockGoal.
- *Ocelot*: fish inside 3 blocks while the tempt goal runs → 1/3 trusting
  (server-side flag; nothing rendered keys on it), reassessTrustingGoals,
  trust-waived tempt scare, trust-gated despawn.
- *Parrot*: seeds → 1/10 tame, sit toggle when grounded, FollowOwner may
  perch on leaves (canFlyToOwner), cookie = 900-tick poison + kill.
  LandOnOwnersShoulderGoal still skipped (player render side).
- *Panda*: bamboo feeding (the block item) — ages cubs, courts adults, sits
  a fed adult down (TryToSit), and gotBamboo now stands the grudge goal
  down; the hold-and-chew mouth-item half stays skipped.
- *Mooshroom* (new class): shears → ConvertTo(Cow) + 5 red mushrooms as
  singles; bowl→stew and the brown variant skipped.
- *Horse family*: the full handleEating heal/ageUp/temper table (golden
  apple/carrot love for a tamed one included), makeMad rear on a non-food
  click; temper is stored and ready but taming COMPLETION waits on player
  riding (doPlayerRide / tameWithName — commented). SkeletonHorse ignores
  clicks untamed, per source.

Still gated elsewhere: fox trust (rides the mob item layer), allay (item
following), horse riding itself.

**Status effects** — LANDED 2026-08-22, server-side. Core:
`src/common/entity/effect/MobEffects.{hpp,cpp}` (MobEffectInstance with MC's
hiddenEffect upgrade chain, tickServer cadences, attribute templates) held on
`LivingEntity` (AddEffect/RemoveEffect/HasEffect/GetEffect, tickEffects at the
end of BaseTick, undead = #minecraft:undead tag for both inverted-heal-and-harm
and poison/regen immunity). Hooks: FIRE_RESISTANCE in Hurt, magic/wither damage
bypasses armor, RESISTANCE in getDamageAfterMagicAbsorb, LEVITATION +
SLOW_FALLING in Travel (and the squid's beached branch), JUMP_BOOST in
getJumpPower + SAFE_FALL_DISTANCE, HASTE/MINING_FATIGUE in the swing duration,
HUNGER/SATURATION forwarded to ServerPlayer's FoodData through the player
view. What each mob gained:

- *Cave spider* (new class): poison bite 7 s/15 s on Normal/Hard; MC's
  finalizeSpawn override (no jockey/pack rolls); MAX_HEALTH 12.
- *Spider*: the HARD pack-effect roll now APPLIES the rolled permanent
  speed/strength/regen/invisibility (the RNG draws were already in place).
- *Bee*: sting poison 10 s/18 s, hasStung (bit 1 of the anim byte, next to
  rolling), no re-attack after stinging, and the 1200-tick sting-death
  countdown.
- *Pufferfish*: full puff state machine (PufferfishPuffGoal + inflate/deflate
  in tick), puff state on the anim byte, touch = (1 + state) damage + poison
  60·state ticks for mobs AND players (one sweep covers MC's aiStep loop and
  playerTouch).
- *Elder guardian*: the Mining Fatigue III aura (1200-tick cadence staggered
  by id, 50 blocks, 6000 ticks, survival players only, MC's refresh filter).
- *Witch*: real potion selection on throw (slowness/poison/weakness/harming by
  target state), splash applies effects with MC's distance scaling, and the
  self-drink machinery (water breathing/fire resistance/healing/swiftness on
  MC's triggers, 32-tick drink with the −0.25 speed modifier) plus the 85%
  magic-damage resistance and self-splash immunity.
- *Stray* (new class over Skeleton): SLOWNESS 600-tick arrows via the
  getArrow hook; freeze immunity is a comment (no powder snow).
- *Bogged* (new class over Skeleton): POISON 100-tick arrows, MAX_HEALTH 16,
  the slower 50/70 bow intervals — and it actually shoots now (was a generic
  melee monster).
- *Wither skeleton* (new class): WITHER 200 ticks on melee hit + fire
  immunity.
- *Wither skull*: WITHER II 10 s/40 s on Normal/Hard. *Shulker bullet*:
  LEVITATION 200 ticks. *Husk*: HUNGER 140·(int)effectiveDifficulty on hit
  (base regional difficulty — no inhabited-time scaling exists).

Honest follow-ups, each commented at its site: **client sync + particles** —
effects live only on the server's entity; no HUD icons, no swirls, no
invisibility/glowing rendering, and LEVITATION on a PLAYER ticks without
moving them (player motion is client-authoritative until an effect sync
exists). WATER_BREATHING is stored but idle until the air-supply system.
DragonFireball's lingering cloud LANDED with the dragon-AI wave (the
AreaEffectCloud entity — see "2026-08-22 wave three" below). Item-side
effects (potions drunk by players,
suspicious stew) are out of scope; ServerPlayer::addEffect's stub now points
at the view. Evoker fangs / turtle-master still wait on their mobs' own
systems.

**Anger system (NeutralMob)** — LANDED 2026-08-22, server-side. Core:
`src/common/entity/NeutralMob.{hpp,cpp}` — a mixin base carrying MC's
anger_end_time shape (absolute game-time deadline, -1 calm) plus
updatePersistentAnger / isAngryAt / stopBeingAngry / the
forgetCurrentTargetAndRefreshUniversalAnger pair, mixed into MC's exact
implementer list: **zombified piglin** (herd alert every 80..120 ticks within
follow range with LOS, +0.05 angry speed modifier, isAngryAt-gated player
targeting), **enderman** (isAngryAt feeds the stare-aggro selector),
**wolf** (promoted to a class: neutral-until-provoked player hunt replacing
the def's unconditional one, pack alert, LeapAtTarget + MeleeAttack it never
had, angry state mapped onto the wire's aggressive bit for the red-eye
texture/tail), **bee** (real BeeHurtByOtherGoal / BeeBecomeAngryTargetGoal /
BeeAttackGoal in BeeGoals.{hpp,cpp} replacing the stand-ins;
updatePersistentAnger(false); sting → stopBeingAngry), **polar bear** and
**iron golem** (isAngryAt-gated player hunts). Every implementer samples
PERSISTENT_ANGER_TIME = rangeOfSeconds(20, 39) → 400..780 ticks, per its own
source. `ResetUniversalAngerTargetGoal` (TargetGoals) is ported whole and
registered at MC's priorities on all six; its UNIVERSAL_ANGER game-rule gate
is a `false` constant (MC's default world) so it never fires, exactly as in
vanilla. Honest skips at their sites: the anger NBT halves (no mob
persistence), the angry/ambient sound splits (no sound system), goat is NOT
neutral in MC and got nothing.

**Projectiles beyond arrows** — small/large fireballs, wither skulls, shulker
bullets, llama spit, snowballs, tridents. Each needs an EntityTypeId append +
a renderer. Gates: blaze, ghast, wither, shulker, llama, snow golem, drowned
tridents, evoker fangs (ground hazard).

**Riding / passengers** — the Entity-level machinery LANDED 2026-08-22:
passengers/vehicle graph on `Game::Entity` (StartRiding/StopRiding/
EjectPassengers/RideTick/PositionRider, MC attachment-point table in
Entity.cpp), server tick order (MobManager::TickPassengerChain), tracker
passenger branch (no position packets for riders, forced PositionSync on
dismount), wire sync (`vehicleId` appended to AddEntityS2C/SetEntityDataS2C),
and client-side seating (ClientMobManager pass 0/1/2). A jockey is now
`rider->StartRiding(*vehicle, /*force=*/true)` after both are constructed.
Still gated on later waves: PLAYER mount control (`GetControllingPassenger`
returns null, no travelRidden/steering), the renderer's riding pose (a seated
rider's legs still animate), dynamic seats (strider bob, camel sitting), and
the per-mob wiring itself: chicken jockeys (the RNG draws already happen),
spider jockeys (the skeleton spawns beside instead of mounted), horse family
riding, camel/happy ghast mounting, ravager riders.

**Villages / POI / raids** — beds, job sites, bells, raid state machine.
Gates: villager profession AI (currently wanders/looks only), iron golem
patrolling, evoker/pillager/vindicator/illusioner raid goals, cat spawning on
beds, zombie MoveThroughVillageGoal, witch raid participation. (Zombie and
vindicator DOOR BREAKING landed in wave three — see below; only the
village-pathing halves still wait here.)

**Block interactions not yet modelled** — sniffer digging produces seeds
(needs item drops from digging, agent-ported behaviour handles the animation),
fox berry picking, turtle egg laying (TurtleEggBlock exists; laying flow does
not). Bee pollination + crop growth and the rabbit's carrot raid LANDED in
wave three (see below).

**Conversion system** — LANDED 2026-08-22 for the in-water pair. Core:
`Mob::ConvertTo` / `CopyConversionState` (Mob.{hpp,cpp}) — MC
ConversionType.SINGLE + convertCommon reduced to what this port tracks
(position/rotations/velocity/fall/hurtTime/onGround, passenger + vehicle
hand-off, active effects, left hand, NoAi, persistence, canPickUpLoot, fire;
NOT health — MC converts at full health). Zombie carries MC's clock
(`Zombie::Tick`): eyes underwater 600 straight ticks → 300-tick conversion →
**zombie→drowned**, **husk→zombie** (Husk.doUnderWaterConversion override),
with baby/canBreakDoors carried and MC's afterConversion handleAttributes
re-roll; drowned/zombified piglin/zombie villager return convertsInWater
false, per source. **Mooshroom→cow** landed with the interaction wave
(Mooshroom::Shear — shears convert and pop five mushrooms). Still gated,
each skip-commented at its site:
**skeleton→stray** (no powder snow — Skeleton class comment),
**zombie villager curing** (golden-apple interaction — ZombieVillager
comment), **pig→zombified piglin** (no lightning strikes — Pig comment), the
DATA_DROWNED_CONVERSION shaking visual (no renderer hook) and the 1040/1041
conversion sounds.

**Boss machinery** — the ender dragon's PHASES AND FLIGHT landed in wave
three (see below); still gated: the boss bar (dragon + wither), the End
dimension layer (crystals + healing, the podium/egg, the perch phase cycle),
and the dragon's 200-tick death cinematic. (The elder guardian's mining
fatigue landed with status effects.)

**Home/restriction positions** (`restrictTo`) — MoveTowardsRestrictionGoal for
guardian/elder guardian/blaze, SeekShelterGoal, patrol leaders.

**Air-supply / drowning** — LANDED 2026-08-22, server-side. Core: air on
Entity (300 ticks, MC TOTAL_AIR_SUPPLY; no bubble-bar HUD so no sync),
`Entity::IsEyeInWater`, and MC LivingEntity.baseTick's water block in
`LivingEntity::HandleUnderwaterAir` — eyes underwater and unable to breathe
there → -1/tick (WATER_BREATHING respected; RESPIRATION/conduits absent),
2.0 drown damage every time air hits -20; +4/tick recovery out of water.
`CanBreatheUnderwater` reads the CAN_BREATHE_UNDER_WATER tag
(CanBreatheUnderWaterEntityType next to the undead tag): undead + fish +
squids + guardian family + turtle + frog + tadpole + axolotl + copper golem
+ nautilus. The INVERSE (MC WaterAnimal.handleAirSupply,
`HandleWaterAnimalAirSupply` in Fish.cpp) beaches-kills fish, squid and
tadpole: -1/tick out of water, 2.0 suffocation at -20, pinned at 300 in
water. Dolphin: 4800-tick lungs, drowns underwater, instant refill at the
surface (increaseAirSupply → max), the real BreathAirGoal (surfaces below
140 air, MC's find-air scan + self-propelled rise) replacing the inert stub,
air pinned at max under NoAi; its moistness clock was already in. Iron golem
never loses air (decreaseAirSupply override). Bee keeps its own faster rule:
20 ticks submerged then 1.0 drown/tick (customServerAiStep). Turtle breathes
underwater via the tag and needs no override, per Turtle.java. Axolotl's
6000-tick land dry-out LANDED with its class promotion (brain wave two);
the nautili run the same inverted rule at 300 ticks.

## Models & animations (2026-08-22 exactness pass)

State of the render side after the compiler/mesh overhaul:

- **Meshes: exact for all 81 generated models** — an auditor script compared
  every part a model class's constructor `getChild`s against the generated
  part list: zero missing. This pass ADDED previously-missing geometry: ghast
  and happy ghast tentacles (seeded `Random(1660)` lengths replayed
  bit-exactly), silverfish spine layers, the real piglin head (snout, tusks,
  ears) for the piglin family, and fixed collateral constants (blaze rod
  ring positions, squid tentacle ring poses, guardian spike poses,
  humanoid-family arm/leg y-offsets that folded to zero).
- **setupAnim: 4450 of 5707 statements compile (78%)** — and everything still
  skipped falls into exactly four buckets, none of which is a visible gap for
  a mob doing what our port lets it do:
  1. *Dead branches*: crossbow/spear/spyglass/use-item/dancing poses no mob
     can enter (no item system). Their guards compile; the branch bodies are
     unreachable at runtime, same as in MC for an empty-handed mob.
  2. *Keyframe-clip statements* (`.apply` / `.applyWalk`): played by the
     KeyframeAnimation clip system instead, including per-model applyWalk
     speed/scale factors (camel, sniffer, armadillo, creaking, copper golem,
     nautilus).
  3. *The ender dragon*: hand-written DragonModel replaces the program
     wholesale (kinematic neck/tail chains over the REAL flight history as
     of wave three; the constant-history hover pose remains as the
     no-history fallback).
  4. *Behavior-gated inputs that stay at their truthful defaults* until the
     behavior exists: guardian eye-tracking (lookAtPosition), wither side
     heads (yHeadRots), fox faceplant leg wiggle, enderman carried-block arm
     pose (waits on carried-block client sync).
- **Render-state inputs**: ~45 fields added (EntityRenderState), populated
  from real entity state where the game tracks it — squid tentacleAngle
  (entity event 19 wired), guardian tail/spikes (client-side aiStep, exact),
  parrot flap + pose, phantom flap clock (id·3+age), equine stand/eat
  defaults, wolf angry tail, illager/piglin arm poses (ordinals verified
  against MC enum declarations), SwingAnimationType defaults WHACK — the
  empty-hand swing. Skeleton family (stray/bogged/parched) now draws the
  bow pose when aggressive; only the plain skeleton renders the bow item
  (held-item transform needs RightHandMatrix on GeneratedModel).

## Honest approximations currently in place (documented at each site)

- Enderman: carried block is server-only (the wire's variant byte carries one
  byte; a BlockID needs sixteen) — a carrying enderman renders empty-handed.
- Slime: contact damage is a per-tick reach test instead of MC's
  playerTouch/push callbacks; same observable cadence through i-frames.
- Squid: flee routine approximates MC's depth-aware escape with a straight
  tripled jet; ink cloud waits on particles.
- Arrow: rides the Mob pipeline (documented in Arrow.hpp); deflection on
  invulnerable targets is velocity-reverse without the sound.
- RangedBowAttackGoal: the draw is a goal-local 20-tick counter — no mob
  item-use system — with identical timing.
- Fish schools: recruitment uses the goal's own scan; MC's stream-order
  differences are unobservable.
- SmoothSwimming mobs on land: the turning-speed throttle is exact, but the
  body-pitch render lerp (xBodyRot) is not mirrored client-side yet.
- Wolf: MC syncs DATA_ANGER_END_TIME so the client's isAngry() picks the
  angry texture; here the server maps IsAngry() onto the wire's aggressive
  bit each tick (which the renderer's wolf branch already reads for the
  angry tail), overriding MeleeAttackGoal's start/stop writes — an angry
  wolf looks angry, a skeleton-hunting one stays wild-faced, as in MC.

## 2026-08-22 wave three — dragon flight + world-interaction goals

**Ender dragon** — promoted to a bespoke class (Monsters.{hpp,cpp}), the
first mob whose AI is a phase machine instead of goals, exactly as in MC.
Ported: DragonFlightHistory (the 64-sample {y, yRot} ring, recorded on both
sides), the phase manager over the wire's anim byte (MC DATA_PHASE), the
full aiStep flight physics (flap clock from real movement speed, yaw bank
via yRotA, the dot-product thrust scaling, the 0.91/slide drag), the 24-node
End-pillar graph with MC's adjacency masks and A*, HoldingPattern /
StrafePlayer (charges and fires a real DragonFireball, which now leaves the
lingering breath cloud) / Takeoff / ChargingPlayer / Dying / Hover phases,
the wing-buffet knockback + head/neck bite sweeps, checkWalls terrain
carving over the head/neck/body boxes (DRAGON_IMMUNE tag transcribed,
mobGriefing-gated), player-only damage with the body-hit reduction, the
1-HP DYING pin and dive, the sitting 25%-damage takeoff, knockback
suppressed while sitting, total effect immunity, fire immunity, and
never despawning. Render: DragonModel::SetupAnim now runs MC's real
neck/tail kinematics from the pre-lerped flight history
(EntityRenderState's dragon block), and MobRenderer poses the body from
history sample 7 + the 5-vs-10 climb pitch with MC's exact pose-stack chain
(the dragon.png sheet replacing the def row's beam-sheet artefact).
Documented deviations/skips, each at its site: the eight sub-entity
hitboxes (whole-box; sweeps and the head position rebuilt from the tickPart
layout), no End dimension (fight origin = spawn point anchors the node
ring; node height floors at the origin instead of MC's absolute 73; the
perch cycle LandingApproach/Landing/Sitting* is unported and the holding
pattern's landing roll is drawn-and-ignored), a summoned dragon STARTS the
holding pattern (MC's bare /summon hovers forever — the EndDragonFight
spawn stands in; hover + the sitting-takeoff rule still work if a phase is
forced), the 200-tick death cinematic (standard 20-tick death; the DYING
dive itself is in), crystals/crystal healing, sounds and particles, and
noPhysics (collision stands in; the carve makes it moot while griefing).
ChargingPlayer is complete but unreachable until the perch cycle lands —
its only vanilla trigger is SittingScanning.

**AreaEffectCloud** — new Misc entity (projectile/AreaEffectCloud.{hpp,cpp},
EntityTypeId 103) per AreaEffectCloud.java: wait/duration aging,
radiusPerTick / radiusOnUse / durationOnUse, the 5-tick application sweep
with the per-victim reapplication cooldown, potionDurationScale, owner
attribution, instantaneous effects at scale 0.5. Producers: DragonFireball
impact (harming II, radius 3→7 over 600 ticks, recentred on the nearest
living within 4 — the deferred TODO in HurtingProjectile is paid off) and
the creeper's death cloud (spawnLingeringCloud: only a creeper carrying
active effects leaves one — plain creepers leave nothing, as in MC; the
witch throws SPLASH potions only, verified against Witch.java, so no
lingering variant is needed). Renders as NOTHING still: MC draws the
cloud purely as particles; the particle system has since landed (see
"Particle system" above) but the cloud's ambient swirl emitter is not yet
wired — the client constructs the entity (id continuity) and MobRenderer
skips it.

**Blindness + Illusioner** — BLINDNESS appended to MobEffectId (flag-only,
like INVISIBILITY: the fog is client render work). Illusioner promoted
(Monsters) with MC's goal list: both spells in IllusionerGoals.{hpp,cpp}
(blindness 400 ticks on the target, once per target, HARD only — MC's
isHarderThan(NORMAL) without inhabited-time scaling IS the HARD setting;
the mirror spell casts real INVISIBILITY 1200 on itself, with the
four-image render trickery skip-commented), RangedBowAttackGoal(0.5, 20,
15) driving the skeleton-exact arrow math, the creaking avoidance, and the
memory-carrying target set. The renderer's illusioner arm pose now follows
Illusioner.getArmPose (BOW_AND_ARROW while aggressive, SPELLCASTING while
casting).

**Rabbit RaidGardenGoal** — ported verbatim into MoveToBlockGoal.{hpp,cpp}
(the ZombieAttackTurtleEggGoal precedent): farmland-below-max-age-carrots
targeting, the moreCarrotTicks appetite gate (40 per raid, -rand(3)/tick in
CustomServerAiStep), age-decrement via the new state-aware
EntityLevel::SetBlockState seam (MC flag 2), and MC's own oddity of the
age-0 crop breaking WITHOUT drops (setBlock(AIR) before destroyBlock)
transcribed as-is.

**Bee flower/crop goals** — BeeWanderGoal, BeePollinateGoal,
ValidateFlowerGoal, BeeGrowCropGoal ported into BeeGoals.{hpp,cpp} with
AttractsBees over the vendored BEE_ATTRACTIVE tag (sunflower upper-half and
waterlogged rules included). The per-bee state MC keeps as Bee fields lives
in the shared BeeFlowerState (the Bee class was owned by a parallel wave;
the registration lines + the anim-byte nectar bit are the coordinator
handoff — see the header comment in BeeGoals.hpp for the exact lines).
MobRenderer's bee texture now follows BeeRenderer: bee.png default, the
angry/nectar sheet matrix per instance (nectar reads anim-byte bit 2 and
stays off until the Bee-side setter lands). Deviations at their sites:
BeeGrowCropGoal's isHiveValid gate treated as satisfied (no hive block
entities — the 10-crop cap becomes per-lifetime), rain vetoes skipped (no
rain state), pitcher_crop and cave_vines growth skipped (double-block /
bonemeal-path growth), the hive goals + BeeGoToKnownFlowerGoal unported.

**Door breaking** — DoorInteractGoal + BreakDoorGoal ported
(DoorGoals.{hpp,cpp}): collision + door-node-in-path detection, the
240-tick pound with arm swings, mobGriefing + difficulty gates, and the
break clearing BOTH door halves (no double-block linkage — MC's neighbour
update stands in). The pathfinder half: PathNavigation::SetCanOpenDoors
mirrors canFloat onto the evaluator, whose DoorWoodClosed branch already
honoured it. Zombie::SetCanBreakDoors now does MC's full job (goal at
priority 1 + navigation flag, spawn-rolled and leader-forced as before,
HARD-only predicate). Vindicator promoted (Monsters) with its goal set and
VindicatorBreakDoorGoal (6-arg ctor swallowed by MC's own max(240, ...) —
transcribed as-is; NORMAL-or-HARD predicate; 1-in-10 start roll).
DEVIATION, documented at both sites: MC gates the vindicator's goal and
its per-tick canOpenDoors on raids; with no raid system those gates would
be permanently dead, so they are treated as satisfied — the pounding
itself is MC's in-raid behaviour exactly. VindicatorJohnnyAttackGoal stays
skip-commented (armed only by the "Johnny" custom name — no custom-name
system).

**Copper golem chest clips — determination (the last BUG-class gap).**
Investigated 2026-08-22: chests are NOT render-only. The engine has real
chest block entities with server-side inventories
(`src/common/world/block/entity/ChestBlockEntity` — 27 slots on
BaseContainerBlockEntity, double-chest pairing via FindChestPartner, menus
the player can open; ChestRenderer is only the visual half), and the brain
layer already declares the three memories MC's
TransportItemsBetweenContainers needs (GeneratedMemoryModules:
VisitedBlockPositions, UnreachableTransportBlockPositions,
TransportItemsCooldownTicks). The five dead clips are therefore
IMPLEMENTABLE at MC's exact moments: the golem walks to a chest
(TransportItemsBetweenContainers: source = copper chests, destination =
chest/trapped chest, hand-empty decides direction), and at tick 1 of the
60-tick interaction the synched CopperGolemState picks the clip
(GETTING_ITEM / GETTING_NO_ITEM when it arrives empty-handed at a
non-empty/empty chest; DROPPING_ITEM / DROPPING_NO_ITEM when it arrives
holding at a chest with/without room; IDLE re-arms a 200-240-tick head-spin
between trips), with the item moving at tick 60. IMPLEMENTED (follow-up
wave): CopperGolem promoted (AnimatedMobs, state + holding bit on the anim
byte), CopperGolemAi + TransportItemsBetweenContainers ported whole
(`ai/brain/CopperGolemAi.{hpp,cpp}`), `MobRenderer` sets
`state.isHoldingItem`, and the audit's five dead-clip findings are gone.
Seams this took: `EntityLevel::GetContainerBlockEntity(pos)` /
`GetContainerBlockEntities(center, chunkRange)` (MC Level.getBlockEntity +
the chunk BE walk, server bridge over Chunk's BE map) and
`PathNavigation::SetRequiredPathLength`. Deviations, each commented at its
site: copper chests are decorative-only blocks (no BE registration), so
regular chests serve as BOTH source and destination; the GlobalPos-set
memories live on the behaviour (no position-set memory variant — sniffer
precedent); no ContainerOpenersCounter, so startOpen/stopOpen are
skip-commented (lid stays closed) and shouldQueueForTarget never queues;
weathering/statue/lightning/waxing/shearing skipped.

**Seams added for this wave** — `EntityLevel::SetBlockState(pos, state)`
(state-aware block write, MC Level.setBlock flag 2; ServerLevelBridge
forwards to World::SetBlock with MarkDirty) used by the rabbit and bee
goals; `PathNavigation::SetCanOpenDoors`; MobEffectId::Blindness;
EntityTypeId::AreaEffectCloud (appended, wire-visible order preserved).
