// File: src/common/entity/projectile/ThrowableProjectile.cpp
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    ThrowableProjectile::ThrowableProjectile(EntityTypeId type, EntityLevel* level)
        : Projectile(type, level) {}

    void ThrowableProjectile::SetOwnerAndPosition(LivingEntity& owner) {
        SetOwner(&owner);
        position = glm::dvec3(owner.position.x, owner.GetEyeY() - 0.1,
                              owner.position.z);
    }

    void ThrowableProjectile::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const bool serverSide = !m_level->IsClientSide();

        // MC order: applyGravity, applyInertia, THEN clip along the result.
        velocity.y -= GetDefaultGravity();
        velocity *= static_cast<double>(IsInWater() ? 0.8f : 0.99f);

        const glm::dvec3 origin = position;
        const HitResult hit = Clip(origin, velocity, serverSide);

        position = hit.IsHit() ? hit.location : origin + velocity;
        RotateTowardsMovement(0.2f);

        Entity::BaseTick();

        if (hit.IsHit() && IsAlive()) OnHit(hit);
    }

    // ── ThrownEnderpearl ───────────────────────────────────────────────────

    void ThrownEnderpearl::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC onHitEntity: hurt for ZERO — the hit registers (flash, kill
        // credit chain) but deals nothing; the 5.0 lands on the THROWER.
        DealHitDamage(target, hit, MobDamageSource::Projectile, 0.0f, GetOwner());
    }

    void ThrownEnderpearl::OnHit(const HitResult& hit) {
        // MC ThrownEnderpearl.onHit, transcribed. (The 32 PORTAL particles
        // are a client visual with no ParticleKind here — skipped like the
        // eye of ender's shatter burst.)
        Projectile::OnHit(hit);
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            // The client copy waits for the server's removal, like the TNT.
            return;
        }
        if (IsRemoved()) return;

        Entity* owner = GetOwner();
        auto* livingOwner = dynamic_cast<LivingEntity*>(owner);
        // MC isAllowedToTeleportOwner: alive (and not sleeping — no sleep
        // system); the cross-dimension branch cannot arise, projectiles do
        // not travel dimensions here.
        const bool mayTeleport =
            owner != nullptr && (livingOwner ? livingOwner->IsAlive()
                                             : owner->IsAlive());
        if (mayTeleport) {
            // MC teleports to oldPosition() — the pearl's pre-impact spot,
            // which is what keeps the thrower out of the wall it hit.
            const glm::dvec3 teleportPos = oldPosition;

            if (owner->IsPlayer()) {
                // MC: 5% endermite at the position the player LEAVES, gated
                // on doMobSpawning.
                if (m_level->Random().NextFloat() < 0.05f &&
                    m_level->DoMobSpawning()) {
                    auto endermite =
                        MakeGenericMob(EntityTypeId::Endermite, m_level);
                    if (endermite) {
                        endermite->position = owner->position;
                        endermite->yRot = endermite->yBodyRot =
                            endermite->yHeadRot = owner->yRot;
                        endermite->FinalizeSpawn(SpawnReason::Triggered, nullptr);
                        m_level->AddFreshEntity(std::move(endermite));
                    }
                }

                // Player movement is client-authoritative, so the move is a
                // packet — EntityLevel::TeleportPlayer, the server bridge's
                // seam. MC: resetFallDistance + 5.0 ender_pearl damage after
                // the teleport (the closest source here is FALL — feather
                // falling reduces both in vanilla).
                if (livingOwner && m_level->TeleportPlayer(*livingOwner, teleportPos)) {
                    livingOwner->Hurt(MobDamageSource::Fall, 5.0f, nullptr);
                }
            } else {
                // MC's non-player branch: move the entity, reset its fall.
                owner->position = teleportPos;
                owner->fallDistance = 0.0f;
                owner->needsSync = true;
            }
            // MC ThrownEnderpearl.playSound: PLAYER_TELEPORT at the landing.
            m_level->PlaySound(nullptr, teleportPos, SoundEvents::PLAYER_TELEPORT, SoundSource::Players, 1.0f, 1.0f);
        }

        Discard();
    }

    // ── Snowball ───────────────────────────────────────────────────────────

    void Snowball::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC Snowball.onHitEntity: 3 vs blazes, 0 otherwise — the zero still
        // counts as a hit (hurt flash + knockback), which is MC's behaviour.
        const float damage = target.GetType() == EntityTypeId::Blaze ? 3.0f : 0.0f;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }
        DealHitDamage(target, hit, MobDamageSource::Projectile, damage,
                      GetOwner() ? GetOwner() : this);
    }

    void Snowball::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            // MC broadcasts entity event 3 for the poof particles; the client
            // has no particle system yet, so the event is a no-op there.
            m_level->BroadcastEntityEvent(*this, 3);
            Discard();
        }
    }

    // ── ThrownEgg ──────────────────────────────────────────────────────────

    void ThrownEgg::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        DealHitDamage(target, hit, MobDamageSource::Projectile, 0.0f,
                      GetOwner() ? GetOwner() : this);
    }

    void ThrownEgg::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (!m_level || m_level->IsClientSide()) return;

        JavaRandom& rng = m_level->Random();
        if (rng.NextInt(8) == 0) {
            int count = 1;
            if (rng.NextInt(32) == 0) count = 4;

            for (int i = 0; i < count; ++i) {
                auto chicken = std::make_unique<Chicken>(m_level);
                chicken->SetAge(AgeableMob::kBabyStartAge);
                chicken->position = position;
                chicken->yRot = yRot;
                m_level->AddFreshEntity(std::move(chicken));
            }
        }

        m_level->BroadcastEntityEvent(*this, 3);
        Discard();
    }

    // ── ThrownSplashPotion (AbstractThrownPotion + splash + lingering) ─────

    ThrownSplashPotion::ThrownSplashPotion(EntityLevel* level)
        : ThrowableProjectile(EntityTypeId::SplashPotion, level),
          // MC ThrownSplashPotion.getDefaultItem — an EMPTY splash potion,
          // which splashes and does nothing (what a bare /summon throws).
          m_item(Items::SplashPotion, 1) {}

    void ThrownSplashPotion::SetItem(const ItemStack& stack) {
        m_item = stack;
        m_item.count = 1;   // ThrowableItemProjectile.setItem: copyWithCount(1)
        if (m_item.IsEmpty()) m_item = ItemStack(Items::SplashPotion, 1);
    }

    bool ThrownSplashPotion::IsLingering() const {
        return m_item.itemId == Items::LingeringPotion;
    }

    void ThrownSplashPotion::OnHitBlock(const HitResult& hit) {
        // MC AbstractThrownPotion.onHitBlock: a douses_fire potion (water)
        // puts out fire, lit candles and lit campfires in the cell in front
        // of the hit face, the hit block itself, and the four horizontal
        // neighbours of the front cell.
        ThrowableProjectile::OnHitBlock(hit);
        if (!m_level || m_level->IsClientSide()) return;

        const PotionContents potion = GetPotionContents(m_item);
        if (!potion.potion || !potion.Is(*potion.potion) || !PotionDousesFire(*potion.potion)) return;

        // The hit face: the cell the projectile arrived from. The hit point
        // sits on the face, so a hair back along the flight is outside it.
        const glm::ivec3 hitPos = hit.blockPos;
        glm::dvec3 back = velocity;
        const double len = glm::length(back);
        back = len > 1e-9 ? back / len : glm::dvec3(0.0, -1.0, 0.0);
        const glm::dvec3 probe = hit.location - back * 1.0e-3;
        glm::ivec3 face(static_cast<int>(std::floor(probe.x)) - hitPos.x,
                        static_cast<int>(std::floor(probe.y)) - hitPos.y,
                        static_cast<int>(std::floor(probe.z)) - hitPos.z);
        if (std::abs(face.x) + std::abs(face.y) + std::abs(face.z) != 1) {
            face = glm::ivec3(0, 1, 0);   // an edge or corner hit: call it the top
        }
        const glm::ivec3 effectPos = hitPos + face;

        DouseFire(effectPos);
        DouseFire(effectPos - face);                       // relative(opposite)
        DouseFire(effectPos + glm::ivec3( 0, 0, -1));      // Direction.Plane.HORIZONTAL:
        DouseFire(effectPos + glm::ivec3( 1, 0,  0));      // north, east, south, west
        DouseFire(effectPos + glm::ivec3( 0, 0,  1));
        DouseFire(effectPos + glm::ivec3(-1, 0,  0));
    }

    void ThrownSplashPotion::DouseFire(const glm::ivec3& pos) {
        // MC AbstractThrownPotion.douseFire.
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return;
        const BlockState state = blocks->GetBlockState(pos.x, pos.y, pos.z);
        const BlockID block = state.Block();
        if (block == BlockID::Fire || block == BlockID::SoulFire) {       // #fire
            m_level->DestroyBlock(pos, false);
            return;
        }
        const std::string_view slug = BlockRegistry::Get(block).registrySlug;
        const auto endsWith = [&](std::string_view suffix) {
            return slug.size() >= suffix.size() &&
                   slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        const bool candle = endsWith("candle") || endsWith("candle_cake");   // #candles, #candle_cakes
        const bool campfire = block == BlockID::Campfire || block == BlockID::SoulCampfire;
        if ((candle || campfire) && state.HasProperty(PropertyId::LIT) && LitOf(state)) {
            // AbstractCandleBlock.extinguish (CANDLE_EXTINGUISH) / the
            // campfire's level event 1009 + CampfireBlock.douse: LIT=false
            // (the smoke particles wait on particles).
            if (candle) {
                m_level->PlaySound(nullptr, pos, SoundEvents::CANDLE_EXTINGUISH, SoundSource::Blocks, 1.0f, 1.0f);
            } else {
                PlayLevelEventSound(*m_level, nullptr, LevelEvent::SOUND_EXTINGUISH_FIRE, pos, 0, &m_level->Random());
            }
            m_level->SetBlockState(pos, WithLit(state, false));
        }
    }

    void ThrownSplashPotion::AffectEntitiesAround(const PotionContents& potion) {
        // MC AbstractThrownPotion.affectEntitiesAround — the water potion's
        // extras. Each tag test is potion.is(TAG): the plain potion, no
        // custom effects.
        const bool plain = potion.potion.has_value() && potion.Is(*potion.potion);
        const bool hurtsWaterSensitive = plain && PotionHurtsWaterSensitive(*potion.potion);
        const bool extinguishes = plain && PotionExtinguishesEntities(*potion.potion);
        const bool rehydrates = plain && *potion.potion == PotionId::Water;   // #rehydrates_axolotls
        if (!hurtsWaterSensitive && !extinguishes && !rehydrates) return;

        AABB box = GetAABB();
        box.min -= glm::vec3(4.0f, 2.0f, 4.0f);
        box.max += glm::vec3(4.0f, 2.0f, 4.0f);
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);

        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living) continue;
            auto* mob = dynamic_cast<Mob*>(living);
            const bool sensitive = mob && mob->SensitiveToWater();
            if ((hurtsWaterSensitive || extinguishes) && (sensitive || living->IsOnFire())) {
                if (DistanceToSqr(*living) < 16.0) {
                    if (hurtsWaterSensitive && sensitive) {
                        // damageSources().indirectMagic(this, owner), 1.0.
                        living->Hurt(MobDamageSource::Magic, 1.0f,
                                     GetOwner() ? GetOwner() : this);
                    }
                    if (extinguishes && living->IsOnFire() && living->IsAlive()) {
                        living->ClearFire();
                    }
                }
            }
            if (rehydrates && living->GetType() == EntityTypeId::Axolotl) {
                // Axolotl.rehydrate: +1800 air, capped at the maximum.
                living->SetAirSupply(std::min(living->GetAirSupply() + 1800,
                                              living->GetMaxAirSupply()));
            }
        }
    }

    void ThrownSplashPotion::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (!m_level || m_level->IsClientSide()) return;

        // MC AbstractThrownPotion.onHit.
        const PotionContents potion = GetPotionContents(m_item);
        AffectEntitiesAround(potion);
        if (potion.HasEffects()) {
            if (IsLingering()) OnHitAsLingering(hit);
            else               OnHitAsSplash(potion, GetPotionDurationScale(m_item), hit);
        }
        // levelEvent 1054/1053, the glass break (2007/2002, the splash
        // particles in potion.getColor(), wait on particles). Entity event 3
        // keeps the client's copy on the same removal path the snowball uses.
        if (!IsSilent()) {
            m_level->PlaySound(nullptr, Sound::BlockCenter(BlockPosition()), SoundEvents::SPLASH_POTION_BREAK,
                               SoundSource::Neutral, 1.0f, m_level->Random().NextFloat() * 0.1f + 0.9f);
        }
        m_level->BroadcastEntityEvent(*this, 3);
        Discard();
    }

    void ThrownSplashPotion::OnHitAsSplash(const PotionContents& contents, float durationScale,
                                           const HitResult& hit) {
        // MC ThrownSplashPotion.onHitAsPotion, transcribed.
        const std::vector<MobEffectInstance> mobEffects = contents.GetAllEffects();

        // potionAabb = getBoundingBox().move(hit.location - position()).
        AABBd potionBox = GetAABBd();
        const glm::dvec3 shift = hit.location - position;
        potionBox.min += shift;
        potionBox.max += shift;

        AABB effectBox = AABB::FromMinMax(glm::vec3(potionBox.min) - glm::vec3(4.0f, 2.0f, 4.0f),
                                          glm::vec3(potionBox.max) + glm::vec3(4.0f, 2.0f, 4.0f));
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(effectBox, this, nearby);

        // ProjectileUtil.computeMargin(this).
        const double margin = std::max(0.0f, std::min(0.3f, static_cast<float>(tickCount - 2) / 20.0f));
        Entity* effectSource = GetOwner() ? GetOwner() : this;   // getEffectSource

        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            // LivingEntity.isAffectedByPotions — false only for the armor stand.
            if (living->GetType() == EntityTypeId::ArmorStand) continue;

            // potionAabb.distanceToSqr(entity box inflated by margin).
            AABBd target = living->GetAABBd();
            target.min -= glm::dvec3(margin);
            target.max += glm::dvec3(margin);
            const double dx = std::max({potionBox.min.x - target.max.x, target.min.x - potionBox.max.x, 0.0});
            const double dy = std::max({potionBox.min.y - target.max.y, target.min.y - potionBox.max.y, 0.0});
            const double dz = std::max({potionBox.min.z - target.max.z, target.min.z - potionBox.max.z, 0.0});
            const double dist = dx * dx + dy * dy + dz * dz;
            if (!(dist < 16.0)) continue;

            const double scale = 1.0 - std::sqrt(dist) / 4.0;
            for (const MobEffectInstance& effect : mobEffects) {
                if (IsInstantenousEffect(effect.effect)) {
                    ApplyInstantenousEffect(this, GetOwner(), *living,
                                            effect.effect, effect.amplifier, scale);
                } else {
                    // mapDuration(d -> (int)(scale * d * durationScale + 0.5)).
                    int duration = effect.duration;
                    if (!effect.IsInfiniteDuration() && duration != 0) {
                        duration = static_cast<int>(scale * static_cast<double>(duration) *
                                                    static_cast<double>(durationScale) + 0.5);
                    }
                    MobEffectInstance scaled(effect.effect, duration, effect.amplifier,
                                             effect.ambient, effect.visible);
                    if (!scaled.EndsWithin(20)) {
                        living->AddEffect(std::move(scaled), effectSource);
                    }
                }
            }
        }
    }

    void ThrownSplashPotion::OnHitAsLingering(const HitResult& hit) {
        // MC ThrownLingeringPotion.onHitAsPotion, transcribed.
        auto cloud = std::make_unique<AreaEffectCloud>(m_level);
        cloud->position = (hit.IsEntity() && hit.entity) ? hit.entity->position : position;
        if (auto* owner = dynamic_cast<LivingEntity*>(GetOwner())) cloud->SetOwner(owner);
        cloud->SetRadius(3.0f);
        cloud->SetRadiusOnUse(-0.5f);
        cloud->SetDuration(600);
        cloud->SetWaitTime(10);
        cloud->SetRadiusPerTick(-cloud->GetRadius() / static_cast<float>(cloud->GetDuration()));
        cloud->ApplyComponentsFromItemStack(m_item);
        m_level->AddFreshEntity(std::move(cloud));
    }

} // namespace Game
