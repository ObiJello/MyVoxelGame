// File: src/common/entity/mobs/FarmSoundVariants.hpp
//
// MC 26.3 PigSoundVariants / CowSoundVariants / ChickenSoundVariants /
// CatSoundVariants: each a registry of sound sets ("classic" plus pig
// mini/big, cow moody, chicken picky, cat royal), picked at random in
// finalizeSpawn (registry.getRandom — uniform in bootstrap order), saved as
// "sound_variant" (minecraft:<name>) and set by the <mob>/sound_variant item
// component. A variant's adult sounds are entity.<sound event id>.<kind>
// (SoundEvents.register<Mob>SoundVariants); every variant shares the baby
// sounds (the cow has no baby set — its variant applies to calves too) and
// the pig's / chicken's step sound.
#pragma once

#include <cstdint>
#include <string_view>

namespace Game::FarmSoundVariants {

    enum class Mob : uint8_t { Pig, Cow, Chicken, Cat };

    // Bootstrap order (the registry's order).
    inline constexpr const char* kPigNames[]     = {"classic", "big", "mini"};
    inline constexpr const char* kCowNames[]     = {"classic", "moody"};
    inline constexpr const char* kChickenNames[] = {"classic", "picky"};
    inline constexpr const char* kCatNames[]     = {"classic", "royal"};

    inline uint8_t Count(Mob mob) {
        switch (mob) {
            case Mob::Pig: return 3;
            case Mob::Cow: case Mob::Chicken: case Mob::Cat: return 2;
        }
        return 1;
    }

    inline const char* Name(Mob mob, uint8_t id) {
        const uint8_t n = Count(mob);
        if (id >= n) id = 0;
        switch (mob) {
            case Mob::Pig:     return kPigNames[id];
            case Mob::Cow:     return kCowNames[id];
            case Mob::Chicken: return kChickenNames[id];
            case Mob::Cat:     return kCatNames[id];
        }
        return "classic";
    }

    // "minecraft:<name>" or "<name>"; false when the registry has no such entry.
    inline bool FromName(Mob mob, std::string_view name, uint8_t& out) {
        if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
        for (uint8_t i = 0; i < Count(mob); ++i) {
            if (name == Name(mob, i)) { out = i; return true; }
        }
        return false;
    }

} // namespace Game::FarmSoundVariants
