// File: src/common/entity/LightningBolt.cpp
//
// MC net.minecraft.world.entity.LightningBolt — see the header for the
// timeline and what is not modelled.
#include "common/entity/LightningBolt.hpp"
#include "common/entity/decoration/HangingEntity.hpp"

#include "common/sound/SoundEvents.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/fluid/FlowingFluid.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <cmath>
#include <vector>

namespace Game {

    namespace {

        // MC ServerLevel.canSpreadFireAround: the fire_spread_radius_around_
        // player rule (-1 = everywhere), else ChunkMap.anyPlayerCloseEnoughTo
        // — a non-spectator player within that many blocks of the cell's
        // centre.
        bool CanSpreadFireAround(const EntityLevel& level, const glm::ivec3& pos) {
            const int spreadRadius = Rules::GetInt(Rules::Id::FireSpreadRadiusAroundPlayer);
            if (spreadRadius == -1) return true;
            std::vector<LivingEntity*> players;
            level.GetPlayers(players);
            const glm::dvec3 center = glm::dvec3(pos) + glm::dvec3(0.5);
            const double radiusSq = static_cast<double>(spreadRadius) * spreadRadius;
            for (const LivingEntity* player : players) {
                if (!player || player->IsSpectator()) continue;
                const glm::dvec3 d = player->position - center;
                if (glm::dot(d, d) < radiusSq) return true;
            }
            return false;
        }

        // MC `BaseFireBlock.getState(level, pos).canSurvive(level, pos)`.
        //   SoulFireBlock.canSurvive — the block below is a soul-fire base;
        //     getState only returns soul fire when it is, so always true.
        //   FireBlock.canSurvive — the block below has a sturdy top face, or
        //     isValidFireLocation: some neighbour canBurn. FireBlock's
        //     igniteOdds table is not ported (fire spread does not exist yet),
        //     so canBurn reads Properties.ignitedByLava — the same flammable-
        //     material set vanilla builds both tables from.
        bool FireCanSurvive(const ILevelWrite& level, const glm::ivec3& pos, BlockState fire) {
            if (fire.Block() == BlockID::SoulFire) return true;
            const glm::ivec3 below{pos.x, pos.y - 1, pos.z};
            if (IsFaceSturdyAt(level, below, Direction::Up)) return true;
            for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                                Direction::South, Direction::West, Direction::East}) {
                const glm::ivec3 n{pos.x + StepX(d), pos.y + StepY(d), pos.z + StepZ(d)};
                if (!level.IsPositionLoaded(n.x, n.y, n.z)) continue;
                if (BlockRegistry::Get(level.GetBlock(n.x, n.y, n.z)).ignitedByLava) return true;
            }
            return false;
        }

        // MC `level.getBlockState(pos).isAir() && fire.canSurvive(level, pos)`
        // then `setBlockAndUpdate(pos, fire)`.
        bool TryPlaceFire(ILevelWrite& level, const glm::ivec3& pos) {
            if (!level.IsPositionLoaded(pos.x, pos.y, pos.z)) return false;
            const BlockState fire = Fluids::FireStateFor(level, pos);
            if (level.GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) return false;
            if (!FireCanSurvive(level, pos, fire)) return false;
            return level.SetBlock(pos.x, pos.y, pos.z, fire, World::UpdateFlags::All);
        }

    } // namespace

    LightningBolt::LightningBolt(EntityLevel* level)
        : Projectile(EntityTypeId::LightningBolt, level) {
        // MC Entity's random is seeded per entity; the level's stream stands
        // in for the seed source (RandomSource.create()).
        if (level) m_random.SetSeed(level->Random().NextLong());
        // MC LightningBolt(type, level).
        m_seed = m_random.NextLong();
        m_flashes = m_random.NextInt(3) + 1;
    }

    void LightningBolt::Tick() {
        if (!m_level) return;
        Entity::BaseTick();   // MC super.tick()

        const bool clientSide = m_level->IsClientSide();

        if (m_life == 2) {
            if (clientSide) {
                // MC playLocalSound(..., LIGHTNING_BOLT_THUNDER, WEATHER,
                // 10000, 0.8 + r*0.2) and (..., LIGHTNING_BOLT_IMPACT, WEATHER,
                // 2, 0.5 + r*0.2), unless silent. Client-local: every client
                // simulates the bolt it was sent and makes its own thunder.
                // The random draws happen either way so the stream matches.
                const float thunderPitch = 0.8f + m_random.NextFloat() * 0.2f;
                const float impactPitch = 0.5f + m_random.NextFloat() * 0.2f;
                if (!IsSilent()) {
                    m_level->PlayLocalSound(position, SoundEvents::LIGHTNING_BOLT_THUNDER, SoundSource::Weather,
                                            10000.0f, thunderPitch, false);
                    m_level->PlayLocalSound(position, SoundEvents::LIGHTNING_BOLT_IMPACT, SoundSource::Weather,
                                            2.0f, impactPitch, false);
                }
            } else {
                const Difficulty difficulty = m_level->GetDifficulty();
                if (difficulty == Difficulty::Normal || difficulty == Difficulty::Hard) {
                    SpawnFire(4);
                }
                // MC powerLightningRod (LightningRodBlock.onLightningStrike
                // at getStrikePosition — BlockPos.containing(x, y - 1e-6, z))
                // and clearCopperOnLightningStrike: SKIPPED — see the header.
                // MC gameEvent(GameEvent.LIGHTNING_STRIKE): no game events.
            }
        }

        --m_life;
        if (m_life < 0) {
            if (m_flashes == 0) {
                // MC fires the LIGHTNING_STRIKE advancement trigger for every
                // player within 256 blocks here — no advancements.
                Discard();
            } else if (m_life < -m_random.NextInt(10)) {
                --m_flashes;
                m_life = 1;
                m_seed = m_random.NextLong();
                SpawnFire(0);
            }
        }

        if (m_life >= 0) {
            if (clientSide) {
                // MC level().setSkyFlashTime(2) — ClientLevel's sky flash.
                m_level->SetSkyFlashTime(2);
            } else if (!m_visualOnly) {
                // MC getEntities(this, AABB(x-3, y-3, z-3, x+3, y+6+3, z+3),
                // Entity::isAlive) → thunderHit each.
                const AABB box = AABB::FromMinMax(
                    glm::vec3(static_cast<float>(position.x - kDamageRadius),
                              static_cast<float>(position.y - kDamageRadius),
                              static_cast<float>(position.z - kDamageRadius)),
                    glm::vec3(static_cast<float>(position.x + kDamageRadius),
                              static_cast<float>(position.y + 6.0 + kDamageRadius),
                              static_cast<float>(position.z + kDamageRadius)));
                std::vector<Entity*> entities;
                m_level->GetEntitiesInBox(box, this, entities);
                for (Entity* entity : entities) {
                    if (entity && entity->IsAlive()) ThunderHit(*entity);
                }
                // MC hitEntities.addAll + CHANNELED_LIGHTNING for `cause`:
                // advancement-only, not modelled.
            }
        }
    }

    void LightningBolt::SpawnFire(int additionalSources) {
        if (m_visualOnly || !m_level || m_level->IsClientSide()) return;
        ILevelWrite* level = m_level->MutableBlocks();
        if (!level) return;

        const glm::ivec3 pos = BlockPosition();
        if (!CanSpreadFireAround(*m_level, pos)) return;

        if (TryPlaceFire(*level, pos)) ++m_blocksSetOnFire;

        for (int i = 0; i < additionalSources; ++i) {
            // MC pos.offset(r(3)-1, r(3)-1, r(3)-1) — x, y, z draw order.
            const int dx = m_random.NextInt(3) - 1;
            const int dy = m_random.NextInt(3) - 1;
            const int dz = m_random.NextInt(3) - 1;
            if (TryPlaceFire(*level, pos + glm::ivec3(dx, dy, dz))) ++m_blocksSetOnFire;
        }
    }

    void LightningBolt::ThunderHit(Entity& victim) {
        // MC BlockAttachedEntity.thunderHit is empty: lightning neither burns
        // nor breaks a painting or an item frame.
        if (dynamic_cast<const HangingEntity*>(&victim)) return;
        // MC Entity.thunderHit:
        //     setRemainingFireTicks(remainingFireTicks + 1);
        //     if (remainingFireTicks == 0) igniteForSeconds(8);
        //     hurtServer(level, damageSources().lightningBolt(), 5);
        // MC's resting "not burning" value is -getFireImmuneTicks() = -1,
        // stamped by Entity.move; the engine rests at 0. So "+1 lands on 0"
        // is "was not burning" here, and a burning entity gains its one tick.
        const int fireTicks = victim.GetRemainingFireTicks();
        if (fireTicks <= 0) {
            victim.IgniteForSeconds(8);
        } else {
            victim.SetRemainingFireTicks(fireTicks + 1);
        }
        // A player's fire lives on ServerPlayer, which the entity view does
        // not forward to (the same gap every mob-side ignition has); the
        // damage does reach the player through PlayerEntityView::Hurt.
        if (LivingEntity* living = victim.AsLiving()) {
            living->Hurt(MobDamageSource::Generic, kThunderDamage, nullptr);
        }
    }

} // namespace Game
