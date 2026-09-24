// File: src/common/sound/EntitySounds.hpp
//
// Every entity type's sounds, as MC's classes define them — the generated
// table (tools/gen_entity_sounds.py → GeneratedEntitySounds.inc) behind the
// defaults of Mob.getAmbientSound, LivingEntity.getHurtSound / getDeathSound /
// getSoundVolume, Entity.playStepSound / getSwimSound / getSwimSplashSound /
// getSwimHighSpeedSplashSound / getSoundSource / getMovementEmission and
// Mob.getAmbientSoundInterval.
//
// A sound slot is a pick: one event, or the two arms of a ternary on a
// predicate the engine can evaluate (MC `this.isBaby() ? BABY : ADULT`,
// `this.isInWater() ? WATER : LAND`, ...). A class whose MC method depends on
// more than that (AI state, a variant, a block tag) hand-codes its override on
// the engine class; the table then holds the parent's answer.
//
// Keyed by EntityTypeId (built once from the type slugs), so an engine class
// shared by several types (the generic mobs) gets each type's own row.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/sound/SoundSource.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game {

    class Entity;
    struct EntityLevel;

    // MC entity level events (Level.levelEvent(null, type, pos, 0) from mob
    // code) — the sound half of LevelEventHandler for the entity ids, with
    // its exact event, source, volume and pitch. (The block/world ids live
    // in LevelEventSounds.hpp.)
    namespace EntityLevelEvent {
        inline constexpr int GHAST_WARNING            = 1015;
        inline constexpr int GHAST_SHOOT              = 1016;
        inline constexpr int DRAGON_SHOOT             = 1017;
        inline constexpr int BLAZE_SHOOT              = 1018;
        inline constexpr int ZOMBIE_ATTACK_WOODEN_DOOR = 1019;
        inline constexpr int ZOMBIE_ATTACK_IRON_DOOR  = 1020;
        inline constexpr int ZOMBIE_BREAK_WOODEN_DOOR = 1021;
        inline constexpr int WITHER_BREAK_BLOCK       = 1022;
        inline constexpr int WITHER_SHOOT             = 1024;
        inline constexpr int BAT_TAKEOFF              = 1025;
        inline constexpr int ZOMBIE_INFECT            = 1026;
        inline constexpr int ZOMBIE_VILLAGER_CONVERTED = 1027;
        inline constexpr int PHANTOM_BITE             = 1039;
        inline constexpr int ZOMBIE_TO_DROWNED        = 1040;
        inline constexpr int HUSK_TO_ZOMBIE           = 1041;
        inline constexpr int SKELETON_TO_STRAY        = 1048;
    }

    // Plays `type`'s sound at the block centre of `pos` to everyone in range
    // (the pitch jitter rolled once, here). False for an id not listed above.
    bool PlayEntityLevelEventSound(EntityLevel& level, int type, const glm::ivec3& pos);

    enum class EntitySoundPred : uint8_t { None, Baby, InWater, UnderWater, OnGround };
    enum class EntityStepMode  : uint8_t { Block, Event, None };

    struct EntitySoundPick {
        EntitySoundPred pred;
        const char*     whenTrue;
        const char*     whenFalse;
    };

    struct EntitySoundRow {
        SoundSource     source;
        float           soundVolume;       // MC getSoundVolume
        int             ambientInterval;   // MC getAmbientSoundInterval
        EntitySoundPick ambient;
        EntitySoundPick hurt;
        EntitySoundPick death;
        EntityStepMode  stepMode;          // Block: the block's SoundType (Entity default)
        EntitySoundPick step;
        float           stepVolume;
        float           stepPitch;
        const char*     swim;
        const char*     splash;
        const char*     splashHighSpeed;
    };

    const EntitySoundRow& EntitySoundsOf(EntityTypeId type);

    // The pick's event for this entity now ("" = none, MC null).
    const char* PickSound(const EntitySoundPick& pick, const Entity& entity);

} // namespace Game
