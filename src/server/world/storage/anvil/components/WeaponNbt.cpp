// File: src/server/world/storage/anvil/components/WeaponNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the weapon
// components, registered with ComponentNbt: blocks_attacks (its type lives
// in DataComponents.hpp), piercing_weapon, kinetic_weapon, attack_range,
// minimum_attack_charge, damage_type, attack_animation, interact_animation.
// (weapon stays with the durability set in ItemStackNbt.cpp.)
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "common/data/components/WeaponComponents.hpp"

#include <cmath>

namespace Game::Anvil::ComponentNbt {

    namespace {

        std::optional<double> NumberKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            return tag ? NumberOf(*tag) : std::nullopt;
        }
        std::optional<bool> BoolKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            return tag ? BoolOf(*tag) : std::nullopt;
        }
        // SoundEvent.CODEC: an id, or {sound_id, range?}; kept without
        // "minecraft:".
        std::string SoundKey(const ::World::NBTTagCompound& c, const char* key) {
            auto tag = c.GetTag(key);
            if (!tag) return {};
            if (auto s = StringOf(*tag)) return std::string(StripMinecraft(*s));
            if (const auto* d = AsCompound(tag.get())) return std::string(StripMinecraft(d->GetValue<std::string>("sound_id", "")));
            return {};
        }
        void WriteSound(Nbt::Writer& w, const char* key, const std::string& sound) {
            if (!sound.empty()) w.String(key, WithNamespace(sound));
        }
        std::vector<std::string> HolderSetKey(const ::World::NBTTagCompound& c, const char* key) {
            std::vector<std::string> set = ReadHolderSet(c.GetTag(key).get());
            for (std::string& e : set) e = WithNamespace(e);
            return set;
        }

        // ── blocks_attacks ────────────────────────────────────────────────
        void WriteBlocksAttacks(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto b = stack.components.get(DataComponents::BLOCKS_ATTACKS);
            if (!b) return;
            w.BeginCompound(key);
            if (b->blockDelaySeconds != 0.0f) w.Float("block_delay_seconds", b->blockDelaySeconds);
            if (b->disableCooldownScale != 1.0f) w.Float("disable_cooldown_scale", b->disableCooldownScale);
            auto list = w.BeginList("damage_reductions", Nbt::TagType::Compound);
            for (const BlocksAttacks::DamageReduction& r : b->damageReductions) {
                w.ListCompoundBegin(list);
                if (r.horizontalBlockingAngle != 90.0f) w.Float("horizontal_blocking_angle", r.horizontalBlockingAngle);
                if (!r.type.empty()) WriteHolderSet(w, "type", r.type);
                w.Float("base", r.base);
                w.Float("factor", r.factor);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
            w.BeginCompound("item_damage");
            w.Float("threshold", b->itemDamage.threshold);
            w.Float("base", b->itemDamage.base);
            w.Float("factor", b->itemDamage.factor);
            w.EndCompound();
            if (!b->bypassedBy.empty()) WriteHolderSet(w, "bypassed_by", b->bypassedBy);
            WriteSound(w, "block_sound", b->blockSound);
            WriteSound(w, "disabled_sound", b->disableSound);
            w.EndCompound();
        }

        bool ReadBlocksAttacks(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            BlocksAttacks b;
            if (auto v = NumberKey(*c, "block_delay_seconds")) b.blockDelaySeconds = static_cast<float>(*v);
            if (auto v = NumberKey(*c, "disable_cooldown_scale")) b.disableCooldownScale = static_cast<float>(*v);
            if (b.blockDelaySeconds < 0.0f || b.disableCooldownScale < 0.0f) {
                ctx.Fail("Value must be non-negative");
                return false;
            }
            if (const auto* list = AsList(c->GetTag("damage_reductions").get())) {
                b.damageReductions.clear();
                for (const auto& element : list->value) {
                    const auto* r = AsCompound(Unwrap(element.get()));
                    if (!r) { ctx.Fail("Not a map"); return false; }
                    BlocksAttacks::DamageReduction dr;
                    dr.horizontalBlockingAngle = static_cast<float>(NumberKey(*r, "horizontal_blocking_angle").value_or(90.0));
                    dr.type = HolderSetKey(*r, "type");
                    const auto base = NumberKey(*r, "base");
                    const auto factor = NumberKey(*r, "factor");
                    if (!base || !factor) { ctx.Fail("No key base / factor in MapLike"); return false; }
                    dr.base = static_cast<float>(*base);
                    dr.factor = static_cast<float>(*factor);
                    b.damageReductions.push_back(std::move(dr));
                }
            }
            if (const auto* d = AsCompound(c->GetTag("item_damage").get())) {
                const auto threshold = NumberKey(*d, "threshold");
                const auto base = NumberKey(*d, "base");
                const auto factor = NumberKey(*d, "factor");
                if (!threshold || !base || !factor) { ctx.Fail("No key threshold / base / factor in MapLike"); return false; }
                b.itemDamage = BlocksAttacks::ItemDamageFunction{static_cast<float>(*threshold), static_cast<float>(*base),
                                                                 static_cast<float>(*factor)};
            }
            b.bypassedBy = HolderSetKey(*c, "bypassed_by");
            b.blockSound = SoundKey(*c, "block_sound");
            b.disableSound = SoundKey(*c, "disabled_sound");
            stack.components.set(DataComponents::BLOCKS_ATTACKS, std::move(b));
            return true;
        }

        // ── piercing_weapon ───────────────────────────────────────────────
        void WritePiercing(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto p = stack.components.get(DataComponents::PIERCING_WEAPON);
            if (!p) return;
            w.BeginCompound(key);
            if (!p->dealsKnockback) w.Bool("deals_knockback", false);
            if (p->dismounts) w.Bool("dismounts", true);
            WriteSound(w, "sound", p->sound);
            WriteSound(w, "hit_sound", p->hitSound);
            w.EndCompound();
        }

        bool ReadPiercing(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            PiercingWeapon p;
            p.dealsKnockback = BoolKey(*c, "deals_knockback").value_or(true);
            p.dismounts = BoolKey(*c, "dismounts").value_or(false);
            p.sound = SoundKey(*c, "sound");
            p.hitSound = SoundKey(*c, "hit_sound");
            stack.components.set(DataComponents::PIERCING_WEAPON, std::move(p));
            return true;
        }

        // ── kinetic_weapon ────────────────────────────────────────────────
        void WriteCondition(Nbt::Writer& w, const char* key, const std::optional<KineticWeapon::Condition>& c) {
            if (!c) return;
            w.BeginCompound(key);
            w.Int("max_duration_ticks", c->maxDurationTicks);
            if (c->minSpeed != 0.0f) w.Float("min_speed", c->minSpeed);
            if (c->minRelativeSpeed != 0.0f) w.Float("min_relative_speed", c->minRelativeSpeed);
            w.EndCompound();
        }

        bool ReadCondition(const ::World::NBTTagCompound& parent, const char* key,
                           std::optional<KineticWeapon::Condition>& out, const ReadContext& ctx) {
            auto tag = parent.GetTag(key);
            if (!tag) return true;
            const auto* c = AsCompound(tag.get());
            if (!c) { ctx.Fail("Not a map"); return false; }
            const auto ticks = NumberKey(*c, "max_duration_ticks");
            if (!ticks || *ticks < 0.0) { ctx.Fail("No key max_duration_ticks in MapLike"); return false; }
            KineticWeapon::Condition cond;
            cond.maxDurationTicks = static_cast<int>(*ticks);
            cond.minSpeed = static_cast<float>(NumberKey(*c, "min_speed").value_or(0.0));
            cond.minRelativeSpeed = static_cast<float>(NumberKey(*c, "min_relative_speed").value_or(0.0));
            out = cond;
            return true;
        }

        void WriteKinetic(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto k = stack.components.get(DataComponents::KINETIC_WEAPON);
            if (!k) return;
            w.BeginCompound(key);
            if (k->contactCooldownTicks != 10) w.Int("contact_cooldown_ticks", k->contactCooldownTicks);
            if (k->delayTicks != 0) w.Int("delay_ticks", k->delayTicks);
            WriteCondition(w, "dismount_conditions", k->dismountConditions);
            WriteCondition(w, "knockback_conditions", k->knockbackConditions);
            WriteCondition(w, "damage_conditions", k->damageConditions);
            if (k->forwardMovement != 0.0f) w.Float("forward_movement", k->forwardMovement);
            if (k->damageMultiplier != 1.0f) w.Float("damage_multiplier", k->damageMultiplier);
            WriteSound(w, "sound", k->sound);
            WriteSound(w, "hit_sound", k->hitSound);
            w.EndCompound();
        }

        bool ReadKinetic(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            KineticWeapon k;
            k.contactCooldownTicks = static_cast<int>(NumberKey(*c, "contact_cooldown_ticks").value_or(10.0));
            k.delayTicks = static_cast<int>(NumberKey(*c, "delay_ticks").value_or(0.0));
            if (k.contactCooldownTicks < 0 || k.delayTicks < 0) { ctx.Fail("Value must be non-negative"); return false; }
            if (!ReadCondition(*c, "dismount_conditions", k.dismountConditions, ctx)) return false;
            if (!ReadCondition(*c, "knockback_conditions", k.knockbackConditions, ctx)) return false;
            if (!ReadCondition(*c, "damage_conditions", k.damageConditions, ctx)) return false;
            k.forwardMovement = static_cast<float>(NumberKey(*c, "forward_movement").value_or(0.0));
            k.damageMultiplier = static_cast<float>(NumberKey(*c, "damage_multiplier").value_or(1.0));
            k.sound = SoundKey(*c, "sound");
            k.hitSound = SoundKey(*c, "hit_sound");
            stack.components.set(DataComponents::KINETIC_WEAPON, std::move(k));
            return true;
        }

        // ── attack_range ──────────────────────────────────────────────────
        void WriteAttackRange(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto r = stack.components.get(DataComponents::ATTACK_RANGE);
            if (!r) return;
            const AttackRange d;
            w.BeginCompound(key);
            if (r->minReach != d.minReach) w.Float("min_reach", r->minReach);
            if (r->maxReach != d.maxReach) w.Float("max_reach", r->maxReach);
            if (r->minCreativeReach != d.minCreativeReach) w.Float("min_creative_reach", r->minCreativeReach);
            if (r->maxCreativeReach != d.maxCreativeReach) w.Float("max_creative_reach", r->maxCreativeReach);
            if (r->hitboxMargin != d.hitboxMargin) w.Float("hitbox_margin", r->hitboxMargin);
            if (r->mobFactor != d.mobFactor) w.Float("mob_factor", r->mobFactor);
            w.EndCompound();
        }

        bool ReadAttackRange(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            AttackRange r;
            r.minReach = static_cast<float>(NumberKey(*c, "min_reach").value_or(r.minReach));
            r.maxReach = static_cast<float>(NumberKey(*c, "max_reach").value_or(r.maxReach));
            r.minCreativeReach = static_cast<float>(NumberKey(*c, "min_creative_reach").value_or(r.minCreativeReach));
            r.maxCreativeReach = static_cast<float>(NumberKey(*c, "max_creative_reach").value_or(r.maxCreativeReach));
            r.hitboxMargin = static_cast<float>(NumberKey(*c, "hitbox_margin").value_or(r.hitboxMargin));
            r.mobFactor = static_cast<float>(NumberKey(*c, "mob_factor").value_or(r.mobFactor));
            // floatRange(0, 64) reaches, (0, 1) margin, (0, 2) mob factor.
            for (float v : { r.minReach, r.maxReach, r.minCreativeReach, r.maxCreativeReach }) {
                if (v < 0.0f || v > 64.0f) { ctx.Fail("Value " + std::to_string(v) + " outside of range [0.0:64.0]"); return false; }
            }
            if (r.hitboxMargin < 0.0f || r.hitboxMargin > 1.0f || r.mobFactor < 0.0f || r.mobFactor > 2.0f) {
                ctx.Fail("Value outside of range");
                return false;
            }
            stack.components.set(DataComponents::ATTACK_RANGE, r);
            return true;
        }

        // ── minimum_attack_charge (floatRange(0, 1)) ───────────────────────
        void WriteMinCharge(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto v = stack.components.get(DataComponents::MINIMUM_ATTACK_CHARGE)) w.Float(key, *v);
        }

        bool ReadMinCharge(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto v = NumberOf(tag);
            if (!v) { ctx.Fail("Not a number"); return false; }
            if (*v < 0.0 || *v > 1.0) { ctx.Fail("Value " + std::to_string(*v) + " outside of range [0.0:1.0]"); return false; }
            stack.components.set(DataComponents::MINIMUM_ATTACK_CHARGE, static_cast<float>(*v));
            return true;
        }

        // ── damage_type (a damage type id) ─────────────────────────────────
        void WriteDamageType(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto v = stack.components.get(DataComponents::DAMAGE_TYPE); v && !v->empty()) w.String(key, WithNamespace(*v));
        }

        bool ReadDamageType(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto id = StringOf(tag);
            if (!id || id->empty()) { ctx.Fail("Not a string"); return false; }
            stack.components.set(DataComponents::DAMAGE_TYPE, WithNamespace(*id));
            return true;
        }

        // ── attack_animation / interact_animation (SwingAnimation) ─────────
        void WriteSwing(Nbt::Writer& w, std::string_view key, const SwingAnimation& a) {
            w.BeginCompound(key);
            if (a.type != SwingAnimationType::Whack) w.String("type", std::string(SwingAnimationTypeName(a.type)));
            if (a.duration != 6) w.Int("duration", a.duration);
            w.EndCompound();
        }

        bool ReadSwing(const ::World::NBTTag& tag, SwingAnimation& out, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            if (auto t = c->GetTag("type")) {
                const auto name = StringOf(*t);
                if (!name || !SwingAnimationTypeFromName(*name, out.type)) {
                    ctx.Fail("Unknown element name:" + name.value_or("?"));
                    return false;
                }
            }
            const int duration = static_cast<int>(NumberKey(*c, "duration").value_or(6.0));
            if (duration < 0) { ctx.Fail("Value must be non-negative: " + std::to_string(duration)); return false; }
            out.duration = duration;
            return true;
        }

        void WriteAttackAnimation(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto a = stack.components.get(DataComponents::ATTACK_ANIMATION)) WriteSwing(w, key, *a);
        }
        bool ReadAttackAnimation(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            SwingAnimation a;
            if (!ReadSwing(tag, a, ctx)) return false;
            stack.components.set(DataComponents::ATTACK_ANIMATION, a);
            return true;
        }
        void WriteInteractAnimation(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto a = stack.components.get(DataComponents::INTERACT_ANIMATION)) WriteSwing(w, key, *a);
        }
        bool ReadInteractAnimation(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            SwingAnimation a;
            if (!ReadSwing(tag, a, ctx)) return false;
            stack.components.set(DataComponents::INTERACT_ANIMATION, a);
            return true;
        }

        const Registrar kBlocksAttacks{DataComponents::BLOCKS_ATTACKS, &WriteBlocksAttacks, &ReadBlocksAttacks};
        const Registrar kPiercing{DataComponents::PIERCING_WEAPON, &WritePiercing, &ReadPiercing};
        const Registrar kKinetic{DataComponents::KINETIC_WEAPON, &WriteKinetic, &ReadKinetic};
        const Registrar kAttackRange{DataComponents::ATTACK_RANGE, &WriteAttackRange, &ReadAttackRange};
        const Registrar kMinCharge{DataComponents::MINIMUM_ATTACK_CHARGE, &WriteMinCharge, &ReadMinCharge};
        const Registrar kDamageType{DataComponents::DAMAGE_TYPE, &WriteDamageType, &ReadDamageType};
        const Registrar kAttackAnimation{DataComponents::ATTACK_ANIMATION, &WriteAttackAnimation, &ReadAttackAnimation};
        const Registrar kInteractAnimation{DataComponents::INTERACT_ANIMATION, &WriteInteractAnimation, &ReadInteractAnimation};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
