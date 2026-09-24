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
        inline constexpr int LAVA_FIZZ                    = 1501;
        inline constexpr int REDSTONE_TORCH_BURNOUT       = 1502;
        inline constexpr int END_PORTAL_FRAME_FILL        = 1503;
        inline constexpr int PARTICLES_AND_SOUND_PLANT_GROWTH = 1505;
        inline constexpr int PARTICLES_DESTROY_BLOCK      = 2001;
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
    // does. Players in other dimensions do not hear it; vanilla's
    // global_sound_events rule is treated as its default (true).
    template <class LevelT>
    bool PlayGlobalLevelEventSound(LevelT& level, int type, const glm::ivec3& pos) {
        constexpr float kGlobalVolume = 10000.0f;
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
    template <class LevelT>
    bool PlayLevelEventSound(LevelT& level, const SoundExcept& except, int type,
                             const glm::ivec3& pos, int data, JavaRandom* random) {
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
