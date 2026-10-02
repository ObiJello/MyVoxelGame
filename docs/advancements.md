# Advancements

A port of Minecraft 26.3's advancement system: the data pack's advancements,
their criteria triggers wired into gameplay, per-player progress saved in MC's
file format, the update packets, the toast, the chat announcement, the
Advancements screen and `/advancement`.

## Pieces

| Layer | Files | MC counterpart |
|---|---|---|
| Data model | `common/advancements/Advancement.{hpp,cpp}` | `Advancement`, `DisplayInfo`, `AdvancementType`, `AdvancementRequirements`, `AdvancementProgress`, `CriterionProgress`, `AdvancementNode`/`AdvancementTree`, `TreeNodePosition` |
| Loader | `common/advancements/AdvancementLoader.{hpp,cpp}` | `ServerAdvancementManager.apply` |
| Predicates | `server/advancements/AdvancementPredicates.{hpp,cpp}` | `advancements/predicates/*`, the loot conditions |
| Triggers | `server/advancements/CriteriaTriggers.{hpp,cpp}` | `advancements/triggers/*` |
| Progress | `server/advancements/PlayerAdvancements.{hpp,cpp}` | `PlayerAdvancements`, `AdvancementVisibilityEvaluator` |
| Lifecycle | `server/advancements/ServerAdvancements.{hpp,cpp}` | `ServerAdvancementManager`, the advancement half of `PlayerList` and `ServerPlayer.doTick` |
| Chat | `server/advancements/AdvancementText.{hpp,cpp}` | `AdvancementType.createAnnouncement`, `Advancement.name` |
| Packets | `common/network/packets/game/AdvancementPackets.hpp` (0xC0–0xC2) | `ClientboundUpdateAdvancementsPacket`, `ClientboundSelectAdvancementsTabPacket`, `ServerboundSeenAdvancementsPacket` |
| Command | `server/commands/AdvancementCommand.{hpp,cpp}` | `AdvancementCommands` |
| Client mirror | `client/advancements/ClientAdvancements.{hpp,cpp}` | `ClientAdvancements` |
| Toasts | `client/renderer/gui/toasts/ToastManager.{hpp,cpp}` | `ToastManager`, `Toast`, `AdvancementToast` |
| Screen | `client/renderer/gui/screens/AdvancementsScreen.{hpp,cpp}` | `AdvancementsScreen`, `AdvancementTab`, `AdvancementTabType`, `AdvancementWidget`, `AdvancementWidgetType` |

### Data

The advancements are read at runtime from `data/<ns>/advancement/**.json` (the
vanilla data pack already in the repo; a namespace dropped next to it loads
the same way), with `Advancement.CODEC`'s rules. A criterion keeps its trigger
id and its raw `conditions` JSON, which the server's trigger code interprets —
there is no generated table to keep in step with the data.

Every root with a display is laid out once at load (`TreeNodePosition.run`),
so a display carries its x/y from then on; the server sends it with the
advancement, as MC does.

The recipe-unlock advancements (`advancement/recipes/**`: no display, recipe
rewards only) are **not** loaded. They exist to fill the recipe book, which
this engine does not have, and would put ~1500 always-listening criteria on
every player. The loader skips anything of that shape, not the folder by name.

Display icons are decoded by the server through its item NBT codecs (the icon
JSON is SNBT), so component icons — the ominous banner, the sherd pot — arrive
complete on the client.

### Triggers

`Server::CriteriaTriggers::*` is one function per MC trigger, called from the
site MC calls it from. Shared (common) code reaches the server through
`CriteriaTriggers::PlayerOf(entity | IUsePlayer)`, which answers null on the
client, so prediction paths do nothing. A trigger looks up the player's
listeners for its type, tests each criterion's own fields and then its shared
`player` predicate (SimpleCriterionTrigger), collects the matches and awards
them after the walk.

Predicates are evaluated from the JSON: entity (type and tags, distance,
location, stepping_on, movement_affected_by, movement, effects, flags,
equipment, vehicle, passenger, targeted_entity, periodic_tick, the cat / wolf /
frog variant components, and the player / lightning / fishing_hook / slime /
sheep type_specific forms including `looking_at`), location (position,
dimension, biomes, structures, smokey, light, block, fluid, can_see_sky), item
(through `ComponentNbt::ItemPredicateMatches` over the predicate's NBT form),
damage, damage source, distance, mob effects, block and state properties, and
the loot conditions entity_properties, location_check, block_state_property,
match_tool, all_of, any_of, inverted, random_chance, weather_check and
time_check. Entity `nbt`, `team` and `slots` predicates, and the player
predicate's `stats`, `recipes` and `advancements`, never match (no vanilla
advancement uses them).

`structures` uses `LocateFinder::FindStructureStartAt`, which now memoises
each start's piece boxes per (generator, seed, structure, chunk): the location
trigger asks every second for every player, and a bastion's jigsaw layout
costs milliseconds.

The player-tick triggers (TICK, LOCATION every 20 ticks, LEVITATION,
FALL_FROM_HEIGHT, FALL_AFTER_EXPLOSION, RIDE_ENTITY_IN_LAVA) and
INVENTORY_CHANGED run from `ServerAdvancements::TickPlayer`, called from
`PlayerSession::Tick` after the player ticks. INVENTORY_CHANGED fires per slot
of the inventory's diff since the last tick (the engine's menus write the
inventory directly, so the diff is MC's container listener). A fall is the
fall distance leaving and returning to zero (MC's `trackStartFallingPosition` /
`resetFallDistance`).

A player's death runs MC `ServerPlayer.die`'s kill credit
(`ServerPlayer.cpp` `AwardDeathCriteria`): the blow's causing living entity,
else the last mob to hurt the player within 100 ticks, gets
ENTITY_KILLED_PLAYER for the victim (Adventure's root) and, when it is a
player, PLAYER_KILLED_ENTITY. Mob deaths award from
`LivingEntity::AwardKillCriteria`.

### Progress, saving, packets

`PlayerAdvancements` follows MC: listeners registered for every undone
criterion of every undone advancement, award/revoke, visibility re-evaluated
per changed root (`AdvancementVisibilityEvaluator`, depth 2), and one
`UpdateAdvancementsS2C` per tick with what became visible, what stopped being
visible and the progress that changed (the first one is the reset that
rebuilds the client's tree). Rewards (experience, loot tables) and the chat
announcement run at that flush, i.e. at the end of the player's tick, rather
than inside `award()` — a trigger fired from inside a mob's tick never
re-enters the world from there. Recipe and function rewards are carried in the
data and skipped (no recipe book, no function system).

The file is `<world>/advancements/<uuid>.json` (the offline-player UUID that
names `playerdata/<uuid>.dat`): per advancement with progress,
`{"criteria": {name: "yyyy-MM-dd HH:mm:ss +0000"}, "done": bool}`, plus
`DataVersion` — what a vanilla server reads. Loaded on join, saved with the
player data (autosave, disconnect, shutdown), written through a temp file. A
read-only (imported) world never writes it.

The per-player trigger state MC keeps on `ServerPlayer` (entered-nether
position, levitation start, fall start, explosion impact, lava ride start)
lives on `PlayerAdvancements` and is not persisted, so a NETHER_TRAVEL started
before a restart does not complete after it.

### Chat

A completed advancement with `announce_to_chat` is broadcast to every player as
`chat.type.advancement.<frame>` with the player's name and the advancement's
bracketed name — the frame's colour, the title and description in its hover —
when `show_advancement_messages` is on.

### Client

`ClientAdvancements` mirrors the visible tree and progress. It is cleared with
the session (PlatformMain's teardown — MC builds one per connection): a world
whose player has nothing visible sends no reset packet, so the previous
world's tree would otherwise survive into it. A progress update
that completes an advancement with `show_toast` queues an `AdvancementToast`
(not for the join reset, not for `/advancement grant … everything`). The
`ToastManager` is drawn over the HUD and any open screen.

The Advancements screen opens from the pause menu (vanilla's Advancements
button; Friends moved into vanilla's Statistics slot) and with the
advancements key (L by default, `key.advancements`; not while the debug
modifier is held, where L is the profiler chord). It is MC's: the window,
up to 26 tabs around it, the tiled background, the connection lines, the
frames and icons, drag (left button) and wheel scrolling, the hover with its
progress-filled title bar and the description in the type's colour, and the
"nothing here" face for a player with nothing visible.

### Cheats

In this engine advancements do **not** progress in a world with cheats on
(level.dat `allowCommands`, the World Options "Allow Cheats") unless the
world's engine rule `advancements_with_cheats` ("Advancements work with cheats
on", default false) is on. While switched off no trigger awards anything; the
screen, the existing progress and `/advancement` keep working. The tick that
finds awarding switched back on (the rule turned on, or cheats turned off,
mid-session) evaluates the player as at login: every undone criterion's
listener re-registered, LOCATION fired at once, and INVENTORY_CHANGED fired
for every held stack — the inventory trigger's snapshot is dropped while
awarding is off, so a crafting table carried all along still earns the
Minecraft root the moment the rule goes on. The rule
follows the engine-rule pattern: `GameRuleCommand`'s engine table (so it
appears on the Edit Game Rules screen), `IntegratedServerConfig`, and
`level.dat`'s `obeycraft.advancements_with_cheats`.

### Command

`/advancement (grant|revoke) <targets> everything | only <advancement>
[<criterion>] | from|through|until <advancement>` with MC's semantics and
messages. Tab completion offers the advancements the server loads
(`Cmd::Arg::Advancement`, bare ids) and the criteria of the one typed
(`Cmd::Arg::AdvancementCriterion`).

## Advancements that cannot be earned yet

Each needs a subsystem the engine does not have. The trigger is implemented
and the advancement loads; it simply never fires.

| Advancement | Missing |
|---|---|
| `adventure/hero_of_the_village` | raids (HERO_OF_THE_VILLAGE / raid win) |
| `adventure/voluntary_exile` | killing a captain is tracked, but it needs captains carrying the ominous banner component exactly as vanilla patrols do; raids themselves are missing |
| `adventure/honey_block_slide` | the honey block's wall slide (SLIDE_DOWN_BLOCK) |
| `adventure/crafters_crafting_crafters` | crafter crafting on a redstone pulse (CRAFTER_RECIPE_CRAFTED) |
| `adventure/spyglass_at_parrot`, `spyglass_at_ghast`, `spyglass_at_dragon` | using the spyglass (it never enters item use, so USING_ITEM never fires for it) |
| `nether/create_beacon`, `nether/create_full_beacon` | the beacon pyramid / beacon block entity (CONSTRUCT_BEACON) |
| `story/cure_zombie_villager` | zombie villager curing (CURED_ZOMBIE_VILLAGER) |
| `nether/all_effects` | effects the engine never applies to a player (bad omen / raid omen from raids, among others) |

Partly dependent on engine coverage (they work as far as the underlying
feature does): structure location advancements need the structure's
generator implemented (`Structures::isImplemented`); `adventure/walk_on_powder_snow_with_leather_boots`
needs powder snow; `husbandry/place_dried_ghast_in_water` needs the dried ghast
block; `adventure/craft_decorated_pot_using_only_sherds` needs the decorated pot
recipe to carry its `minecraft:decorated_pot` id; recipe-unlock advancements are
not loaded at all (see above).
