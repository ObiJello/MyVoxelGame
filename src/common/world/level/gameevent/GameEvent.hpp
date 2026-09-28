// File: src/common/world/level/gameevent/GameEvent.hpp
//
// Port of net.minecraft.world.level.gameevent.GameEvent (26.3) — the
// registry of "something happened here" signals that sculk sensors,
// shriekers, catalysts and the warden listen for — plus the event tags
// (data/minecraft/tags/game_event/*.json) and VibrationSystem's frequency
// table, which are per-event constants.
//
// MC registers each event as a Holder<GameEvent> carrying only its
// notification radius. Here an event is an enum in MC's registration order
// (BuiltInRegistries.GAME_EVENT ids), and everything that hangs off an event —
// its name, radius, tag membership and vibration frequency — is a table
// lookup.
//
// Emitting one: ILevelWrite::GameEvent / Entity::GameEvent (GameEventLevel.cpp),
// which forward to the server level's GameEventDispatcher. The client's level
// has no dispatcher, so an emit there is the no-op MC's ClientLevel.gameEvent is.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Game {

    class Entity;

    // MC GameEvent's registered holders, in registration order.
#define GAME_EVENT_LIST(X)                                   \
    X(BlockActivate,               "block_activate",   16)   \
    X(BlockAttach,                 "block_attach",     16)   \
    X(BlockChange,                 "block_change",     16)   \
    X(BlockClose,                  "block_close",      16)   \
    X(BlockDeactivate,             "block_deactivate", 16)   \
    X(BlockDestroy,                "block_destroy",    16)   \
    X(BlockDetach,                 "block_detach",     16)   \
    X(BlockOpen,                   "block_open",       16)   \
    X(BlockPlace,                  "block_place",      16)   \
    X(Bounce,                      "bounce",           16)   \
    X(ContainerClose,              "container_close",  16)   \
    X(ContainerOpen,               "container_open",   16)   \
    X(Drink,                       "drink",            16)   \
    X(Eat,                         "eat",              16)   \
    X(ElytraGlide,                 "elytra_glide",     16)   \
    X(EntityDamage,                "entity_damage",    16)   \
    X(EntityDie,                   "entity_die",       16)   \
    X(EntityDismount,              "entity_dismount",  16)   \
    X(EntityInteract,              "entity_interact",  16)   \
    X(EntityMount,                 "entity_mount",     16)   \
    X(EntityPlace,                 "entity_place",     16)   \
    X(EntityAction,                "entity_action",    16)   \
    X(Equip,                       "equip",            16)   \
    X(Explode,                     "explode",          16)   \
    X(Flap,                        "flap",             16)   \
    X(FluidPickup,                 "fluid_pickup",     16)   \
    X(FluidPlace,                  "fluid_place",      16)   \
    X(HitGround,                   "hit_ground",       16)   \
    X(InstrumentPlay,              "instrument_play",  16)   \
    X(ItemInteractFinish,          "item_interact_finish", 16) \
    X(ItemInteractStart,           "item_interact_start",  16) \
    X(JukeboxPlay,                 "jukebox_play",     10)   \
    X(JukeboxStopPlay,             "jukebox_stop_play", 10)  \
    X(LightningStrike,             "lightning_strike", 16)   \
    X(NoteBlockPlay,               "note_block_play",  16)   \
    X(PrimeFuse,                   "prime_fuse",       16)   \
    X(ProjectileLand,              "projectile_land",  16)   \
    X(ProjectileShoot,             "projectile_shoot", 16)   \
    X(SculkSensorTendrilsClicking, "sculk_sensor_tendrils_clicking", 16) \
    X(Shear,                       "shear",            16)   \
    X(Shriek,                      "shriek",           32)   \
    X(Splash,                      "splash",           16)   \
    X(Step,                        "step",             16)   \
    X(Swim,                        "swim",             16)   \
    X(Teleport,                    "teleport",         16)   \
    X(Unequip,                     "unequip",          16)   \
    X(Resonate1,                   "resonate_1",       16)   \
    X(Resonate2,                   "resonate_2",       16)   \
    X(Resonate3,                   "resonate_3",       16)   \
    X(Resonate4,                   "resonate_4",       16)   \
    X(Resonate5,                   "resonate_5",       16)   \
    X(Resonate6,                   "resonate_6",       16)   \
    X(Resonate7,                   "resonate_7",       16)   \
    X(Resonate8,                   "resonate_8",       16)   \
    X(Resonate9,                   "resonate_9",       16)   \
    X(Resonate10,                  "resonate_10",      16)   \
    X(Resonate11,                  "resonate_11",      16)   \
    X(Resonate12,                  "resonate_12",      16)   \
    X(Resonate13,                  "resonate_13",      16)   \
    X(Resonate14,                  "resonate_14",      16)   \
    X(Resonate15,                  "resonate_15",      16)

    enum class GameEventId : uint8_t {
#define GAME_EVENT_ENUM(Enum, name, radius) Enum,
        GAME_EVENT_LIST(GAME_EVENT_ENUM)
#undef GAME_EVENT_ENUM
        Count
    };
    inline constexpr size_t kGameEventCount = static_cast<size_t>(GameEventId::Count);

    // MC GameEvent.DEFAULT_NOTIFICATION_RADIUS.
    inline constexpr int kDefaultGameEventRadius = 16;

    namespace GameEvents {

        // MC GameEvent.notificationRadius.
        int NotificationRadius(GameEventId event);

        // The registry path ("step", "resonate_3"); "minecraft:" is implied.
        std::string_view Name(GameEventId event);

        // By registry id, with or without the "minecraft:" namespace.
        std::optional<GameEventId> FromName(std::string_view name);

        // ── Event tags (data/minecraft/tags/game_event) ─────────────────────
        //
        // Resolved once from the data pack, nested tags followed.
        enum class Tag : uint8_t {
            Vibrations,                // #minecraft:vibrations
            WardenCanListen,           // #minecraft:warden_can_listen
            ShriekerCanListen,         // #minecraft:shrieker_can_listen
            IgnoreVibrationsSneaking,  // #minecraft:ignore_vibrations_sneaking
            AllayCanListen,            // #minecraft:allay_can_listen
        };
        bool Is(GameEventId event, Tag tag);

        // MC VibrationSystem.getGameEventFrequency — the 1..15 "frequency" a
        // sculk sensor's comparator reads for the event it last heard, 0 for
        // an event that carries none (a sensor ignores those outright).
        int Frequency(GameEventId event);

        // MC VibrationSystem.getResonanceEventByFrequency(1..15).
        GameEventId ResonanceEventForFrequency(int frequency);

    } // namespace GameEvents

    // MC GameEvent.Context — who caused the event, and the block state it
    // concerned (the state broken, placed or stepped on). A state is what
    // #dampens_vibrations (wool, carpets) is tested against.
    //
    // `sourceEntity` is only valid for the duration of the post; anything a
    // listener keeps past it (a scheduled vibration) holds the UUID instead.
    struct GameEventContext {
        Entity*                   sourceEntity = nullptr;
        std::optional<BlockState> affectedState;

        static GameEventContext Of(Entity* source) { return GameEventContext{source, std::nullopt}; }
        static GameEventContext Of(BlockState state) { return GameEventContext{nullptr, state}; }
        static GameEventContext Of(Entity* source, BlockState state) { return GameEventContext{source, state}; }
        static GameEventContext Of(Entity* source, std::optional<BlockState> state) {
            return GameEventContext{source, state};
        }
    };

} // namespace Game
