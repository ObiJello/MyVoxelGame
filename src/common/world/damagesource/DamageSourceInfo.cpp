// File: src/common/world/damagesource/DamageSourceInfo.cpp
#include "DamageSourceInfo.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/world/tags/DataTags.hpp"

namespace Game {

    bool DamageSourceInfo::Is(std::string_view tag) const {
        return DataTags::HasTag(DataTags::Registry::DamageType, type, tag);
    }

    bool DamageSourceInfo::IsType(std::string_view id) const {
        constexpr std::string_view kPrefix = "minecraft:";
        if (id.find(':') == std::string_view::npos) {
            return type.size() == kPrefix.size() + id.size() &&
                   std::string_view(type).substr(0, kPrefix.size()) == kPrefix &&
                   std::string_view(type).substr(kPrefix.size()) == id;
        }
        return type == id;
    }

    ItemStack* DamageSourceInfo::GetWeaponItem() const {
        return direct ? direct->GetWeaponItem() : nullptr;
    }

    DamageSourceInfo DamageSourceInfo::Of(MobDamageSource source, Entity* causing, Entity* direct) {
        DamageSourceInfo out;
        // A hit that named no separate direct entity came straight from its
        // causing entity (MC's melee sources pass the attacker as both).
        out.causing = causing;
        out.direct  = direct ? direct : causing;
        out.type    = std::string(TypeIdFor(source, out.causing, out.direct));
        return out;
    }

    std::string_view DamageSourceInfo::TypeIdFor(MobDamageSource source, const Entity* causing,
                                                 const Entity* direct) {
        switch (source) {
            case MobDamageSource::Generic:           return "minecraft:generic";
            case MobDamageSource::MobAttack:         return "minecraft:mob_attack";
            case MobDamageSource::PlayerAttack:      return "minecraft:player_attack";
            case MobDamageSource::Fall:              return "minecraft:fall";
            // The on-fire tick; the fire BLOCK (in_fire) reaches entities
            // through the same source here, and the two share every tag an
            // enchantment reads (#is_fire, the knockback and armor tags).
            case MobDamageSource::Fire:              return "minecraft:on_fire";
            case MobDamageSource::Lava:              return "minecraft:lava";
            case MobDamageSource::Drown:             return "minecraft:drown";
            // DamageSources.explosion(source, attacker): player_explosion
            // when a player set it off.
            case MobDamageSource::Explosion:
                return causing && causing->IsPlayer() ? "minecraft:player_explosion"
                                                      : "minecraft:explosion";
            case MobDamageSource::Void:              return "minecraft:out_of_world";
            // DamageSources.magic() for a tick; indirectMagic(source, owner)
            // when a thrower is credited (a splash of harming).
            case MobDamageSource::Magic:
                return causing ? "minecraft:indirect_magic" : "minecraft:magic";
            case MobDamageSource::Wither:            return "minecraft:wither";
            case MobDamageSource::Cramming:          return "minecraft:cramming";
            case MobDamageSource::FallingBlock:      return "minecraft:falling_block";
            case MobDamageSource::FallingAnvil:      return "minecraft:falling_anvil";
            case MobDamageSource::FallingStalactite: return "minecraft:falling_stalactite";
            case MobDamageSource::Stalagmite:        return "minecraft:stalagmite";
            case MobDamageSource::Thorns:            return "minecraft:thorns";
            case MobDamageSource::Projectile:
                break;
        }

        // A projectile: what flew decides the type (DamageSources.arrow,
        // trident, fireball, witherSkull, windCharge, thrown, mobProjectile).
        if (!direct) return "minecraft:mob_projectile";
        const std::string_view slug = direct->TypeInfo().slug;
        if (slug == "trident") return "minecraft:trident";
        if (dynamic_cast<const Arrow*>(direct)) return "minecraft:arrow";
        if (slug == "small_fireball" || slug == "fireball") {
            // DamageSources.fireball(fireball, owner): unattributed without
            // an owner.
            return causing && causing != direct ? "minecraft:fireball"
                                                : "minecraft:unattributed_fireball";
        }
        if (slug == "wither_skull") return "minecraft:wither_skull";
        if (slug == "wind_charge" || slug == "breeze_wind_charge") return "minecraft:wind_charge";
        if (slug == "snowball" || slug == "egg" || slug == "ender_pearl" ||
            slug == "splash_potion" || slug == "lingering_potion") {
            return "minecraft:thrown";
        }
        return "minecraft:mob_projectile";
    }

} // namespace Game
