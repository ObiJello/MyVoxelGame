// File: src/common/entity/LightningBolt.cpp
//
// MC net.minecraft.world.entity.LightningBolt — see the header for the
// timeline and what is not modelled.
#include "common/entity/LightningBolt.hpp"
#include <algorithm>
#include "server/advancements/CriteriaTriggers.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/entity/decoration/HangingEntity.hpp"
#include "common/entity/decoration/BlockAttachedEntity.hpp"

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
#include "common/sound/LevelEventSounds.hpp"
#include "common/world/block/CopperChestBlock.hpp"     // the copper weathering families
#include "common/world/block/RedstoneComponents.hpp"  // LightningRodOnLightningStrike

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
                PowerLightningRod();
                ClearCopperOnLightningStrike();
                // MC gameEvent(GameEvent.LIGHTNING_STRIKE).
                GameEvent(GameEventId::LightningStrike);
            }
        }

        --m_life;
        if (m_life < 0) {
            if (m_flashes == 0) {
                // MC: every live entity within 15 blocks the strike did NOT
                // hit is a bystander; every player within 256 blocks gets
                // CriteriaTriggers.LIGHTNING_STRIKE with them.
                if (!clientSide) {
                    const AABB around = AABB::FromMinMax(
                        glm::vec3(static_cast<float>(position.x - kDetectionRadius),
                                  static_cast<float>(position.y - kDetectionRadius),
                                  static_cast<float>(position.z - kDetectionRadius)),
                        glm::vec3(static_cast<float>(position.x + kDetectionRadius),
                                  static_cast<float>(position.y + 6.0 + kDetectionRadius),
                                  static_cast<float>(position.z + kDetectionRadius)));
                    std::vector<Entity*> nearby;
                    m_level->GetEntitiesInBox(around, this, nearby);
                    std::vector<Entity*> bystanders;
                    for (Entity* e : nearby) {
                        if (!e || !e->IsAlive()) continue;
                        if (std::find(m_hitEntityIds.begin(), m_hitEntityIds.end(), e->GetId()) != m_hitEntityIds.end()) continue;
                        bystanders.push_back(e);
                    }
                    std::vector<LivingEntity*> players;
                    m_level->GetPlayers(players);
                    for (LivingEntity* p : players) {
                        if (!p || DistanceTo(*p) >= 256.0) continue;
                        if (Server::ServerPlayer* player = Server::CriteriaTriggers::PlayerOf(p)) {
                            Server::CriteriaTriggers::LightningStrike(*player, *this, bystanders);
                        }
                    }
                }
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
                std::vector<Entity*> struck;
                for (Entity* entity : entities) {
                    if (entity && entity->IsAlive()) {
                        entity->ThunderHit(this);
                        struck.push_back(entity);
                    }
                }
                // MC hitEntities.addAll(entities), then CHANNELED_LIGHTNING
                // for the channeling player.
                for (Entity* e : struck) m_hitEntityIds.push_back(e->GetId());
                if (m_causeId != 0) {
                    if (Server::ServerPlayer* cause = Server::CriteriaTriggers::PlayerOf(m_level->ResolveEntityById(m_causeId))) {
                        Server::CriteriaTriggers::ChanneledLightning(*cause, struck);
                    }
                }
            }
        }
    }

    glm::ivec3 LightningBolt::GetStrikePosition() const {
        // MC getStrikePosition: BlockPos.containing(x, y - 1.0E-6, z) — the
        // block the bolt stands on.
        return glm::ivec3(static_cast<int>(std::floor(position.x)),
                          static_cast<int>(std::floor(position.y - 1.0e-6)),
                          static_cast<int>(std::floor(position.z)));
    }

    void LightningBolt::PowerLightningRod() {
        // MC powerLightningRod: a lightning rod under the strike takes it.
        ILevelWrite* level = m_level ? m_level->MutableBlocks() : nullptr;
        if (!level) return;
        const glm::ivec3 strike = GetStrikePosition();
        if (!level->IsPositionLoaded(strike.x, strike.y, strike.z)) return;
        const BlockState below = level->GetBlockState(strike.x, strike.y, strike.z);
        if (IsLightningRodBlock(below.Block())) LightningRodOnLightningStrike(*level, strike, below);
    }

    void LightningBolt::ClearCopperOnLightningStrike() {
        // MC clearCopperOnLightningStrike(level, getStrikePosition()): a
        // strike on weathering copper scrapes it back to new, then 3..5
        // random walks of 1..8 steps each clean one stage off the copper
        // they cross (a waxed block is struck through: it starts the walks
        // but keeps its own stage).
        ILevelWrite* level = m_level ? m_level->MutableBlocks() : nullptr;
        if (!level) return;
        JavaRandom* random = level->Random();
        if (!random) return;
        const glm::ivec3 struck = GetStrikePosition();
        if (!level->IsPositionLoaded(struck.x, struck.y, struck.z)) return;
        const BlockState struckState = level->GetBlockState(struck.x, struck.y, struck.z);
        const bool isWaxed = IsWaxedCopperBlock(struckState.Block());
        const bool isWeatheringCopper = WeatheringCopperAge(struckState.Block()) >= 0;
        if (!isWeatheringCopper && !isWaxed) return;
        if (isWeatheringCopper) {
            level->SetBlock(struck.x, struck.y, struck.z, WeatheringCopperFirstState(struckState),
                            World::UpdateFlags::All);
        }

        // randomStepCleaningCopper: up to 10 random cells of the 3x3x3 cube
        // around the walker; the first weathering copper one steps back a
        // stage (getPrevious, if any), sparks (level event 3002, data -1)
        // and becomes the walker's next position.
        const auto randomStep = [&](const glm::ivec3& from, glm::ivec3& to) -> bool {
            for (int i = 0; i < 10; ++i) {
                const glm::ivec3 candidate(from.x - 1 + random->NextInt(3),
                                           from.y - 1 + random->NextInt(3),
                                           from.z - 1 + random->NextInt(3));
                if (!level->IsPositionLoaded(candidate.x, candidate.y, candidate.z)) continue;
                const BlockState state = level->GetBlockState(candidate.x, candidate.y, candidate.z);
                if (WeatheringCopperAge(state.Block()) < 0) continue;
                const BlockState previous = WeatheringCopperPreviousState(state);
                if (previous.RawId() != state.RawId()) {
                    level->SetBlock(candidate.x, candidate.y, candidate.z, previous, World::UpdateFlags::All);
                }
                level->PlayLevelEvent(SoundExcept(nullptr), LevelEvent::PARTICLES_ELECTRIC_SPARK, candidate, -1);
                to = candidate;
                return true;
            }
            return false;
        };

        const int strikesCount = random->NextInt(3) + 3;
        for (int strike = 0; strike < strikesCount; ++strike) {
            const int stepCount = random->NextInt(8) + 1;
            // randomWalkCleaningCopper: every walk starts at the strike.
            glm::ivec3 walker = struck;
            for (int step = 0; step < stepCount; ++step) {
                glm::ivec3 next;
                if (!randomStep(walker, next)) break;
                walker = next;
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
        victim.ThunderHit(nullptr);
    }

    void Entity::ThunderHit(Entity* bolt) {
        (void)bolt;
        // MC Entity.thunderHit:
        //     setRemainingFireTicks(remainingFireTicks + 1);
        //     if (remainingFireTicks == 0) igniteForSeconds(8);
        //     hurtServer(level, damageSources().lightningBolt(), 5);
        // MC's resting "not burning" value is -getFireImmuneTicks() = -1,
        // stamped by Entity.move; the engine rests at 0. So "+1 lands on 0"
        // is "was not burning" here, and a burning entity gains its one tick.
        const int fireTicks = GetRemainingFireTicks();
        if (fireTicks <= 0) {
            IgniteForSeconds(8);
        } else {
            SetRemainingFireTicks(fireTicks + 1);
        }
        // A player's fire lives on ServerPlayer, which the entity view does
        // not forward to (the same gap every mob-side ignition has); the
        // damage does reach the player through PlayerEntityView::Hurt.
        if (LivingEntity* living = AsLiving()) {
            living->Hurt(MobDamageSource::Lightning, LightningBolt::kThunderDamage, nullptr);
        }
    }

} // namespace Game
