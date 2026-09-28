// File: src/common/entity/mobs/WolfVariants.hpp
//
// MC 26.3 world/entity/animal/wolf/WolfVariant(s) + WolfSoundVariant(s), and
// the Crackiness.WOLF_ARMOR thresholds the wolf's armour layer and crack
// sound share.
//
// ── Coat variants (data/minecraft/wolf_variant/*.json) ─────────────────────
// Nine coats, each with a wild / tame / angry sheet (and, under the 26.x baby
// look, the `_baby` twins RemodelTexturePath maps to). The spawn conditions
// are MC's SpawnPrioritySelectors: pale is the priority-0 fallback that
// always matches; every other coat is a single priority-1 biome check. MC's
// PriorityProvider.pick keeps every matching entry of the highest matched
// priority and then draws Util.getRandomSafe — ONE nextInt(size) even when a
// single entry survives, which is part of the finalizeSpawn RNG stream.
//
// The enum order is WolfVariants.bootstrap's; it is what the wire's variant
// byte carries. The registry the pick walks is the datapack's, which the
// loader lists in identifier order (ashen, black, chestnut, pale, …) — that
// order decides which of several equal-priority matches an index lands on,
// so the pick walks that order, not the enum's.
//
// ── Sound variants (data/minecraft/wolf_sound_variant/*.json) ──────────────
// MC 26.3's seven sound sets — classic, puglin, sad, angry, grumpy, big and
// cute — in WolfSoundVariants.bootstrap's registration order, which is the
// enum order, the wire value and the note-block cycle order (Next).
// pickRandomSoundVariant is registry.getRandom — one nextInt(7) over the
// datapack registry's listing. A saved key the registry does not hold keeps
// the default (classic), as MC's readAdditionalSaveData does.
//
// A baby wolf uses the shared baby set whatever its variant
// (SoundEvents.registerWolfSoundVariants: every variant's babySounds is the
// same WOLF_*_BABY row); the step sound is WOLF_STEP / WOLF_STEP_BABY for all.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Game {

    class JavaRandom;

    namespace WolfVariants {

        enum class Variant : uint8_t {
            Pale = 0, Spotted, Snowy, Black, Ashen, Rusty, Woods, Chestnut, Striped,
        };
        inline constexpr uint8_t kCount = 9;
        // MC WolfVariants.DEFAULT.
        inline constexpr Variant kDefault = Variant::Pale;

        // "pale", "spotted", … — the registry path (no namespace).
        const char* Name(Variant v);
        // Accepts "minecraft:pale" or "pale"; false for anything unknown.
        bool FromName(std::string_view id, Variant& out);

        // The coat's asset base under textures/entity/wolf/ ("wolf",
        // "wolf_spotted", …) — WolfVariants.register's fileName.
        const char* TextureBase(Variant v);

        // MC Wolf.getTexture's pick, adult sheet: tame beats angry beats wild.
        // The baby sheet is the `_baby` twin RemodelTexturePath maps to.
        std::string TexturePath(Variant v, bool tame, bool angry);

        // MC VariantUtils.selectVariantToSpawn(SpawnContext.create(level,
        // blockPosition()), WOLF_VARIANT) for the biome the wolf stands in.
        Variant SelectToSpawn(std::string_view biome, JavaRandom& random);

    } // namespace WolfVariants

    namespace WolfSoundVariants {

        // MC WolfSoundVariants.SoundSet / bootstrap order.
        enum class SoundVariant : uint8_t {
            Classic = 0, Puglin, Sad, Angry, Grumpy, Big, Cute,
        };
        inline constexpr uint8_t kCount = 7;

        // MC WolfSoundVariant.WolfSoundSet (step sound excepted — it is the
        // same for every set and rides the entity sound table).
        struct SoundSet {
            const char* ambient;
            const char* death;
            const char* growl;
            const char* hurt;
            const char* pant;
            const char* whine;
        };

        // adultSounds() / babySounds().
        const SoundSet& Sounds(SoundVariant v, bool baby);

        const char* Name(SoundVariant v);
        bool FromName(std::string_view id, SoundVariant& out);

        // MC WolfSoundVariants.pickRandomSoundVariant.
        SoundVariant PickRandom(JavaRandom& random);

        // The engine's note-block cycle: the next set in registration order,
        // wrapping cute → classic.
        SoundVariant Next(SoundVariant v);

    } // namespace WolfSoundVariants

    // MC Crackiness.WOLF_ARMOR (0.95 / 0.69 / 0.32) — the crack overlay the
    // armour layer draws and the WOLF_ARMOR_CRACK threshold the wolf plays.
    enum class WolfArmorCrackiness : uint8_t { None = 0, Low, Medium, High };

    inline WolfArmorCrackiness WolfArmorCrackinessByDamage(int damage, int maxDamage) {
        if (maxDamage <= 0) return WolfArmorCrackiness::None;
        const float fraction = static_cast<float>(maxDamage - damage) /
                               static_cast<float>(maxDamage);
        if (fraction < 0.32f) return WolfArmorCrackiness::High;
        if (fraction < 0.69f) return WolfArmorCrackiness::Medium;
        if (fraction < 0.95f) return WolfArmorCrackiness::Low;
        return WolfArmorCrackiness::None;
    }

} // namespace Game
