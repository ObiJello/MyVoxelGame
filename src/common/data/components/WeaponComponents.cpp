// File: src/common/data/components/WeaponComponents.cpp
#include "WeaponComponents.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Game {

    // ── SwingAnimationType ─────────────────────────────────────────────────

    std::string_view SwingAnimationTypeName(SwingAnimationType type) {
        switch (type) {
            case SwingAnimationType::None:  return "none";
            case SwingAnimationType::Whack: return "whack";
            case SwingAnimationType::Stab:  return "stab";
        }
        return "whack";
    }

    bool SwingAnimationTypeFromName(std::string_view name, SwingAnimationType& out) {
        if (name == "none")  { out = SwingAnimationType::None;  return true; }
        if (name == "whack") { out = SwingAnimationType::Whack; return true; }
        if (name == "stab")  { out = SwingAnimationType::Stab;  return true; }
        return false;
    }

    // ── AttackRange ───────────────────────────────────────────────────────

    float AttackRange::EffectiveMinRange(const Entity& entity) const {
        if (entity.IsPlayer()) return entity.IsCreative() ? minCreativeReach : minReach;
        return minReach * mobFactor;
    }

    float AttackRange::EffectiveMaxRange(const Entity& entity) const {
        if (entity.IsPlayer()) return entity.IsCreative() ? maxCreativeReach : maxReach;
        return maxReach * mobFactor;
    }

    namespace {
        bool InRange(const AttackRange& r, const LivingEntity& attacker, double distanceSqr, double extraBuffer) {
            const double distance = std::sqrt(distanceSqr);
            const double minReach = static_cast<double>(r.EffectiveMinRange(attacker) - r.hitboxMargin) - extraBuffer;
            const double maxReach = static_cast<double>(r.EffectiveMaxRange(attacker) + r.hitboxMargin) + extraBuffer;
            return distance >= minReach && distance <= maxReach;
        }
    }

    bool AttackRange::IsInRange(const LivingEntity& attacker, const glm::dvec3& location) const {
        const glm::dvec3 d = location - attacker.GetEyePosition();
        return InRange(*this, attacker, glm::dot(d, d), 0.0);
    }

    bool AttackRange::IsInRange(const LivingEntity& attacker, const AABBd& box, double extraBuffer) const {
        return InRange(*this, attacker, box.DistanceToSqr(attacker.GetEyePosition()), extraBuffer);
    }

    // ── Readers ───────────────────────────────────────────────────────────

    SwingAnimation GetAttackAnimation(const ItemStack& stack) {
        if (stack.IsEmpty()) return SwingAnimation::Default();
        return stack.get(DataComponents::ATTACK_ANIMATION).value_or(SwingAnimation::Default());
    }

    SwingAnimation GetInteractAnimation(const ItemStack& stack) {
        if (stack.IsEmpty()) return SwingAnimation::Default();
        return stack.get(DataComponents::INTERACT_ANIMATION).value_or(SwingAnimation::Default());
    }

    SwingAnimation GetAttackAnimation(ItemID item) {
        if (item == Items::Air) return SwingAnimation::Default();
        return ItemRegistry::Get(item).defaultComponents.get(DataComponents::ATTACK_ANIMATION)
            .value_or(SwingAnimation::Default());
    }

    float GetMinimumAttackCharge(const ItemStack& stack) {
        if (stack.IsEmpty()) return 0.0f;
        return stack.get(DataComponents::MINIMUM_ATTACK_CHARGE).value_or(0.0f);
    }

    bool CannotAttackWithItem(const ItemStack& stack, float strengthWithTolerance) {
        const float required = GetMinimumAttackCharge(stack);
        return required > 0.0f && strengthWithTolerance < required;
    }

    std::optional<std::string> GetItemDamageType(const ItemStack& stack) {
        if (stack.IsEmpty()) return std::nullopt;
        return stack.get(DataComponents::DAMAGE_TYPE);
    }

    MobDamageSource MobDamageSourceForType(std::string_view id, MobDamageSource fallback) {
        if (id.rfind("minecraft:", 0) == 0) id.remove_prefix(10);
        struct Row { std::string_view type; MobDamageSource source; };
        static constexpr Row kRows[] = {
            { "generic",            MobDamageSource::Generic },
            { "generic_kill",       MobDamageSource::Generic },
            { "mob_attack",         MobDamageSource::MobAttack },
            { "mob_attack_no_aggro",MobDamageSource::MobAttack },
            { "player_attack",      MobDamageSource::PlayerAttack },
            { "arrow",              MobDamageSource::Projectile },
            { "trident",            MobDamageSource::Projectile },
            { "mob_projectile",     MobDamageSource::Projectile },
            { "thrown",             MobDamageSource::Projectile },
            { "fireball",           MobDamageSource::Projectile },
            { "unattributed_fireball", MobDamageSource::Projectile },
            { "wither_skull",       MobDamageSource::Projectile },
            { "wind_charge",        MobDamageSource::Projectile },
            { "fall",               MobDamageSource::Fall },
            { "on_fire",            MobDamageSource::Fire },
            { "in_fire",            MobDamageSource::Fire },
            { "campfire",           MobDamageSource::Campfire },
            { "hot_floor",          MobDamageSource::HotFloor },
            { "cactus",             MobDamageSource::Cactus },
            { "sweet_berry_bush",   MobDamageSource::SweetBerryBush },
            { "lava",               MobDamageSource::Lava },
            { "drown",              MobDamageSource::Drown },
            { "explosion",          MobDamageSource::Explosion },
            { "player_explosion",   MobDamageSource::Explosion },
            { "bad_respawn_point",  MobDamageSource::Explosion },
            { "out_of_world",       MobDamageSource::Void },
            { "magic",              MobDamageSource::Magic },
            { "indirect_magic",     MobDamageSource::Magic },
            { "wither",             MobDamageSource::Wither },
            { "cramming",           MobDamageSource::Cramming },
            { "falling_block",      MobDamageSource::FallingBlock },
            { "falling_anvil",      MobDamageSource::FallingAnvil },
            { "falling_stalactite", MobDamageSource::FallingStalactite },
            { "stalagmite",         MobDamageSource::Stalagmite },
            { "thorns",             MobDamageSource::Thorns },
            { "mace_smash",         MobDamageSource::MaceSmash },
            { "fireworks",          MobDamageSource::Fireworks },
            { "fly_into_wall",      MobDamageSource::FlyIntoWall },
            { "lightning_bolt",     MobDamageSource::Lightning },
            { "spear",              MobDamageSource::Spear },
        };
        for (const Row& r : kRows) if (r.type == id) return r.source;
        return fallback;
    }

    MobDamageSource ItemMeleeDamageSource(const ItemStack& weapon, MobDamageSource fallback) {
        if (auto type = GetItemDamageType(weapon)) return MobDamageSourceForType(*type, fallback);
        return fallback;
    }

    bool DamageTypeHolderSetContains(const std::vector<std::string>& set, std::string_view typeId) {
        std::string_view bareType = typeId;
        if (bareType.rfind("minecraft:", 0) == 0) bareType.remove_prefix(10);
        for (const std::string& entry : set) {
            if (entry.empty()) continue;
            if (entry[0] == '#') {
                const std::string full = typeId.find(':') == std::string_view::npos
                    ? "minecraft:" + std::string(typeId) : std::string(typeId);
                if (DataTags::HasTag(DataTags::Registry::DamageType, full, entry)) return true;
                continue;
            }
            std::string_view e(entry);
            if (e.rfind("minecraft:", 0) == 0) e.remove_prefix(10);
            if (e == bareType) return true;
        }
        return false;
    }

    // ── BLOCKS_ATTACKS math ───────────────────────────────────────────────

    float ResolveBlockedDamage(const BlocksAttacks& blocks, std::string_view damageTypeId,
                               float dealtDamage, double angle) {
        float blocked = 0.0f;
        for (const BlocksAttacks::DamageReduction& r : blocks.damageReductions) {
            // DamageReduction.resolve.
            if (angle > static_cast<double>(0.017453292f * r.horizontalBlockingAngle)) continue;
            if (!r.type.empty() && !DamageTypeHolderSetContains(r.type, damageTypeId)) continue;
            blocked += std::clamp(r.base + r.factor * dealtDamage, 0.0f, dealtDamage);
        }
        return std::clamp(blocked, 0.0f, dealtDamage);
    }

    int BlockingItemDamage(const BlocksAttacks& blocks, float blockedDamage) {
        const BlocksAttacks::ItemDamageFunction& f = blocks.itemDamage;
        if (blockedDamage < f.threshold) return 0;
        return static_cast<int>(std::floor(f.base + f.factor * blockedDamage));
    }

    int DisableBlockingForTicks(const BlocksAttacks& blocks, float baseSeconds) {
        const float seconds = baseSeconds * blocks.disableCooldownScale;
        return seconds > 0.0f ? static_cast<int>(std::floor(seconds * 20.0f + 0.5f)) : 0;
    }

    bool BlocksAttacksBypassedBy(const BlocksAttacks& blocks, std::string_view damageTypeId) {
        return !blocks.bypassedBy.empty() && DamageTypeHolderSetContains(blocks.bypassedBy, damageTypeId);
    }

    // ── The spears (Item.Properties.spear) ────────────────────────────────

    void ItemRegistry_RegisterWeaponComponents(std::unordered_map<ItemID, Item>& pureItems) {
        struct SpearRow {
            ItemID item;
            bool   wood;
            float  attackDuration, damageMultiplier, delay, dismountTime, dismountThreshold,
                   knockbackTime, knockbackThreshold, damageTime, damageThreshold;
        };
        // Items.java: the seven spear(...) registrations, number for number.
        const SpearRow rows[] = {
            { Items::WoodenSpear,    true,  0.65f, 0.7f,   0.75f, 5.0f, 14.0f, 10.0f,  5.1f, 15.0f,  4.6f },
            { Items::StoneSpear,     false, 0.75f, 0.82f,  0.7f,  4.5f, 13.0f, 9.0f,   5.1f, 13.75f, 4.6f },
            { Items::CopperSpear,    false, 0.85f, 0.82f,  0.65f, 4.0f, 12.0f, 8.25f,  5.1f, 12.5f,  4.6f },
            { Items::IronSpear,      false, 0.95f, 0.95f,  0.6f,  2.5f, 11.0f, 6.75f,  5.1f, 11.25f, 4.6f },
            { Items::GoldenSpear,    false, 0.95f, 0.7f,   0.7f,  3.5f, 13.0f, 8.5f,   5.1f, 13.75f, 4.6f },
            { Items::DiamondSpear,   false, 1.05f, 1.075f, 0.5f,  3.0f, 10.0f, 6.5f,   5.1f, 10.0f,  4.6f },
            { Items::NetheriteSpear, false, 1.15f, 1.2f,   0.4f,  2.5f, 9.0f,  5.5f,   5.1f, 8.75f,  4.6f },
        };
        // The seconds become ticks as Java does it — (int)(x * 20.0F).
        const auto ticks = [](float seconds) { return static_cast<int>(seconds * 20.0f); };
        int registered = 0;
        for (const SpearRow& row : rows) {
            auto it = pureItems.find(row.item);
            if (it == pureItems.end()) continue;
            DataComponentMap& c = it->second.defaultComponents;

            // .delayedHolderComponent(DAMAGE_TYPE, DamageTypes.SPEAR)
            c.set(DataComponents::DAMAGE_TYPE, std::string("minecraft:spear"));

            // KineticWeapon(10, delay, ofAttackerSpeed(dismount…),
            // ofAttackerSpeed(knockback…), ofRelativeSpeed(damage…), 0.38,
            // damageMultiplier, USE sound, HIT sound).
            KineticWeapon k;
            k.contactCooldownTicks = 10;
            k.delayTicks = ticks(row.delay);
            k.dismountConditions  = KineticWeapon::Condition{ ticks(row.dismountTime), row.dismountThreshold, 0.0f };
            k.knockbackConditions = KineticWeapon::Condition{ ticks(row.knockbackTime), row.knockbackThreshold, 0.0f };
            k.damageConditions    = KineticWeapon::Condition{ ticks(row.damageTime), 0.0f, row.damageThreshold };
            k.forwardMovement  = 0.38f;
            k.damageMultiplier = row.damageMultiplier;
            k.sound    = row.wood ? SoundEvents::SPEAR_WOOD_USE : SoundEvents::SPEAR_USE;
            k.hitSound = row.wood ? SoundEvents::SPEAR_WOOD_HIT : SoundEvents::SPEAR_HIT;
            c.set(DataComponents::KINETIC_WEAPON, std::move(k));

            // PiercingWeapon(true, false, ATTACK sound, HIT sound).
            PiercingWeapon p;
            p.dealsKnockback = true;
            p.dismounts = false;
            p.sound    = row.wood ? SoundEvents::SPEAR_WOOD_ATTACK : SoundEvents::SPEAR_ATTACK;
            p.hitSound = row.wood ? SoundEvents::SPEAR_WOOD_HIT : SoundEvents::SPEAR_HIT;
            c.set(DataComponents::PIERCING_WEAPON, std::move(p));

            // AttackRange(2.0, 4.5, 2.0, 6.5, 0.125, 0.5).
            c.set(DataComponents::ATTACK_RANGE, AttackRange{ 2.0f, 4.5f, 2.0f, 6.5f, 0.125f, 0.5f });
            c.set(DataComponents::MINIMUM_ATTACK_CHARGE, 1.0f);
            // SwingAnimation(STAB, (int)(attackDuration * 20)).
            c.set(DataComponents::ATTACK_ANIMATION, SwingAnimation{ SwingAnimationType::Stab, ticks(row.attackDuration) });
            ++registered;
        }
        Log::Info("[ItemRegistry] Registered weapon components on %d spears", registered);
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {

        void WriteOptionalCondition(Network::PacketBuffer& b, const std::optional<KineticWeapon::Condition>& c) {
            b.WriteByte(c ? 1 : 0);
            if (!c) return;
            b.WriteVarInt(static_cast<uint32_t>(c->maxDurationTicks));
            b.WriteFloat(c->minSpeed);
            b.WriteFloat(c->minRelativeSpeed);
        }

        std::optional<KineticWeapon::Condition> ReadOptionalCondition(Network::PacketReader& r) {
            if (r.ReadByte() == 0) return std::nullopt;
            KineticWeapon::Condition c;
            c.maxDurationTicks = static_cast<int>(r.ReadVarInt());
            c.minSpeed = r.ReadFloat();
            c.minRelativeSpeed = r.ReadFloat();
            return c;
        }

        // Optional<Holder<SoundEvent>>: present flag + id.
        void WriteOptionalSound(Network::PacketBuffer& b, const std::string& sound) {
            b.WriteByte(sound.empty() ? 0 : 1);
            if (!sound.empty()) b.WriteString(sound);
        }

        std::string ReadOptionalSound(Network::PacketReader& r) {
            return r.ReadByte() != 0 ? r.ReadString() : std::string();
        }

        // KineticWeapon.STREAM_CODEC.
        void SerKinetic(Network::PacketBuffer& b, const KineticWeapon& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.contactCooldownTicks));
            b.WriteVarInt(static_cast<uint32_t>(v.delayTicks));
            WriteOptionalCondition(b, v.dismountConditions);
            WriteOptionalCondition(b, v.knockbackConditions);
            WriteOptionalCondition(b, v.damageConditions);
            b.WriteFloat(v.forwardMovement);
            b.WriteFloat(v.damageMultiplier);
            WriteOptionalSound(b, v.sound);
            WriteOptionalSound(b, v.hitSound);
        }

        KineticWeapon DeKinetic(Network::PacketReader& r) {
            KineticWeapon v;
            v.contactCooldownTicks = static_cast<int>(r.ReadVarInt());
            v.delayTicks = static_cast<int>(r.ReadVarInt());
            v.dismountConditions  = ReadOptionalCondition(r);
            v.knockbackConditions = ReadOptionalCondition(r);
            v.damageConditions    = ReadOptionalCondition(r);
            v.forwardMovement  = r.ReadFloat();
            v.damageMultiplier = r.ReadFloat();
            v.sound    = ReadOptionalSound(r);
            v.hitSound = ReadOptionalSound(r);
            return v;
        }

        // PiercingWeapon.STREAM_CODEC.
        void SerPiercing(Network::PacketBuffer& b, const PiercingWeapon& v) {
            b.WriteByte(v.dealsKnockback ? 1 : 0);
            b.WriteByte(v.dismounts ? 1 : 0);
            WriteOptionalSound(b, v.sound);
            WriteOptionalSound(b, v.hitSound);
        }

        PiercingWeapon DePiercing(Network::PacketReader& r) {
            PiercingWeapon v;
            v.dealsKnockback = r.ReadByte() != 0;
            v.dismounts = r.ReadByte() != 0;
            v.sound    = ReadOptionalSound(r);
            v.hitSound = ReadOptionalSound(r);
            return v;
        }

        // AttackRange.STREAM_CODEC: six floats.
        void SerAttackRange(Network::PacketBuffer& b, const AttackRange& v) {
            b.WriteFloat(v.minReach);
            b.WriteFloat(v.maxReach);
            b.WriteFloat(v.minCreativeReach);
            b.WriteFloat(v.maxCreativeReach);
            b.WriteFloat(v.hitboxMargin);
            b.WriteFloat(v.mobFactor);
        }

        AttackRange DeAttackRange(Network::PacketReader& r) {
            AttackRange v;
            v.minReach = r.ReadFloat();
            v.maxReach = r.ReadFloat();
            v.minCreativeReach = r.ReadFloat();
            v.maxCreativeReach = r.ReadFloat();
            v.hitboxMargin = r.ReadFloat();
            v.mobFactor = r.ReadFloat();
            return v;
        }

        void SerChargeFloat(Network::PacketBuffer& b, const float& v) { b.WriteFloat(v); }
        float DeChargeFloat(Network::PacketReader& r) { return r.ReadFloat(); }

        void SerTypeId(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeTypeId(Network::PacketReader& r) { return r.ReadString(); }

        // SwingAnimation.STREAM_CODEC: type id, VarInt duration.
        void SerSwing(Network::PacketBuffer& b, const SwingAnimation& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.type));
            b.WriteVarInt(static_cast<uint32_t>(std::max(0, v.duration)));
        }

        SwingAnimation DeSwing(Network::PacketReader& r) {
            SwingAnimation v;
            const uint32_t type = r.ReadVarInt();
            // ByIdMap OutOfBoundsStrategy.ZERO.
            v.type = type <= 2 ? static_cast<SwingAnimationType>(type) : SwingAnimationType::None;
            v.duration = static_cast<int>(r.ReadVarInt());
            return v;
        }

    } // namespace

    const DataComponentType<PiercingWeapon> PIERCING_WEAPON{"piercing_weapon", 270, &SerPiercing, &DePiercing};
    const DataComponentType<KineticWeapon>  KINETIC_WEAPON {"kinetic_weapon",  271, &SerKinetic,  &DeKinetic};
    const DataComponentType<AttackRange>    ATTACK_RANGE   {"attack_range",    272, &SerAttackRange, &DeAttackRange};
    const DataComponentType<float>          MINIMUM_ATTACK_CHARGE{"minimum_attack_charge", 273,
                                                                  &SerChargeFloat, &DeChargeFloat};
    const DataComponentType<std::string>    DAMAGE_TYPE    {"damage_type",     274, &SerTypeId,   &DeTypeId};
    const DataComponentType<SwingAnimation> ATTACK_ANIMATION  {"attack_animation",   275, &SerSwing, &DeSwing};
    const DataComponentType<SwingAnimation> INTERACT_ANIMATION{"interact_animation", 276, &SerSwing, &DeSwing};

} // namespace Game::DataComponents
