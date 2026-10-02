// File: src/common/sound/LevelEventSounds.hpp
//
// The SOUND half of MC's level events (Level.levelEvent(except, type, pos,
// data) → ClientboundLevelEventPacket → client LevelEventHandler.levelEvent).
//
// This engine has no level-event packet: the particle halves live in their own
// packets or are not drawn yet, and the sound halves are played here as
// ordinary Level.playSound calls with LevelEventHandler's exact event, source,
// volume and pitch — the server broadcasts them to everyone in range but
// `except` (MC's levelEvent excepts the same player), a predicting client
// plays its own. The pitch jitter is rolled where the event fires, so every
// client hears the same pitch rather than one each.
//
// Only the events whose SOUND a block, item or world site in this engine
// raises are listed; an unlisted type plays nothing (and says so in debug
// builds via the return value).
#pragma once

#include "common/core/JavaRandom.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/GameRules.hpp"

#include <glm/glm.hpp>

namespace Game {

    // MC LevelEvent constants, the ones played through PlayLevelEventSound.
    namespace LevelEvent {
        inline constexpr int SOUND_DISPENSER_DISPENSE     = 1000;
        inline constexpr int SOUND_DISPENSER_FAIL         = 1001;
        inline constexpr int SOUND_DISPENSER_PROJECTILE_LAUNCH = 1002;
        inline constexpr int SOUND_EXTINGUISH_FIRE        = 1009;
        inline constexpr int SOUND_ANVIL_BROKEN           = 1029;
        inline constexpr int SOUND_ANVIL_USED             = 1030;
        inline constexpr int SOUND_ANVIL_LAND             = 1031;
        inline constexpr int SOUND_CHORUS_GROW            = 1033;
        inline constexpr int SOUND_CHORUS_DEATH           = 1034;
        inline constexpr int SOUND_BREWING_STAND_BREW     = 1035;
        inline constexpr int SOUND_GRINDSTONE_USED        = 1042;
        inline constexpr int SOUND_PAGE_TURN              = 1043;
        inline constexpr int SOUND_SMITHING_TABLE_USED    = 1044;
        inline constexpr int SOUND_POINTED_DRIPSTONE_LAND = 1045;
        inline constexpr int SOUND_DRIP_LAVA_INTO_CAULDRON  = 1046;
        inline constexpr int SOUND_DRIP_WATER_INTO_CAULDRON = 1047;
        inline constexpr int SOUND_CRAFTER_CRAFT          = 1049;
        inline constexpr int SOUND_CRAFTER_FAIL           = 1050;
        // WindChargeItem's DispenseConfig.overrideDispenseEvent(1051).
        inline constexpr int SOUND_WIND_CHARGE_SHOOT      = 1051;
        inline constexpr int LAVA_FIZZ                    = 1501;
        inline constexpr int REDSTONE_TORCH_BURNOUT       = 1502;
        inline constexpr int END_PORTAL_FRAME_FILL        = 1503;
        inline constexpr int PARTICLES_AND_SOUND_PLANT_GROWTH = 1505;
        inline constexpr int PARTICLES_DESTROY_BLOCK      = 2001;
        // The particle-only events (and those whose sound LevelEventHandler
        // plays itself), for PlayLevelEvent / the client's LevelEventHandler
        // port (client/world/ClientLevelEvents.cpp).
        inline constexpr int SOUND_FIREWORK_SHOOT         = 1004;
        inline constexpr int SOUND_SPELL_POTION_SPLASH    = 1053;
        inline constexpr int SOUND_INSTANT_POTION_SPLASH  = 1054;
        inline constexpr int COMPOSTER_FILL               = 1500;
        inline constexpr int DRIPSTONE_DRIP               = 1504;
        inline constexpr int PARTICLES_SHOOT_SMOKE        = 2000;
        inline constexpr int PARTICLES_SPELL_POTION_SPLASH = 2002;
        inline constexpr int PARTICLES_EYE_OF_ENDER_DEATH = 2003;
        inline constexpr int PARTICLES_MOBBLOCK_SPAWN     = 2004;
        inline constexpr int PARTICLES_DRAGON_FIREBALL_SPLASH = 2006;
        inline constexpr int PARTICLES_INSTANT_POTION_SPLASH = 2007;
        inline constexpr int PARTICLES_DRAGON_BLOCK_BREAK = 2008;
        inline constexpr int PARTICLES_WATER_EVAPORATING  = 2009;
        inline constexpr int PARTICLES_SHOOT_WHITE_SMOKE  = 2010;
        inline constexpr int PARTICLES_BEE_GROWTH         = 2011;
        inline constexpr int PARTICLES_TURTLE_EGG_PLACEMENT = 2012;
        inline constexpr int PARTICLES_SMASH_ATTACK       = 2013;
        inline constexpr int PARTICLES_DESTROY_BLOCK_ONLY = 2014;   // MC PARTICLES_DESTROY_BLOCK (no sound)
        inline constexpr int PARTICLES_DRAGON_EGG_TELEPORT = 2015;
        inline constexpr int PARTICLES_SHULKER_TELEPORT   = 2016;
        inline constexpr int PARTICLES_CONSUME_EFFECT_TELEPORT = 2017;
        inline constexpr int PARTICLES_ENDERMAN_TELEPORT  = 2018;
        inline constexpr int PARTICLES_DESTROY_PROGRESS   = 2019;
        inline constexpr int PARTICLES_AND_SOUND_DESTROY_PROGRESS = 2020;
        inline constexpr int ANIMATION_END_GATEWAY_SPAWN  = 3000;
        inline constexpr int ANIMATION_DRAGON_SUMMON_ROAR = 3001;
        inline constexpr int PARTICLES_ELECTRIC_SPARK     = 3002;
        inline constexpr int PARTICLES_WAX_ON             = 3003;
        inline constexpr int PARTICLES_WAX_OFF            = 3004;
        inline constexpr int PARTICLES_SCRAPE             = 3005;
        inline constexpr int PARTICLES_SCULK_CHARGE       = 3006;
        inline constexpr int PARTICLES_SCULK_SHRIEK       = 3007;
        inline constexpr int PARTICLES_AND_SOUND_BRUSH_BLOCK_COMPLETE = 3008;
        inline constexpr int PARTICLES_EGG_CRACK          = 3009;
        inline constexpr int PARTICLES_TRIAL_SPAWNER_SPAWN = 3011;
        inline constexpr int PARTICLES_TRIAL_SPAWNER_SPAWN_MOB_AT = 3012;
        inline constexpr int PARTICLES_TRIAL_SPAWNER_DETECT_PLAYER = 3013;
        inline constexpr int ANIMATION_TRIAL_SPAWNER_EJECT_ITEM = 3014;
        inline constexpr int ANIMATION_VAULT_ACTIVATE     = 3015;
        inline constexpr int ANIMATION_VAULT_DEACTIVATE   = 3016;
        inline constexpr int ANIMATION_VAULT_EJECT_ITEM   = 3017;
        inline constexpr int ANIMATION_SPAWN_COBWEB       = 3018;
        inline constexpr int PARTICLES_TRIAL_SPAWNER_DETECT_PLAYER_OMINOUS = 3019;
        inline constexpr int PARTICLES_TRIAL_SPAWNER_BECOME_OMINOUS = 3020;
        inline constexpr int PARTICLES_TRIAL_SPAWNER_SPAWN_ITEM = 3021;
        // Global level events (ServerLevel.globalLevelEvent).
        inline constexpr int SOUND_WITHER_BOSS_SPAWN      = 1023;
        inline constexpr int SOUND_DRAGON_DEATH           = 1028;
        inline constexpr int SOUND_END_PORTAL_SPAWN       = 1038;
    }

    // MC ServerLevel.globalLevelEvent(type, pos, data) — the wither's spawn
    // scream, the dragon's death and the end portal's opening, heard by every
    // player however far away (LevelEventHandler.globalLevelEvent places it 2
    // blocks from each camera, in the event's direction).
    //
    // DEVIATION: the sound sink sends one position to everyone, so this plays
    // AT the event with vanilla's thunder trick — a volume of 10000, whose
    // send range and linear attenuation distance (16 × volume) cover any
    // loaded world, while the engine clamps the loudness itself to 1.0 as MC
    // does. Players in other dimensions do not hear it.
    //
    // global_sound_events off: MC falls back to a plain levelEvent, and the
    // client's LevelEventHandler.levelEvent has no case for 1023/1028/1038 —
    // the event reaches the players near it and plays nothing. So here the
    // sound is not played at all (the type still counts as handled).
    template <class LevelT>
    bool PlayGlobalLevelEventSound(LevelT& level, int type, const glm::ivec3& pos) {
        constexpr float kGlobalVolume = 10000.0f;
        if (!Game::Rules::GetBool(Game::Rules::Id::GlobalSoundEvents)) {
            return type == LevelEvent::SOUND_WITHER_BOSS_SPAWN ||
                   type == LevelEvent::SOUND_DRAGON_DEATH ||
                   type == LevelEvent::SOUND_END_PORTAL_SPAWN;
        }
        const glm::dvec3 at = Sound::BlockCenter(pos);
        switch (type) {
            case LevelEvent::SOUND_WITHER_BOSS_SPAWN:
                level.PlaySound(nullptr, at, SoundEvents::WITHER_SPAWN, SoundSource::Hostile, kGlobalVolume, 1.0f);
                return true;
            case LevelEvent::SOUND_DRAGON_DEATH:
                level.PlaySound(nullptr, at, SoundEvents::ENDER_DRAGON_DEATH, SoundSource::Hostile, kGlobalVolume, 1.0f);
                return true;
            case LevelEvent::SOUND_END_PORTAL_SPAWN:
                level.PlaySound(nullptr, at, SoundEvents::END_PORTAL_SPAWN, SoundSource::Hostile, kGlobalVolume, 1.0f);
                return true;
            default:
                return false;
        }
    }

    // Play the sound half of level event `type` at `pos` (a block position;
    // LevelEventHandler's playLocalSound(BlockPos) is the cell centre).
    // `data` is the event's int (2001: the broken state's raw id; 1009: 0 =
    // fire, 1 = generic). `level` is an ILevelWrite or an EntityLevel — both
    // carry MC's playSound. Returns whether the type has a sound here.
    // Whether PlayLevelEventSound plays `type`'s sound — i.e. the sound
    // travels as its own sound packet, and the client's LevelEventHandler
    // port must NOT play it again when the level event arrives. Kept in step
    // with the switch below.
    // MC BlockUtil.clampedPackDifferenceInPosition: the data of the teleport
    // level events (2015-2018) — `to - from` per axis, clamped to ±127 and
    // to the radius range, one byte each (x << 16 | y << 8 | z).
    inline int ClampedPackDifferenceInPosition(const glm::ivec3& from, const glm::ivec3& to, int xRadius,
                                               int yRadius, int zRadius) {
        const auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
        xRadius = clamp(xRadius, 0, 127);
        yRadius = clamp(yRadius, 0, 127);
        zRadius = clamp(zRadius, 0, 127);
        const int xd = clamp(to.x - from.x, -127, 127);
        const int yd = clamp(to.y - from.y, -127, 127);
        const int zd = clamp(to.z - from.z, -127, 127);
        return (((xd + xRadius) & 255) << 16) | (((yd + yRadius) & 255) << 8) | ((zd + zRadius) & 255);
    }

    inline constexpr bool LevelEventSoundIsNetworked(int type) {
        switch (type) {
            case LevelEvent::SOUND_DISPENSER_DISPENSE:
            case LevelEvent::SOUND_DISPENSER_FAIL:
            case LevelEvent::SOUND_DISPENSER_PROJECTILE_LAUNCH:
            case LevelEvent::SOUND_EXTINGUISH_FIRE:
            case LevelEvent::SOUND_ANVIL_BROKEN:
            case LevelEvent::SOUND_ANVIL_USED:
            case LevelEvent::SOUND_ANVIL_LAND:
            case LevelEvent::SOUND_CHORUS_GROW:
            case LevelEvent::SOUND_CHORUS_DEATH:
            case LevelEvent::SOUND_BREWING_STAND_BREW:
            case LevelEvent::SOUND_GRINDSTONE_USED:
            case LevelEvent::SOUND_PAGE_TURN:
            case LevelEvent::SOUND_SMITHING_TABLE_USED:
            case LevelEvent::SOUND_POINTED_DRIPSTONE_LAND:
            case LevelEvent::SOUND_DRIP_LAVA_INTO_CAULDRON:
            case LevelEvent::SOUND_DRIP_WATER_INTO_CAULDRON:
            case LevelEvent::SOUND_CRAFTER_CRAFT:
            case LevelEvent::SOUND_CRAFTER_FAIL:
            case LevelEvent::LAVA_FIZZ:
            case LevelEvent::REDSTONE_TORCH_BURNOUT:
            case LevelEvent::END_PORTAL_FRAME_FILL:
            case LevelEvent::PARTICLES_AND_SOUND_PLANT_GROWTH:
            case LevelEvent::PARTICLES_DESTROY_BLOCK:
                return true;
            default:
                return false;
        }
    }

    template <class LevelT>
    bool PlayLevelEventSound(LevelT& level, const SoundExcept& except, int type,
                             const glm::ivec3& pos, int data, JavaRandom* random) {
        // The particle half (MC Level.levelEvent's packet / the predicting
        // client's LevelEventHandler call) — see EntityLevel::PlayLevelEvent.
        level.PlayLevelEvent(except, type, pos, data);
        auto f = [random]() { return random ? random->NextFloat() : 0.5f; };
        // (random.nextFloat() - random.nextFloat()) as MC writes it: two draws.
        auto spread = [&f]() { const float a = f(); return a - f(); };
        const glm::dvec3 at = Sound::BlockCenter(pos);
        switch (type) {
            case LevelEvent::SOUND_DISPENSER_DISPENSE:
                level.PlaySound(except, at, SoundEvents::DISPENSER_DISPENSE, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::SOUND_DISPENSER_FAIL:
                level.PlaySound(except, at, SoundEvents::DISPENSER_FAIL, SoundSource::Blocks, 1.0f, 1.2f);
                return true;
            case LevelEvent::SOUND_DISPENSER_PROJECTILE_LAUNCH:
                level.PlaySound(except, at, SoundEvents::DISPENSER_LAUNCH, SoundSource::Blocks, 1.0f, 1.2f);
                return true;
            case LevelEvent::SOUND_WIND_CHARGE_SHOOT:
                // LevelEventHandler 1051: WIND_CHARGE_THROW, BLOCKS, 0.5,
                // 0.4 / (nextFloat * 0.4 + 0.8).
                level.PlaySound(except, at, SoundEvents::WIND_CHARGE_THROW, SoundSource::Blocks, 0.5f,
                                0.4f / (f() * 0.4f + 0.8f));
                return true;
            case LevelEvent::SOUND_EXTINGUISH_FIRE:
                if (data == 0) {
                    level.PlaySound(except, at, SoundEvents::FIRE_EXTINGUISH, SoundSource::Blocks, 0.5f,
                                    2.6f + spread() * 0.8f);
                } else if (data == 1) {
                    level.PlaySound(except, at, SoundEvents::GENERIC_EXTINGUISH_FIRE, SoundSource::Blocks, 0.7f,
                                    1.6f + spread() * 0.4f);
                }
                return true;
            case LevelEvent::SOUND_ANVIL_BROKEN:
                level.PlaySound(except, at, SoundEvents::ANVIL_DESTROY, SoundSource::Blocks, 1.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_ANVIL_USED:
                level.PlaySound(except, at, SoundEvents::ANVIL_USE, SoundSource::Blocks, 1.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_ANVIL_LAND:
                level.PlaySound(except, at, SoundEvents::ANVIL_LAND, SoundSource::Blocks, 0.3f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_CHORUS_GROW:
                level.PlaySound(except, at, SoundEvents::CHORUS_FLOWER_GROW, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::SOUND_CHORUS_DEATH:
                level.PlaySound(except, at, SoundEvents::CHORUS_FLOWER_DEATH, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::SOUND_BREWING_STAND_BREW:
                level.PlaySound(except, at, SoundEvents::BREWING_STAND_BREW, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::SOUND_GRINDSTONE_USED:
                level.PlaySound(except, at, SoundEvents::GRINDSTONE_USE, SoundSource::Blocks, 1.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_PAGE_TURN:
                level.PlaySound(except, at, SoundEvents::BOOK_PAGE_TURN, SoundSource::Blocks, 1.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_SMITHING_TABLE_USED:
                level.PlaySound(except, at, SoundEvents::SMITHING_TABLE_USE, SoundSource::Blocks, 1.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_POINTED_DRIPSTONE_LAND:
                level.PlaySound(except, at, SoundEvents::POINTED_DRIPSTONE_LAND, SoundSource::Blocks, 2.0f,
                                f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_DRIP_LAVA_INTO_CAULDRON:
                level.PlaySound(except, at, SoundEvents::POINTED_DRIPSTONE_DRIP_LAVA_INTO_CAULDRON, SoundSource::Blocks,
                                2.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_DRIP_WATER_INTO_CAULDRON:
                level.PlaySound(except, at, SoundEvents::POINTED_DRIPSTONE_DRIP_WATER_INTO_CAULDRON, SoundSource::Blocks,
                                2.0f, f() * 0.1f + 0.9f);
                return true;
            case LevelEvent::SOUND_CRAFTER_CRAFT:
                level.PlaySound(except, at, SoundEvents::CRAFTER_CRAFT, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::SOUND_CRAFTER_FAIL:
                level.PlaySound(except, at, SoundEvents::CRAFTER_FAIL, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::LAVA_FIZZ:
                level.PlaySound(except, at, SoundEvents::LAVA_EXTINGUISH, SoundSource::Blocks, 0.5f,
                                2.6f + spread() * 0.8f);
                return true;
            case LevelEvent::REDSTONE_TORCH_BURNOUT:
                level.PlaySound(except, at, SoundEvents::REDSTONE_TORCH_BURNOUT, SoundSource::Blocks, 0.5f,
                                2.6f + spread() * 0.8f);
                return true;
            case LevelEvent::END_PORTAL_FRAME_FILL:
                level.PlaySound(except, at, SoundEvents::END_PORTAL_FRAME_FILL, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::PARTICLES_AND_SOUND_PLANT_GROWTH:
                level.PlaySound(except, at, SoundEvents::BONE_MEAL_USE, SoundSource::Blocks, 1.0f, 1.0f);
                return true;
            case LevelEvent::PARTICLES_DESTROY_BLOCK: {
                // The block-break sound: (volume + 1) / 2, pitch * 0.8.
                const BlockState broken = BlockState::FromRawId(static_cast<uint32_t>(data));
                if (broken.Block() == BlockID::Air) return true;
                const SoundType& type2001 = SoundTypeOf(broken);
                if (!IsEmptySound(type2001.breakSound)) {
                    level.PlaySound(except, at, type2001.breakSound, SoundSource::Blocks,
                                    (type2001.volume + 1.0f) / 2.0f, type2001.pitch * 0.8f);
                }
                return true;
            }
            default:
                return false;
        }
    }

} // namespace Game
