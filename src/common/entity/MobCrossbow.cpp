// File: src/common/entity/MobCrossbow.cpp
//
// See MobCrossbow.hpp. The shooting mirrors the player's crossbow in
// server/items/FireworkItems.cpp (CrossbowShoot) with MC's mob differences:
// the shot is aimed at the target (CrossbowItem.shootProjectile's
// targetOverride branch through getProjectileShotVector), it is never a crit
// (ProjectileWeaponItem.shoot's isCrit = shooter instanceof Player), and the
// ammunition comes from Monster.getProjectile.

#include "common/entity/MobCrossbow.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/FireworkItems.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/FireworkRocket.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace Game::MobCrossbow {

    namespace {

        // JOML Vector3f.rotateAxis(angle, x, y, z) — the quaternion sandwich
        // written out with the axis used AS GIVEN (JOML does not normalise
        // it, and getProjectileShotVector hands it non-unit axes; the
        // float rounding is JOML's too).
        glm::vec3 RotateAxisJoml(const glm::vec3& v, float angle, const glm::vec3& a) {
            if (a.y == 0.0f && a.z == 0.0f && std::fabs(std::fabs(a.x) - 1.0f) == 0.0f) {
                // rotateX(aX * angle).
                const float ang = a.x * angle;
                const float s = std::sin(ang), c = std::cos(ang);
                return { v.x, v.y * c - v.z * s, v.y * s + v.z * c };
            }
            const float hangle = angle * 0.5f;
            const float sinAngle = std::sin(hangle);
            const float qx = a.x * sinAngle, qy = a.y * sinAngle, qz = a.z * sinAngle;
            const float qw = std::cos(hangle);
            const float w2 = qw * qw, x2 = qx * qx, y2 = qy * qy, z2 = qz * qz, zw = qz * qw;
            const float xy = qx * qy, xz = qx * qz, yw = qy * qw, yz = qy * qz, xw = qx * qw;
            const float nx = (w2 + x2 - z2 - y2) * v.x + (-zw + xy - zw + xy) * v.y + (yw + xz + xz + yw) * v.z;
            const float ny = (xy + zw + zw + xy) * v.x + (y2 - z2 + w2 - x2) * v.y + (yz + yz - xw - xw) * v.z;
            const float nz = (xz - yw + xz - yw) * v.x + (yz + yz + xw + xw) * v.y + (z2 - y2 - x2 + w2) * v.z;
            return { nx, ny, nz };
        }

        // MC Entity.calculateViewVector(xRot, yRot).
        glm::dvec3 ViewVector(float xRotDeg, float yRotDeg) {
            const double f = static_cast<double>(xRotDeg) * Mth::kDegToRad;
            const double g = -static_cast<double>(yRotDeg) * Mth::kDegToRad;
            return { std::sin(g) * std::cos(f), -std::sin(f), std::cos(g) * std::cos(f) };
        }

        // MC CrossbowItem.getProjectileShotVector(body, originalVector, angle).
        glm::vec3 ProjectileShotVector(const Mob& body, const glm::dvec3& original, float angle) {
            glm::vec3 viewVec = glm::vec3(original);
            const float len = glm::length(viewVec);
            if (len > 0.0f) viewVec /= len;   // Vector3f.normalize
            glm::vec3 rightVectorPreRot = glm::cross(viewVec, glm::vec3(0.0f, 1.0f, 0.0f));
            if (static_cast<double>(glm::dot(rightVectorPreRot, rightVectorPreRot)) <= 1.0e-7) {
                const glm::vec3 up = glm::vec3(ViewVector(body.xRot - 90.0f, body.yRot));   // getUpVector
                rightVectorPreRot = glm::cross(viewVec, up);
            }
            const glm::vec3 viewVec3f = RotateAxisJoml(viewVec, 1.5707964f, rightVectorPreRot);
            return RotateAxisJoml(viewVec, angle * 0.017453292f, viewVec3f);
        }

        // MC CrossbowItem.getShotPitch.
        float ShotPitch(JavaRandom& random, int index) {
            if (index == 0) return 1.0f;
            const float rangeDecider = (index & 1) == 1 ? 0.63f : 0.43f;
            return 1.0f / (random.NextFloat() * 0.5f + 1.8f) + rangeDecider;
        }

        // MC ProjectileWeaponItem.getHeldProjectile(entity, ARROW_OR_FIREWORK):
        // the off hand first, then the main hand.
        EquipmentSlot HeldProjectileSlot(const Mob& mob, bool& found) {
            found = true;
            if (FireworkItems::IsCrossbowHeldProjectile(mob.GetOffhandEquipment().itemId) &&
                !mob.GetOffhandEquipment().IsEmpty()) {
                return EquipmentSlot::OFFHAND;
            }
            if (FireworkItems::IsCrossbowHeldProjectile(mob.GetMainHandEquipment().itemId) &&
                !mob.GetMainHandEquipment().IsEmpty()) {
                return EquipmentSlot::MAINHAND;
            }
            found = false;
            return EquipmentSlot::MAINHAND;
        }

        // MC CrossbowItem.tryLoadProjectiles(shooter, crossbow) →
        // ProjectileWeaponItem.draw(crossbow, shooter.getProjectile(crossbow),
        // shooter). Monster.getProjectile: a held arrow / rocket, else a
        // fresh arrow. useAmmo: the first draw costs ammo_use (Infinity has
        // no match on a mob's crossbow), the Multishot copies nothing; a held
        // stack shrinks in its hand.
        bool TryLoadProjectiles(Mob& mob, ItemStack& crossbow) {
            EntityLevel* level = mob.Level();
            if (!level) return false;
            bool held = false;
            const EquipmentSlot heldSlot = HeldProjectileSlot(mob, held);
            ItemStack fresh(Items::Arrow, 1);
            ItemStack projectile = held ? mob.GetEquipment(heldSlot) : fresh;
            if (projectile.IsEmpty()) return false;

            const int count = EnchantmentHelper::ProcessProjectileCount(*level, crossbow, mob, 1);
            ItemStack projectileCopy = projectile;
            std::vector<ItemStack> drawn;
            drawn.reserve(static_cast<size_t>(std::max(count, 0)));
            for (int i = 0; i < count; ++i) {
                ItemStack& source = i == 0 ? projectile : projectileCopy;
                const int ammoToUse = i > 0 ? 0 : EnchantmentHelper::ProcessAmmoUse(*level, crossbow, source, 1);
                if (ammoToUse > source.count) continue;
                ItemStack used = source;
                if (ammoToUse == 0) {
                    used.count = 1;   // copyWithCount(1), marked INTANGIBLE_PROJECTILE
                    used.components.set(DataComponents::INTANGIBLE_PROJECTILE, true);
                } else {
                    used.count = ammoToUse;
                    source.count -= ammoToUse;   // split
                }
                drawn.push_back(std::move(used));
            }
            if (held) {
                // The held stack as the draw left it (split shrinks it in
                // place in MC).
                if (projectile.count <= 0) projectile = ItemStack{};
                mob.ReplaceEquipmentStack(heldSlot, projectile);
            }
            if (drawn.empty()) return false;
            ChargedProjectiles charged;
            charged.items = std::move(drawn);
            FireworkItems::SetChargedProjectiles(crossbow, charged);
            return true;
        }

        // MC CrossbowItem.onUseTick(level, entity, crossbow, ticksRemaining).
        void OnUseTick(Mob& mob, UseState& use, ItemStack& crossbow, int ticksRemaining) {
            EntityLevel* level = mob.Level();
            if (!level || level->IsClientSide()) return;
            // getChargingSounds: Quick Charge's start (no middle), else the
            // DEFAULT_SOUNDS; the end is loading_end either way.
            const std::string quickStart = EnchantmentHelper::CrossbowChargingStartSound(crossbow);
            const bool hasMid = quickStart.empty();
            const int chargeDuration = ChargeDuration(crossbow);
            const float tickPercent = chargeDuration > 0
                ? static_cast<float>(kUseDuration - ticksRemaining) / static_cast<float>(chargeDuration)
                : 1.0f;
            if (tickPercent < 0.2f) {
                use.startSoundPlayed = false;
                use.midLoadSoundPlayed = false;
            }
            if (tickPercent >= 0.2f && !use.startSoundPlayed) {
                use.startSoundPlayed = true;
                level->PlaySound(nullptr, mob.position,
                                 quickStart.empty() ? std::string(SoundEvents::CROSSBOW_LOADING_START) : quickStart,
                                 SoundSource::Players, 0.5f, 1.0f);
            }
            if (tickPercent >= 0.5f && !use.midLoadSoundPlayed) {
                use.midLoadSoundPlayed = true;
                if (hasMid) {
                    level->PlaySound(nullptr, mob.position, SoundEvents::CROSSBOW_LOADING_MIDDLE,
                                     SoundSource::Players, 0.5f, 1.0f);
                }
            }
            if (tickPercent >= 1.0f && !FireworkItems::IsCrossbowCharged(crossbow) &&
                TryLoadProjectiles(mob, crossbow)) {
                JavaRandom& r = level->Random();
                level->PlaySound(nullptr, mob.position, SoundEvents::CROSSBOW_LOADING_END, mob.GetSoundSource(),
                                 1.0f, 1.0f / (r.NextFloat() * 0.5f + 1.0f) + 0.2f);
            }
        }

        void UpdateUsingItem(Mob& mob, UseState& use) {
            ItemStack crossbow = mob.GetEquipment(use.hand);
            if (crossbow.itemId != Items::Crossbow) {
                // MC updatingUsingItem: the hand no longer holds the used
                // item → stopUsingItem.
                StopUsingItem(use);
                return;
            }
            OnUseTick(mob, use, crossbow, use.remaining);
            // The crossbow's CHARGED_PROJECTILES mutate the held stack in
            // place in MC: stored back without an equip event.
            mob.ReplaceEquipmentStack(use.hand, crossbow);
            --use.remaining;
        }

    } // namespace

    EquipmentSlot WeaponHoldingHand(const Mob& mob, ItemID weapon) {
        return mob.GetMainHandEquipment().itemId == weapon ? EquipmentSlot::MAINHAND : EquipmentSlot::OFFHAND;
    }

    int DefaultProjectileRange(ItemID id) {
        if (id == Items::Crossbow) return kCrossbowRange;
        if (id == Items::Bow) return kBowRange;
        return 0;
    }

    bool IsWithinAttackRange(const Mob& body, const LivingEntity& target, int projectileAttackRangeMargin) {
        const ItemStack& main = body.GetMainHandEquipment();
        const int range = main.IsEmpty() ? 0 : DefaultProjectileRange(main.itemId);
        if (range > 0 && body.CanUseNonMeleeWeapon(main)) {
            const double maxAllowedDistance = static_cast<double>(range - projectileAttackRangeMargin);
            return body.DistanceToSqr(target) < maxAllowedDistance * maxAllowedDistance;   // closerThan
        }
        return body.IsWithinMeleeAttackRange(target);
    }

    int ChargeDuration(const ItemStack& crossbow) {
        return FireworkItems::CrossbowChargeDuration(crossbow);
    }

    void StartUsingItem(Mob& mob, UseState& use, EquipmentSlot hand) {
        const ItemStack& stack = mob.GetEquipment(hand);
        if (stack.IsEmpty() || use.usingItem) return;
        use.usingItem = true;
        use.hand = hand;
        use.remaining = kUseDuration;   // itemStack.getUseDuration(this)
        // CrossbowItem.use's latch reset happens for players only; a mob's
        // start carries the previous draw's latches, reset by onUseTick's
        // < 0.2 branch on the first tick.
        if (EntityLevel* level = mob.Level(); level && !level->IsClientSide()) {
            mob.GameEvent(GameEventId::ItemInteractStart);   // causeUseVibration
        }
    }

    void TickUsingItem(Mob& mob, UseState& use) {
        if (!use.usingItem) return;
        UpdateUsingItem(mob, use);
    }

    void ReleaseUsingItem(Mob& mob, UseState& use) {
        if (use.usingItem && mob.GetEquipment(use.hand).itemId == Items::Crossbow) {
            // CrossbowItem.releaseUsing only answers; useOnRelease → one more
            // updatingUsingItem (the loading onUseTick at full charge).
            UpdateUsingItem(mob, use);
        }
        StopUsingItem(use);
    }

    void StopUsingItem(UseState& use) {
        use.usingItem = false;
        use.remaining = 0;
    }

    void PerformCrossbowAttack(Mob& body, CrossbowAttackMob& shooter, LivingEntity* target, float power) {
        EntityLevel* level = body.Level();
        const EquipmentSlot hand = WeaponHoldingHand(body, Items::Crossbow);
        ItemStack crossbow = body.GetEquipment(hand);
        if (level && !level->IsClientSide() && crossbow.itemId == Items::Crossbow) {
            // performShooting: CHARGED_PROJECTILES set to EMPTY; the old list
            // is what flies.
            const auto loaded = crossbow.get(DataComponents::CHARGED_PROJECTILES);
            if (loaded && !loaded->IsEmpty()) {
                const std::vector<ItemStack> projectiles = loaded->items;
                FireworkItems::SetChargedProjectiles(crossbow, ChargedProjectiles{});
                // RangedAttackMob.rangedAttackUncertainty: 14 - difficulty * 4.
                const float uncertainty = static_cast<float>(14 - static_cast<int>(level->GetDifficulty()) * 4);

                // ProjectileWeaponItem.shoot — the Multishot fan.
                const int n = static_cast<int>(projectiles.size());
                const float maxAngle = EnchantmentHelper::ProcessProjectileSpread(*level, crossbow, body, 0.0f);
                const float angleStep = n == 1 ? 0.0f : 2.0f * maxAngle / static_cast<float>(n - 1);
                const float angleOffset = static_cast<float>((n - 1) % 2) * angleStep / 2.0f;
                float direction = 1.0f;
                JavaRandom& random = level->Random();

                for (int i = 0; i < n; ++i) {
                    const ItemStack& ammo = projectiles[static_cast<size_t>(i)];
                    if (ammo.IsEmpty()) continue;
                    const float angle = angleOffset + direction * static_cast<float>((i + 1) / 2) * angleStep;
                    direction = -direction;

                    // CrossbowItem.createProjectile.
                    std::unique_ptr<Projectile> projectile;
                    if (ammo.itemId == Items::FireworkRocket) {
                        auto rocket = std::make_unique<FireworkRocket>(level);
                        rocket->InitLaunch(glm::dvec3(body.position.x, body.GetEyeY() - 0.15000000596046448,
                                                      body.position.z), ammo);
                        rocket->SetOwner(&body);
                        rocket->SetShotAtAngle(true);
                        projectile = std::move(rocket);
                    } else {
                        // ArrowItem.createArrow(level, projectile, mob,
                        // weapon): at the eye, 0.1 down; not a crit (the
                        // shooter is no Player); CROSSBOW_HIT; the weapon's
                        // Piercing.
                        auto arrow = std::make_unique<Arrow>(level);
                        arrow->SetOwner(&body);
                        if (ammo.itemId == Items::TippedArrow) arrow->SetPotionFromPickupStack(ammo);
                        arrow->SetFiredFromWeapon(crossbow);
                        const int pierce = EnchantmentHelper::GetPiercingCount(*level, crossbow, ammo);
                        if (pierce > 0) arrow->SetPierceLevel(pierce);
                        arrow->SetSoundEvent(SoundEvents::CROSSBOW_HIT);
                        arrow->position = glm::dvec3(body.position.x, body.GetEyeY() - 0.10000000149011612,
                                                     body.position.z);
                        arrow->oldPosition = arrow->position;
                        projectile = std::move(arrow);
                    }

                    // CrossbowItem.shootProjectile.
                    glm::vec3 shot;
                    if (target) {
                        const double xd = target->position.x - body.position.x;
                        const double zd = target->position.z - body.position.z;
                        const double distanceToTarget = std::sqrt(xd * xd + zd * zd);
                        const double yd = (target->position.y + static_cast<double>(target->GetBbHeight()) / 3.0) -
                                          projectile->position.y + distanceToTarget * 0.20000000298023224;
                        shot = ProjectileShotVector(body, glm::dvec3(xd, yd, zd), angle);
                    } else {
                        const glm::dvec3 up = ViewVector(body.xRot - 90.0f, body.yRot);
                        const glm::dvec3 view = ViewVector(body.xRot, body.yRot);
                        shot = RotateAxisJoml(glm::vec3(view), angle * 0.017453292f, glm::vec3(up));
                    }
                    projectile->Shoot(shot.x, shot.y, shot.z, power, uncertainty);
                    level->PlaySound(nullptr, body.position, SoundEvents::CROSSBOW_SHOOT, body.GetSoundSource(),
                                     1.0f, ShotPitch(random, i));

                    // Projectile.spawnProjectile: add, then
                    // applyOnProjectileSpawned (Flame on the arrow).
                    Projectile* spawned = projectile.get();
                    level->AddFreshEntity(std::move(projectile));
                    if (auto* arrow = dynamic_cast<Arrow*>(spawned)) {
                        if (ItemStack* launcher = arrow->GetWeaponItem(); launcher && launcher->itemId != ammo.itemId) {
                            EnchantmentHelper::OnProjectileSpawned(*level, *launcher, *arrow, &body,
                                [arrow](const ItemStack& broken) { arrow->OnItemBreak(broken); });
                        }
                    }

                    // weapon.hurtAndBreak(getDurabilityUse(projectile), shooter,
                    // hand) — a rocket 3, an arrow 1; a broken crossbow ends
                    // the volley.
                    HurtAndBreak(crossbow, ammo.itemId == Items::FireworkRocket ? 3 : 1, body, hand);
                    if (crossbow.IsEmpty()) break;
                }
                body.ReplaceEquipmentStack(hand, crossbow);
            }
        }
        shooter.OnCrossbowAttackPerformed();
    }

    std::string SpriteFor(const ItemStack& crossbow, int useTicks) {
        if (const auto charged = crossbow.get(DataComponents::CHARGED_PROJECTILES); charged && !charged->IsEmpty()) {
            return charged->Contains(Items::FireworkRocket) ? "crossbow_firework" : "crossbow_arrow";
        }
        if (useTicks < 0) return "crossbow_standby";
        const float pull = static_cast<float>(useTicks) / static_cast<float>(std::max(1, ChargeDuration(crossbow)));
        if (pull >= 1.0f)  return "crossbow_pulling_2";
        if (pull >= 0.58f) return "crossbow_pulling_1";
        return "crossbow_pulling_0";
    }

} // namespace Game::MobCrossbow
