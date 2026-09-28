// File: src/common/entity/OminousItemSpawner.cpp
//
// MC OminousItemSpawner — see the header.
#include "common/entity/OminousItemSpawner.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC ProjectileItem.DispenseConfig: the shot's power and uncertainty
        // and the level event it plays instead of the dispenser's.
        enum class DispenseEvent : uint8_t {
            None,
            BlazeFireball,   // LevelEvent 1018 (FireChargeItem)
            WindChargeShoot, // LevelEvent 1051 (WindChargeItem)
        };
        struct DispenseConfig {
            float power = 1.1f;          // DispenseConfig.DEFAULT
            float uncertainty = 6.0f;
            DispenseEvent overrideDispenseEvent = DispenseEvent::None;
        };

        // The sound half of 1018 / 1051 (LevelEventHandler), sent as the
        // plain sound it plays — the engine's level-event packets carry
        // particles; their sounds travel as sound packets
        // (LevelEventSoundIsNetworked).
        void PlayDispenseEvent(EntityLevel& level, DispenseEvent event, const glm::ivec3& pos) {
            JavaRandom& r = level.Random();
            switch (event) {
                case DispenseEvent::BlazeFireball: {
                    const float a = r.NextFloat();
                    const float pitch = (a - r.NextFloat()) * 0.2f + 1.0f;
                    level.PlaySound(nullptr, pos, SoundEvents::BLAZE_SHOOT, SoundSource::Hostile, 2.0f, pitch);
                    break;
                }
                case DispenseEvent::WindChargeShoot:
                    level.PlaySound(nullptr, pos, SoundEvents::WIND_CHARGE_THROW, SoundSource::Blocks, 0.5f,
                                    0.4f / (r.NextFloat() * 0.4f + 0.8f));
                    break;
                case DispenseEvent::None:
                    break;
            }
        }

        // MC ProjectileItem.asProjectile(level, position, item, DOWN) with
        // createDispenseConfig, for every ProjectileItem this engine has an
        // entity for. Null for any other item — it drops as an item entity.
        std::unique_ptr<Projectile> AsProjectile(EntityLevel& level, const glm::dvec3& position,
                                                 const ItemStack& item, DispenseConfig& config) {
            const ItemID id = item.itemId;
            if (id == Items::Arrow || id == Items::TippedArrow) {
                // ArrowItem / TippedArrowItem: an Arrow carrying the stack
                // (a tipped arrow's contents and duration scale ride on it).
                auto arrow = std::make_unique<Arrow>(&level);
                if (id == Items::TippedArrow) arrow->SetPotionFromPickupStack(item);
                // ArrowItem.asProjectile: pickup ALLOWED, the stack its item.
                arrow->SetPickupItemStack(item);
                arrow->SetPickup(Arrow::Pickup::Allowed);
                arrow->position = position;
                return arrow;
            }
            if (id == Items::SplashPotion || id == Items::LingeringPotion) {
                // ThrowablePotionItem: DEFAULT uncertainty x0.5, power x1.25.
                auto potion = std::make_unique<ThrownSplashPotion>(&level);
                potion->SetItem(item);
                potion->position = position;
                config.uncertainty = 6.0f * 0.5f;
                config.power = 1.1f * 1.25f;
                return potion;
            }
            if (id == Items::Snowball) {
                auto snowball = std::make_unique<Snowball>(&level);
                snowball->position = position;
                return snowball;
            }
            if (id == Items::Egg || id == Items::BlueEgg || id == Items::BrownEgg) {
                auto egg = std::make_unique<ThrownEgg>(&level);
                egg->position = position;
                return egg;
            }
            if (id == Items::Trident) {
                // TridentItem.asProjectile: a ThrownTrident of the stack
                // (copyWithCount(1) — its Loyalty and glint), pickup ALLOWED
                // (DEFAULT config).
                auto trident = std::make_unique<ThrownTrident>(&level);
                trident->position = position;
                trident->SetTridentItem(item);
                trident->SetPickup(Arrow::Pickup::Allowed);
                return trident;
            }
            if (id == Items::FireCharge || id == Items::WindCharge) {
                // FireChargeItem / WindChargeItem.asProjectile: the direction
                // is DOWN spread by triangle(step, 0.11485) per axis — drawn
                // from the level random whether or not shoot() then replaces
                // the motion, which it does.
                JavaRandom& random = level.Random();
                const double dirX = random.Triangle(0.0, 0.11485000000000001);
                const double dirY = random.Triangle(-1.0, 0.11485000000000001);
                const double dirZ = random.Triangle(0.0, 0.11485000000000001);
                glm::dvec3 dir(dirX, dirY, dirZ);
                config.uncertainty = 6.6666665f;
                config.power = 1.0f;
                if (id == Items::FireCharge) {
                    config.overrideDispenseEvent = DispenseEvent::BlazeFireball;
                    auto fireball = std::make_unique<SmallFireball>(&level);
                    fireball->position = position;
                    const double len = glm::length(dir);
                    // assignDirectionalMovement(direction.normalize(), accelerationPower).
                    if (len > 1.0e-9) fireball->velocity = dir / len * fireball->GetAccelerationPower();
                    return fireball;
                }
                config.overrideDispenseEvent = DispenseEvent::WindChargeShoot;
                auto charge = std::make_unique<WindCharge>(&level);
                charge->position = position;
                charge->velocity = dir;   // windCharge.setDeltaMovement(dir)
                return charge;
            }
            return nullptr;
        }

    } // namespace

    OminousItemSpawner::OminousItemSpawner(EntityLevel* level)
        : Projectile(EntityTypeId::OminousItemSpawner, level) {
        // MC: this.noPhysics = true — it hangs where it was put.
        SetNoGravity(true);
    }

    std::unique_ptr<OminousItemSpawner> OminousItemSpawner::Create(EntityLevel& level, const ItemStack& item) {
        auto spawner = std::make_unique<OminousItemSpawner>(&level);
        // RandomSource.nextIntBetweenInclusive(60, 120).
        spawner->m_spawnItemAfterTicks =
            static_cast<int64_t>(level.Random().NextInt(kSpawnItemDelayMax - kSpawnItemDelayMin + 1) +
                                 kSpawnItemDelayMin);
        spawner->SetItem(item);
        return spawner;
    }

    void OminousItemSpawner::SetItem(const ItemStack& item) {
        m_item = item;
        m_itemDirty = true;
    }

    void OminousItemSpawner::Tick() {
        if (!m_level) return;
        Entity::BaseTick();   // MC super.tick(): tickCount and the base bookkeeping
        oldPosition = position;
        if (m_level->IsClientSide()) TickClient();
        else                         TickServer();
    }

    void OminousItemSpawner::TickServer() {
        if (static_cast<int64_t>(tickCount) == m_spawnItemAfterTicks - kTicksBeforeAboutToSpawnSound) {
            m_level->PlaySound(nullptr, BlockPosition(), SoundEvents::TRIAL_SPAWNER_ABOUT_TO_SPAWN_ITEM,
                               SoundSource::Neutral);
        }
        if (static_cast<int64_t>(tickCount) >= m_spawnItemAfterTicks) {
            SpawnItem();
            // MC kill(level): removed as KILLED, with the ENTITY_DIE game event.
            GameEvent(GameEventId::EntityDie, this);
            Remove(RemovalReason::Killed);
        }
    }

    void OminousItemSpawner::TickClient() {
        // MC: every fifth game tick, addParticles — 1..3 OMINOUS_SPAWNING
        // motes launched from the item toward a gaussian point around it
        // (the particle flies back in, OminousSpawningParticle).
        if (m_level->GetGameTime() % 5 != 0) return;
        JavaRandom& random = m_level->Random();
        const int count = random.NextInt(3) + 1;   // nextIntBetweenInclusive(1, 3)
        for (int i = 0; i < count; ++i) {
            const double fromX = position.x + 0.4 * (random.NextGaussian() - random.NextGaussian());
            const double fromY = position.y + 0.4 * (random.NextGaussian() - random.NextGaussian());
            const double fromZ = position.z + 0.4 * (random.NextGaussian() - random.NextGaussian());
            m_level->AddParticle(ParticleKind::OminousSpawning, position.x, position.y, position.z,
                                 fromX - position.x, fromY - position.y, fromZ - position.z);
        }
    }

    void OminousItemSpawner::SpawnItem() {
        if (m_item.IsEmpty()) return;
        EntityLevel& level = *m_level;
        const glm::ivec3 blockPos = BlockPosition();
        Entity* spawned = nullptr;

        DispenseConfig config;
        if (std::unique_ptr<Projectile> projectile = AsProjectile(level, position, m_item, config)) {
            // MC spawnProjectile: the override event, then
            // Projectile.spawnProjectileUsingShoot straight DOWN at the
            // item's power and uncertainty, owned by this spawner.
            PlayDispenseEvent(level, config.overrideDispenseEvent, blockPos);
            projectile->Shoot(0.0, -1.0, 0.0, config.power, config.uncertainty);
            projectile->SetOwner(this);
            spawned = projectile.get();
            level.AddFreshEntity(std::move(projectile));
        } else {
            // new ItemEntity(level, x, y, z, item): MC's default hop —
            // (nextDouble * 0.2 - 0.1, 0.2, nextDouble * 0.2 - 0.1), no
            // pickup delay.
            JavaRandom& random = level.Random();
            const double vx = random.NextDouble() * 0.2 - 0.1;
            const double vz = random.NextDouble() * 0.2 - 0.1;
            SpawnItemEntity(level.Dimension(), position, glm::dvec3(vx, 0.2, vz), m_item, 0);
        }

        // levelEvent 3021: the spawn-item sound and the ominous (soul fire)
        // burst, data 1 = FlameParticle.OMINOUS.
        level.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_TRIAL_SPAWNER_SPAWN_ITEM, blockPos, 1);
        if (ILevelWrite* blocks = level.MutableBlocks()) {
            blocks->GameEvent(spawned, GameEventId::EntityPlace, position);
        }
        SetItem(ItemStack{});
    }

} // namespace Game
