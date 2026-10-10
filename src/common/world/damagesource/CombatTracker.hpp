// File: src/common/world/damagesource/CombatTracker.hpp
//
// MC net.minecraft.world.damagesource.CombatTracker (+ CombatEntry and
// FallLocation): the list of recent hits a LivingEntity took, from which
// its death message is written — "Fluffy was slain by Steve", "Bob fell
// from a high place", "Rex was doomed to fall by Zombie".
//
// Kept on every LivingEntity (LivingEntity::GetCombatTracker) and fed by the
// server's damage path (LivingEntity::ActuallyHurt records a hit once armour,
// effects and absorption have had their say — MC actuallyHurt's
// recordDamage). The player keeps its own message path (ServerPlayer's
// BuildDeathMessage); this one speaks for mobs: a named entity's death is
// announced to every player, a tamed pet's to its owner (MC
// TamableAnimal.die).
//
// Unlike MC's CombatEntry, which holds the DamageSource and so the live
// causing/direct entities, an entry here records the NAMES it will need when
// it is recorded: the attacker may well be gone (despawned, unloaded, dead)
// by the time the victim dies, and a dangling pointer is not a name.
//
// The tracker holds no pointer to its owner — every call is handed the
// entity — so a LivingEntity stays freely movable.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace Game {

    class Entity;
    struct EntityLevel;
    class LivingEntity;

    // MC FallLocation — where a fall started, for "fell off a ladder".
    enum class FallLocation : uint8_t {
        Generic,
        Ladder,
        Vines,
        WeepingVines,
        TwistingVines,
        Scaffolding,
        OtherClimbable,
        Water,
    };

    // MC FallLocation.languageKey: "death.fell.accident.<id>".
    std::string_view FallLocationLanguageKey(FallLocation location);

    // MC Entity.getDisplayName().getString(): a player's name, else the
    // custom name, else the type's translated name ("Zombie"). `level` is
    // what can name a player (EntityLevel::GetPlayerName); may be null.
    std::string EntityDisplayName(const Entity& entity, const EntityLevel* level);

    // MC EntityType.getDescription().getString() for a registry slug:
    // en_us "entity.minecraft.<slug>", else the slug title-cased
    // ("zephyr" -> "Zephyr", "slime_beast" -> "Slime Beast").
    std::string EntityTypeDisplayName(std::string_view slug);

    class CombatTracker {
    public:
        // MC CombatTracker.RESET_DAMAGE_STATUS_TIME / RESET_COMBAT_STATUS_TIME.
        static constexpr int kResetDamageStatusTime = 100;
        static constexpr int kResetCombatStatusTime = 300;

        // MC recordDamage. `typeId` is the MC damage type key
        // (DamageSourceInfo::TypeIdFor — "minecraft:mob_attack", ...);
        // `causing` / `direct` are MC's getEntity / getDirectEntity (either
        // may be null; a melee blow passes the attacker as both).
        void RecordDamage(LivingEntity& mob, std::string_view typeId, float damage,
                          Entity* causing, Entity* direct);

        // MC getDeathMessage().getString().
        std::string GetDeathMessage(LivingEntity& mob) const;

        // MC recheckStatus: forget the fight once the mob is dead, or has not
        // been hurt for 5 s (15 s while in combat).
        void RecheckStatus(const LivingEntity& mob);

        // MC LivingEntity.onClimbable's lastClimbablePos bookkeeping and
        // checkFallDamage's reset on landing, run once per server tick for a
        // mob: inside a #climbable block (or an open trapdoor over a ladder
        // facing the same way) remembers the cell; standing on the ground
        // forgets it.
        void TrackClimbable(const LivingEntity& mob);

        bool IsInCombat() const { return m_inCombat; }
        bool IsTakingDamage() const { return m_takingDamage; }
        int  GetCombatDuration(const LivingEntity& mob) const;

    private:
        // MC CombatEntry(source, damage, fallLocation, fallDistance), with
        // the source reduced to what the message reads off it.
        struct Entry {
            std::string typeId;                          // damage type key
            float damage = 0.0f;
            std::optional<FallLocation> fallLocation;    // MC's nullable fallLocation
            float fallDistance = 0.0f;
            std::optional<std::string> causingName;      // getEntity().getDisplayName()
            std::optional<std::string> directName;       // getDirectEntity().getDisplayName()
            // The causing LivingEntity's main-hand stack's display name
            // ("[Excalibur]") when it carries a CUSTOM_NAME — MC's ".item"
            // message variants.
            std::optional<std::string> causingItemName;
        };

        std::string LocalizedDeathMessage(LivingEntity& mob, const Entry& killingBlow) const;
        std::string FallMessage(const LivingEntity& mob, const Entry& knockOff,
                                const Entry& killingBlow) const;
        const Entry* MostSignificantFall() const;

        std::vector<Entry> m_entries;
        int  m_lastDamageTime = 0;
        int  m_combatStartTime = 0;
        int  m_combatEndTime = 0;
        bool m_inCombat = false;
        bool m_takingDamage = false;
        // MC LivingEntity.lastClimbablePos.
        std::optional<glm::ivec3> m_lastClimbablePos;
    };

} // namespace Game
