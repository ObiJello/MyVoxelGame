// File: src/common/entity/ai/brain/Activity.hpp
//
// MC net.minecraft.world.entity.schedule.Activity — the coarse mode a brain is
// in. A behaviour belongs to exactly one, and only behaviours in an ACTIVE
// activity are ever started.
//
// CORE is special: it is always active, alongside whichever single non-core
// activity the brain has selected. That is why LookAtTargetSink and
// MoveToTargetSink live in CORE — they must keep running whatever the mob is
// otherwise doing, and a mob that stopped looking and walking every time it
// switched from IDLE to PANIC would visibly stutter.
#pragma once

#include <cstdint>
#include <string_view>

namespace Game {

    // MC's full list, in declaration order.
    enum class Activity : uint8_t {
        Core, Idle, Work, Play, Rest, Meet, Panic, Raid, PreRaid, Hide, Fight,
        Celebrate, AdmireItem, Avoid, Ride, PlayDead, LongJump, Ram, Tongue,
        Swim, LaySpawn, Sniff, Investigate, Roar, Emerge, Dig,
        Count,
    };

    inline std::string_view ActivityName(Activity a) {
        switch (a) {
            case Activity::Core:        return "core";
            case Activity::Idle:        return "idle";
            case Activity::Work:        return "work";
            case Activity::Play:        return "play";
            case Activity::Rest:        return "rest";
            case Activity::Meet:        return "meet";
            case Activity::Panic:       return "panic";
            case Activity::Raid:        return "raid";
            case Activity::PreRaid:     return "pre_raid";
            case Activity::Hide:        return "hide";
            case Activity::Fight:       return "fight";
            case Activity::Celebrate:   return "celebrate";
            case Activity::AdmireItem:  return "admire_item";
            case Activity::Avoid:       return "avoid";
            case Activity::Ride:        return "ride";
            case Activity::PlayDead:    return "play_dead";
            case Activity::LongJump:    return "long_jump";
            case Activity::Ram:         return "ram";
            case Activity::Tongue:      return "tongue";
            case Activity::Swim:        return "swim";
            case Activity::LaySpawn:    return "lay_spawn";
            case Activity::Sniff:       return "sniff";
            case Activity::Investigate: return "investigate";
            case Activity::Roar:        return "roar";
            case Activity::Emerge:      return "emerge";
            case Activity::Dig:         return "dig";
            default:                    return "?";
        }
    }

} // namespace Game
