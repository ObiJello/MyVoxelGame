// File: src/common/entity/SpearItem.cpp
#include "common/entity/SpearItem.hpp"
#include "server/advancements/CriteriaTriggers.hpp"

#include "common/core/Ease.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace Game {
namespace Spear {

    namespace {

        // ── Geometry (ProjectileUtil / AABB.clip) ─────────────────────────

        // AABB.clip(from, to): the entry point of the segment into the box,
        // false when it misses (or starts inside — callers test contains
        // first, as MC does).
        bool ClipBox(const AABBd& box, const glm::dvec3& from, const glm::dvec3& to, glm::dvec3& out) {
            const glm::dvec3 d = to - from;
            double tMin = 0.0, tMax = 1.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double o = from[axis], dir = d[axis];
                const double lo = box.min[axis], hi = box.max[axis];
                if (std::abs(dir) < 1.0e-12) {
                    if (o < lo || o > hi) return false;
                    continue;
                }
                double t0 = (lo - o) / dir, t1 = (hi - o) / dir;
                if (t0 > t1) std::swap(t0, t1);
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
                if (tMin > tMax) return false;
            }
            out = from + d * tMin;
            return true;
        }

        bool BoxContains(const AABBd& box, const glm::dvec3& p) {
            return p.x >= box.min.x && p.x < box.max.x && p.y >= box.min.y && p.y < box.max.y &&
                   p.z >= box.min.z && p.z < box.max.z;
        }

        AABBd Inflate(const AABBd& box, double by) {
            return AABBd::FromMinMax(box.min - glm::dvec3(by), box.max + glm::dvec3(by));
        }

        // Is the point inside a block's collision shape (ClipContext.Block
        // .COLLIDER)?
        bool InCollider(const IBlockAccess& blocks, const glm::dvec3& p) {
            const glm::ivec3 bp(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                                static_cast<int>(std::floor(p.z)));
            const BlockID id = blocks.GetBlock(bp.x, bp.y, bp.z);
            if (id == BlockID::Air || !BlockRegistry::HasCollision(id)) return false;
            const auto set = BlockRegistry::GetBlockCollisionShapeSet(blocks.GetBlockState(bp.x, bp.y, bp.z));
            const glm::dvec3 local = p - glm::dvec3(bp);
            for (uint8_t i = 0; i < set.count; ++i) {
                const auto& b = set.boxes[i];
                if (local.x >= b.min.x && local.x <= b.max.x && local.y >= b.min.y && local.y <= b.max.y &&
                    local.z >= b.min.z && local.z <= b.max.z) {
                    return true;
                }
            }
            return false;
        }

        // Level.clip(from, to, COLLIDER, Fluid.NONE): the first point along
        // the segment inside a collision shape — marched at 1/40 block and
        // bisected to the surface.
        bool ClipBlocks(const IBlockAccess& blocks, const glm::dvec3& from, const glm::dvec3& to,
                        glm::dvec3& hit) {
            const glm::dvec3 d = to - from;
            const double len = glm::length(d);
            if (len < 1.0e-9) return false;
            const int steps = std::max(1, static_cast<int>(std::ceil(len / 0.025)));
            double prev = 0.0;
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                if (!InCollider(blocks, from + d * t)) { prev = t; continue; }
                double lo = prev, hi = t;
                for (int k = 0; k < 8; ++k) {
                    const double mid = 0.5 * (lo + hi);
                    if (InCollider(blocks, from + d * mid)) hi = mid; else lo = mid;
                }
                hit = from + d * hi;
                return true;
            }
            return false;
        }

        // One candidate box against the reach segment —
        // getManyEntityHitResult's body (includeFromEntity and
        // projectSurfaceHitLocation both true).
        bool HitBox(const IBlockAccess& blocks, const AABBd& box, const glm::dvec3& from,
                    const glm::dvec3& to, double margin, glm::dvec3& location) {
            if (BoxContains(box, from)) { location = from; return true; }
            if (ClipBox(box, from, to, location)) return true;
            if (margin <= 0.0) return false;
            glm::dvec3 outside;
            if (!ClipBox(Inflate(box, margin), from, to, outside)) return false;
            glm::dvec3 towards = (box.min + box.max) * 0.5;
            glm::dvec3 blocked;
            if (ClipBlocks(blocks, outside, towards, blocked)) towards = blocked;
            return ClipBox(box, outside, towards, location);
        }

        glm::dvec3 LookAngle(const Entity& e) {
            return glm::dvec3(Mth::ViewVector(e.xRot, e.yRot));
        }

        glm::dvec3 HeadLookAngle(const Entity& e) {
            const auto* living = e.AsLiving();
            return glm::dvec3(Mth::ViewVector(e.xRot, living ? living->yHeadRot : e.yRot));
        }

        // The ATTACK_RANGE a jab / charge reaches with: the weapon's, else
        // AttackRange.defaultFor(user) — the user's ENTITY_INTERACTION_RANGE
        // with no margin.
        AttackRange RangeFor(LivingEntity& user, const ItemStack* stack) {
            if (stack && !stack->IsEmpty()) {
                if (auto range = stack->get(DataComponents::ATTACK_RANGE)) return *range;
            }
            return AttackRange::DefaultFor(user.GetAttributeValue(Attribute::EntityInteractionRange));
        }

        Entity* RootVehicle(Entity& e) {
            Entity* root = &e;
            while (root->GetVehicle()) root = root->GetVehicle();
            return root;
        }

    } // namespace

    // ── Definitions ──────────────────────────────────────────────────────

    std::optional<SpearDefinition> ForStack(const ItemStack& stack) {
        if (stack.IsEmpty()) return std::nullopt;
        SpearDefinition d;
        d.item = stack.itemId;
        d.kineticWeapon = stack.get(DataComponents::KINETIC_WEAPON);
        d.piercingWeapon = stack.get(DataComponents::PIERCING_WEAPON);
        if (!d.kineticWeapon && !d.piercingWeapon) return std::nullopt;
        // AttackRange.defaultFor(user) stands in when the stack names none;
        // GetHitEntitiesAlong reads the attacker's own range then.
        d.range = stack.get(DataComponents::ATTACK_RANGE).value_or(AttackRange{});
        const SwingAnimation swing = GetAttackAnimation(stack);
        d.stabDurationTicks = swing.duration;
        d.stab = swing.type == SwingAnimationType::Stab;
        return d;
    }

    const SpearDefinition* Find(ItemID id) {
        // The prototypes' definitions, built once (every thread reads the
        // same immutable table afterwards).
        static const std::unordered_map<ItemID, SpearDefinition> table = [] {
            std::unordered_map<ItemID, SpearDefinition> out;
            ItemRegistry::ForEachPureItem([&out](ItemID item, const Item&) {
                if (auto d = ForStack(ItemStack(item, 1))) out.emplace(item, std::move(*d));
            });
            return out;
        }();
        const auto it = table.find(id);
        return it == table.end() ? nullptr : &it->second;
    }

    bool IsPiercing(const ItemStack& stack) {
        return !stack.IsEmpty() && stack.has(DataComponents::PIERCING_WEAPON);
    }

    std::optional<KineticWeapon> Kinetic(const ItemStack& stack) {
        if (stack.IsEmpty()) return std::nullopt;
        return stack.get(DataComponents::KINETIC_WEAPON);
    }

    std::optional<PiercingWeapon> Piercing(const ItemStack& stack) {
        if (stack.IsEmpty()) return std::nullopt;
        return stack.get(DataComponents::PIERCING_WEAPON);
    }

    int AttackAnimationDuration(ItemID id) {
        return GetAttackAnimation(id).duration;
    }

    int AttackAnimationDuration(const ItemStack& stack) {
        return GetAttackAnimation(stack).duration;
    }

    bool IsStabSwing(ItemID id) {
        return GetAttackAnimation(id).type == SwingAnimationType::Stab;
    }

    bool IsStabSwing(const ItemStack& stack) {
        return GetAttackAnimation(stack).type == SwingAnimationType::Stab;
    }

    bool CannotAttackWithItem(const ItemStack& stack, int attackStrengthTicker, float attackStrengthDelay,
                              int tolerance) {
        // Player.cannotAttackWithItem: MINIMUM_ATTACK_CHARGE (0 by default)
        // against the optimistic (ticker + tolerance) / delay.
        const float optimistic = static_cast<float>(attackStrengthTicker + tolerance) / attackStrengthDelay;
        return Game::CannotAttackWithItem(stack, optimistic);
    }

    bool CannotAttackWithItem(ItemID id, int attackStrengthTicker, float attackStrengthDelay,
                              int tolerance) {
        return CannotAttackWithItem(ItemStack(id, 1), attackStrengthTicker, attackStrengthDelay, tolerance);
    }

    // ── SpearAnimations ──────────────────────────────────────────────────

    UseParams UseParams::FromKineticWeapon(const KineticWeapon& weapon, float time) {
        const int finishRaisingTick = weapon.delayTicks;
        const int finishSwayingTick =
            (weapon.dismountConditions ? weapon.dismountConditions->maxDurationTicks : 0) + finishRaisingTick;
        const int startSwayingTick = finishSwayingTick - 20;
        const int finishLoweringTick =
            (weapon.knockbackConditions ? weapon.knockbackConditions->maxDurationTicks : 0) + finishRaisingTick;
        const int startLoweringTick = finishLoweringTick - 40;
        const int finishRaisingBackTick =
            (weapon.damageConditions ? weapon.damageConditions->maxDurationTicks : 0) + finishRaisingTick;

        UseParams p;
        p.raiseProgress = Ease::Progress(time, 0.0f, static_cast<float>(finishRaisingTick));
        p.raiseProgressStart  = Ease::Progress(p.raiseProgress, 0.0f, 0.5f);
        p.raiseProgressMiddle = Ease::Progress(p.raiseProgress, 0.5f, 0.8f);
        p.raiseProgressEnd    = Ease::Progress(p.raiseProgress, 0.8f, 1.0f);
        p.swayProgress = Ease::Progress(time, static_cast<float>(startSwayingTick),
                                        static_cast<float>(finishSwayingTick));
        p.lowerProgress = Ease::OutCubic(Ease::InOutElastic(Ease::Progress(
            time - 20.0f, static_cast<float>(startLoweringTick), static_cast<float>(finishLoweringTick))));
        p.raiseBackProgress = Ease::Progress(time, static_cast<float>(finishRaisingBackTick - 5),
                                             static_cast<float>(finishRaisingBackTick));
        p.swayIntensity = 2.0f * Ease::OutCirc(p.swayProgress) - 2.0f * Ease::InCirc(p.raiseBackProgress);
        p.swayScaleSlow = std::sin(time * 19.0f * 0.017453292f) * p.swayIntensity;
        p.swayScaleFast = std::sin(time * 30.0f * 0.017453292f) * p.swayIntensity;
        return p;
    }

    float HitFeedbackAmount(float ticksSinceFeedbackStart) {
        return 0.4f * (Ease::OutQuart(Ease::Progress(ticksSinceFeedbackStart, 1.0f, 3.0f)) -
                       Ease::InOutSine(Ease::Progress(ticksSinceFeedbackStart, 3.0f, 10.0f)));
    }

    // ── Combat ───────────────────────────────────────────────────────────

    glm::dvec3 GetMotion(Entity& entity) {
        // A mob riding something charges at its mount's speed; a player
        // reports its own (the client moves the pair).
        Entity* who = &entity;
        if (!entity.IsPlayer() && entity.IsPassenger()) who = RootVehicle(entity);
        return who->GetKnownSpeed() * 20.0;
    }

    bool CanHitEntity(Entity& jabber, Entity& target) {
        // Entity.isInvulnerableToPiercingWeapon = isInvulnerable, then alive.
        if (target.IsInvulnerable() || !target.IsAlive()) return false;
        // canBeHitByProjectile (isPickable): no spectator, no bobber, no
        // marker.
        if (!target.IsPickable()) return false;
        if (target.GetType() == EntityTypeId::FishingBobber) return false;
        // Not its own mount nor a co-passenger (isPassengerOfSameVehicle:
        // the same root vehicle).
        return RootVehicle(jabber) != RootVehicle(target);
    }

    void GetHitEntitiesAlong(LivingEntity& attacker, const AttackRange& range,
                             const std::function<bool(Entity&)>& matching,
                             std::vector<EntityHit>& out) {
        out.clear();
        EntityLevel* level = attacker.Level();
        if (!level || !level->Blocks()) return;
        const IBlockAccess& blocks = *level->Blocks();

        const glm::dvec3 look = HeadLookAngle(attacker);
        const glm::dvec3 eye = attacker.GetEyePosition();
        const glm::dvec3 from = eye + look * static_cast<double>(range.EffectiveMinRange(attacker));
        const double movementComponent = glm::dot(attacker.GetKnownMovement(), look);
        glm::dvec3 to = eye + look * (static_cast<double>(range.EffectiveMaxRange(attacker)) +
                                      std::max(0.0, movementComponent));

        // The block clip runs from the EYE: a wall nearer than the minimum
        // reach stops the stab altogether.
        glm::dvec3 blockHit;
        if (ClipBlocks(blocks, eye, to, blockHit)) {
            to = blockHit;
            const glm::dvec3 dTo = to - eye, dFrom = from - eye;
            if (glm::dot(dTo, dTo) < glm::dot(dFrom, dFrom)) return;
        }

        // AABB.ofSize(from, margin³).expandTowards(to - from).inflate(1.0).
        const double margin = static_cast<double>(range.hitboxMargin);
        const glm::dvec3 half(margin * 0.5);
        glm::dvec3 lo = from - half, hi = from + half;
        const glm::dvec3 d = to - from;
        for (int i = 0; i < 3; ++i) {
            if (d[i] < 0.0) lo[i] += d[i]; else hi[i] += d[i];
        }
        AABB search;
        search.min = glm::vec3(lo - glm::dvec3(1.0));
        search.max = glm::vec3(hi + glm::dvec3(1.0));

        std::vector<Entity*> candidates;
        level->GetEntitiesInBox(search, &attacker, candidates);
        for (Entity* e : candidates) {
            if (!e || e == &attacker || !matching(*e)) continue;
            // The dragon is struck on its part boxes (MC's EnderDragonPart
            // entities): the head wins when both are on the line.
            if (auto* dragon = dynamic_cast<EnderDragon*>(e)) {
                AABB parts[EnderDragon::kDragonPartCount];
                dragon->ComputePartBoxes(parts);
                int bestPart = -1;
                glm::dvec3 bestAt{0.0};
                for (int p = 0; p < EnderDragon::kDragonPartCount; ++p) {
                    glm::dvec3 at;
                    if (!HitBox(blocks, ToAABBd(parts[p]), from, to, margin, at)) continue;
                    if (bestPart < 0 || p == EnderDragon::kDragonPartHead) {
                        bestPart = p;
                        bestAt = at;
                    }
                }
                if (bestPart >= 0) out.push_back({ e, bestAt, bestPart });
                continue;
            }
            glm::dvec3 at;
            if (HitBox(blocks, e->GetAABBd(), from, to, margin, at)) out.push_back({ e, at, -1 });
        }
    }

    void DamageEntities(const KineticWeapon& weapon, int useDuration, int ticksRemaining,
                        LivingEntity& user, EquipmentSlot slot) {
        EntityLevel* level = user.Level();
        if (!level || level->IsClientSide()) return;
        int ticksUsed = useDuration - ticksRemaining;
        if (ticksUsed < weapon.delayTicks) return;
        ticksUsed -= weapon.delayTicks;

        const ItemStack* stack = user.EquipmentInSlot(slot);
        const AttackRange range = RangeFor(user, stack);

        const glm::dvec3 look = LookAngle(user);
        const double attackerSpeedProjection = glm::dot(look, GetMotion(user));
        const double actionFactor = user.IsPlayer() ? 1.0 : 0.2;
        // getAttributeBaseValue(ATTACK_DAMAGE): the entity's own, without
        // the spear's modifier — a player's 1, a zombie's 3.
        // (A player's server view mirrors the player's own base — /attribute
        // … attack_damage base set — so its row is read when it has one.)
        const double baseMobDamage = user.IsPlayer() && !user.Attributes().Has(Attribute::AttackDamage)
            ? PlayerBaseAttributeValue(Attribute::AttackDamage)
            : user.Attributes().GetBaseValue(Attribute::AttackDamage);

        std::vector<EntityHit> hits;
        GetHitEntitiesAlong(user, range, [&user](Entity& e) { return CanHitEntity(user, e); }, hits);

        bool affected = false;
        for (const EntityHit& hit : hits) {
            Entity& other = *hit.entity;
            if (user.WasRecentlyStabbed(other, weapon.contactCooldownTicks)) continue;
            user.RememberStabbedEntity(other);
            const double targetSpeedProjection = glm::dot(look, GetMotion(other));
            const double relativeSpeed = std::max(0.0, attackerSpeedProjection - targetSpeedProjection);
            const bool dealsDismount = weapon.dismountConditions &&
                weapon.dismountConditions->Test(ticksUsed, attackerSpeedProjection, relativeSpeed, actionFactor);
            const bool dealsKnockback = weapon.knockbackConditions &&
                weapon.knockbackConditions->Test(ticksUsed, attackerSpeedProjection, relativeSpeed, actionFactor);
            const bool dealsDamage = weapon.damageConditions &&
                weapon.damageConditions->Test(ticksUsed, attackerSpeedProjection, relativeSpeed, actionFactor);
            if (!dealsDismount && !dealsKnockback && !dealsDamage) continue;
            const float damageDealt = static_cast<float>(baseMobDamage) +
                static_cast<float>(std::floor(relativeSpeed * static_cast<double>(weapon.damageMultiplier)));
            affected |= user.StabAttack(slot, other, damageDealt, dealsDamage, dealsKnockback, dealsDismount);
        }
        // broadcastEntityEvent(user, 2): the hit sound and the hand's recoil
        // on every client — and for a player, CriteriaTriggers.SPEAR_MOBS
        // with the living entities this charge has stabbed.
        if (affected) {
            level->BroadcastEntityEvent(user, kEntityEventKineticHit);
            if (Server::ServerPlayer* player = Server::CriteriaTriggers::PlayerOf(&user)) {
                Server::CriteriaTriggers::SpearMobs(*player, user.StabbedLivingEntityCount());
            }
        }
    }

    void PiercingAttack(const PiercingWeapon& weapon, LivingEntity& attacker, EquipmentSlot hand) {
        EntityLevel* level = attacker.Level();
        if (!level || level->IsClientSide()) return;
        // damage = getAttributeValue(ATTACK_DAMAGE), the spear's main-hand
        // modifier included (the player's view answers its ServerPlayer's).
        const float damage = attacker.GetJabAttackDamage();
        const ItemStack* stack = attacker.EquipmentInSlot(hand);
        const AttackRange range = RangeFor(attacker, stack);

        std::vector<EntityHit> hits;
        GetHitEntitiesAlong(attacker, range, [&attacker](Entity& e) { return CanHitEntity(attacker, e); }, hits);
        bool hitSomething = false;
        for (const EntityHit& hit : hits) {
            if (auto* dragon = dynamic_cast<EnderDragon*>(hit.entity); dragon && hit.dragonPart >= 0) {
                // The part's hurtServer → EnderDragon.hurt(part, …): the head
                // takes it whole, anything else the body's reduction.
                hitSomething |= attacker.StabAttack(hand, *dragon, damage, true, weapon.dealsKnockback,
                                                    weapon.dismounts);
                continue;
            }
            hitSomething |= attacker.StabAttack(hand, *hit.entity, damage, true, weapon.dealsKnockback,
                                                weapon.dismounts);
        }
        attacker.OnAttack();
        attacker.PostPiercingAttack();
        // makeHitSound: level.playSound(null, …) — everyone.
        if (hitSomething && !weapon.hitSound.empty()) {
            level->PlaySound(nullptr, attacker.position, weapon.hitSound, attacker.GetSoundSource(), 1.0f, 1.0f);
        }
        PlayWeaponSound(attacker, weapon.sound);
        // swingAndResetAttackStrength(MAIN_HAND, STAB, false): the stab
        // swing for everyone watching (the swinger's client began its own).
        attacker.Swing();
    }

    void PlayWeaponSound(LivingEntity& causer, std::string_view sound) {
        EntityLevel* level = causer.Level();
        if (sound.empty() || !level || level->IsClientSide()) return;
        level->PlaySound(&causer, causer.position, sound, causer.GetSoundSource(), 1.0f, 1.0f);
    }

} // namespace Spear

    // ── LivingEntity: the item use and the kinetic bookkeeping ─────────────
    //
    // Defined here, beside the spear logic that is their only reason to
    // exist (declared in LivingEntity.hpp).

    void LivingEntity::StartUsingItem(EquipmentSlot hand) {
        // MC startUsingItem: nothing for an empty hand or while already
        // using.
        const ItemStack* stack = EquipmentInSlot(hand);
        if (!stack || stack->IsEmpty() || m_usingItem) return;
        m_usingItem = true;
        m_usedItemHand = hand;
        m_useItemId = stack->itemId;
        m_useItemDuration = GetUseDuration(*stack);
        m_useItemRemaining = m_useItemDuration;
        if (m_level && !m_level->IsClientSide()) {
            // useItem.causeUseVibration(this, ITEM_INTERACT_START) — unless
            // its USE_EFFECTS turn the vibrations off (the spears).
            if (GetUseEffects(*stack).interactVibrations) GameEvent(GameEventId::ItemInteractStart);
            if (Spear::Kinetic(*stack)) BeginKineticContacts();
        }
    }

    void LivingEntity::StopUsingItem() {
        // MC stopUsingItem: the flags clear, the use item empties, the
        // kinetic table goes (recentKineticEnemies = null).
        m_usingItem = false;
        m_useItemRemaining = 0;
        m_useItemDuration = 0;
        m_useItemId = 0;
        EndKineticContacts();
    }

    void LivingEntity::UpdatingUsingItem() {
        if (!m_usingItem) return;
        ItemStack* stack = EquipmentInSlot(m_usedItemHand);
        // ItemStack.isSameItem(getItemInHand(hand), useItem).
        if (!stack || stack->IsEmpty() || stack->itemId != m_useItemId) {
            StopUsingItem();
            return;
        }
        // updateUsingItem: useItem.onUseTick(level, this, remaining) — a
        // KINETIC_WEAPON's damageEntities on the server — then the countdown;
        // a finished hold completes on the server (no spear gets there:
        // 72000 ticks).
        if (m_level && !m_level->IsClientSide()) {
            if (const auto kinetic = Spear::Kinetic(*stack)) {
                Spear::DamageEntities(*kinetic, m_useItemDuration, m_useItemRemaining, *this, m_usedItemHand);
            }
        }
        if (!m_usingItem) return;
        if (--m_useItemRemaining <= 0 && m_level && !m_level->IsClientSide()) StopUsingItem();
    }

    bool LivingEntity::WasRecentlyStabbed(const Entity& target, int allowedTime) const {
        if (!m_kineticContacts || !m_level) return false;
        for (const auto& [id, time] : m_recentKineticEnemies) {
            if (id == target.GetId()) return m_level->GetGameTime() - time < static_cast<int64_t>(allowedTime);
        }
        return false;
    }

    void LivingEntity::RememberStabbedEntity(const Entity& target) {
        if (!m_kineticContacts || !m_level) return;
        const int64_t now = m_level->GetGameTime();
        for (auto& [id, time] : m_recentKineticEnemies) {
            if (id == target.GetId()) { time = now; return; }
        }
        m_recentKineticEnemies.emplace_back(target.GetId(), now);
    }

    int LivingEntity::StabbedLivingEntityCount() const {
        if (!m_kineticContacts || !m_level) return 0;
        int count = 0;
        for (const auto& entry : m_recentKineticEnemies) {
            const Entity* e = m_level->ResolveEntityById(entry.first);
            if (e && e->AsLiving()) ++count;
        }
        return count;
    }

    void LivingEntity::BeginKineticContacts() {
        m_kineticContacts = true;
        m_recentKineticEnemies.clear();
    }

    void LivingEntity::EndKineticContacts() {
        m_kineticContacts = false;
        m_recentKineticEnemies.clear();
    }

    bool LivingEntity::StabAttack(EquipmentSlot weaponSlot, Entity& target, float baseDamage,
                                  bool dealsDamage, bool dealsKnockback, bool dismounts) {
        // MC LivingEntity.stabAttack (a mob's).
        if (!m_level || m_level->IsClientSide()) return false;
        ItemStack* weapon = EquipmentInSlot(weaponSlot);
        // weaponItem.getDamageSource(this): the spears' DAMAGE_TYPE spear.
        const bool spear = weapon && !weapon->IsEmpty() && Spear::IsSpear(weapon->itemId);
        const MobDamageSource sourceType = spear ? MobDamageSource::Spear : MobDamageSource::MobAttack;
        const DamageSourceInfo source = DamageSourceInfo::Of(sourceType, this, nullptr);
        float damage = baseDamage;
        if (weapon && !weapon->IsEmpty()) {
            damage = EnchantmentHelper::ModifyDamage(*m_level, *weapon, target, source, baseDamage);
        }
        auto* living = target.AsLiving();
        bool dealtDamage = false;
        if (dealsDamage && living) {
            if (auto* dragon = dynamic_cast<EnderDragon*>(living)) {
                dealtDamage = dragon->HurtPart(sourceType, damage, this, /*headHit=*/false, this);
            } else {
                dealtDamage = living->Hurt(sourceType, damage, this);
            }
        }
        bool affected = dealsKnockback || dealtDamage;
        if (dealsKnockback && living) {
            // causeExtraKnockback(target, 0.4) then (target, getKnockback):
            // each a knockback along this entity's facing and a 0.6
            // horizontal brake on the attacker.
            const float yaw = yRot * Mth::kDegToRad;
            const auto extra = [&](float amount) {
                if (amount <= 0.0f) return;
                living->Knockback(amount, std::sin(yaw), -std::cos(yaw));
                velocity.x *= 0.6;
                velocity.z *= 0.6;
            };
            extra(0.4f);
            float knockback = static_cast<float>(GetAttributeValue(Attribute::AttackKnockback));
            if (weapon && !weapon->IsEmpty()) {
                knockback = EnchantmentHelper::ModifyKnockback(*m_level, *weapon, target, source, knockback);
            }
            extra(knockback / 2.0f);
        }
        // Dismount: not the #cannot_be_dismounted_by_item_usage riders (the
        // ender dragon's and the ghast's harness passengers are none here).
        if (dismounts && target.IsPassenger()) {
            affected = true;
            target.StopRiding();
        }
        // weaponItem.hurtEnemy(target, this): the WEAPON wear (1).
        if (living && weapon && !weapon->IsEmpty()) {
            HurtEnemy(*weapon, *this);
        }
        if (dealtDamage) EnchantmentHelper::DoPostAttackEffects(*m_level, target, source);
        if (!affected) return false;
        SetLastHurtMob(&target);
        if (auto* mob = dynamic_cast<Mob*>(this)) mob->PlayAttackSound();
        return true;
    }

    void LivingEntity::PostPiercingAttack() {
        if (!m_level || m_level->IsClientSide()) return;
        EnchantmentHelper::DoPostPiercingAttackEffects(*m_level, *this);
    }

    void LivingEntity::OnKineticHit() {
        if (!m_level) return;
        const int64_t now = m_level->GetGameTime();
        if (now - m_lastKineticHitFeedbackTime <= Spear::kHitFeedbackTicks) return;
        m_lastKineticHitFeedbackTime = now;
        // kineticWeapon.makeLocalHitSound(this): the used weapon's hit sound,
        // locally, at this entity.
        if (!m_usingItem) return;
        const ItemStack* used = EquipmentInSlot(m_usedItemHand);
        const auto kinetic = used && used->itemId == m_useItemId ? Spear::Kinetic(*used) : std::nullopt;
        if (kinetic && !kinetic->hitSound.empty() && !IsSilent()) {
            m_level->PlayLocalSound(position, kinetic->hitSound, GetSoundSource(), 1.0f, 1.0f, false);
        }
    }

    float LivingEntity::GetTicksSinceLastKineticHitFeedback(float partialTick) const {
        if (m_lastKineticHitFeedbackTime < 0 || !m_level) return 0.0f;
        return static_cast<float>(m_level->GetGameTime() - m_lastKineticHitFeedbackTime) + partialTick;
    }

} // namespace Game
