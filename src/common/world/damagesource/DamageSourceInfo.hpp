// File: src/common/world/damagesource/DamageSourceInfo.hpp
//
// MC net.minecraft.world.damagesource.DamageSource, as far as the data-driven
// enchantment effects read one: the damage TYPE (a key into the damage_type
// registry, whose tags — #is_fire, #is_projectile, #bypasses_armor, ... —
// the damage_source_properties condition tests), the CAUSING entity (MC
// getEntity: the shooter, who gets aggro and kill credit) and the DIRECT
// entity (MC getDirectEntity: the arrow that struck).
//
// The engine's damage paths still speak their own enums (MobDamageSource on
// the mob side, ServerPlayer's DamageSource on the player side), so this is
// built at the point an enchantment needs it, from the enum plus the two
// entities the hit carried. The type ids are MC's DamageTypes keys; the tags
// come straight from data/minecraft/tags/damage_type through DataTags.
#pragma once

#include "common/entity/LivingEntity.hpp"   // MobDamageSource

#include <string>
#include <string_view>

namespace Game {

    class Entity;
    struct ItemStack;

    struct DamageSourceInfo {
        // "minecraft:player_attack", "minecraft:arrow", "minecraft:on_fire" ...
        std::string type = "minecraft:generic";
        Entity* causing = nullptr;   // MC getEntity()
        Entity* direct  = nullptr;   // MC getDirectEntity()

        // MC DamageSource.is(TagKey<DamageType>) — "#minecraft:is_fire",
        // "minecraft:is_fire" or "is_fire".
        bool Is(std::string_view tag) const;
        // MC DamageSource.is(ResourceKey<DamageType>).
        bool IsType(std::string_view id) const;

        // MC DamageSource.isDirect: causingEntity == directEntity (true for a
        // melee blow and for environmental damage, where both are null).
        bool IsDirect() const { return causing == direct; }

        // MC DamageSource.getWeaponItem: the DIRECT entity's weapon — the
        // attacker's main hand for a melee hit, the bow an arrow was fired
        // from. Null when the direct entity carries none.
        ItemStack* GetWeaponItem() const;

        // The source a mob-side hit carries. `direct` is the entity that
        // physically struck (HurtFrom's direct entity); null means the hit
        // came straight from `causing` — a melee blow, where MC's direct and
        // causing entities are the same object.
        static DamageSourceInfo Of(MobDamageSource source, Entity* causing, Entity* direct);

        // The MC damage type key an engine source stands for. A projectile's
        // type depends on what flew (an arrow, a trident, a fireball — MC
        // DamageSources.arrow / trident / fireball / thrown / ...), and an
        // explosion set off by a player is player_explosion.
        static std::string_view TypeIdFor(MobDamageSource source, const Entity* causing,
                                          const Entity* direct);
    };

} // namespace Game
