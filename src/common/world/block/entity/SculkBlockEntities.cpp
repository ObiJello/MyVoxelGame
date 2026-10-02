// File: src/common/world/block/entity/SculkBlockEntities.cpp
#include "SculkBlockEntities.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/WardenSpawnTracker.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"     // Warden
#include "common/entity/projectile/Projectile.hpp"
#include "common/core/Mth.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/SculkBlocks.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <cmath>

namespace Game {

    namespace {

        glm::ivec3 BlockContaining(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }

        // What every listening block entity does first each server tick:
        // assert its registration (GameEventDispatcher's registration note),
        // then report whether its chunk is ticking — MC runs a block
        // entity's ticker only there (LevelChunk.isTicking).
        bool PrepareListenerTick(World& world, const glm::ivec3& pos) {
            if (GameEventDispatcher* dispatcher = world.GameEvents()) dispatcher->EnsureBlockListener(pos);
            return world.ShouldTickBlocksAt(pos);
        }

        // MC Mth.randomBetweenInclusive.
        int RandomBetweenInclusive(JavaRandom& random, int min, int maxInclusive) {
            return random.NextInt(maxInclusive - min + 1) + min;
        }

        // MC SpawnUtil.Strategy.ON_TOP_OF_COLLIDER: nothing to collide with
        // above, a full top face below.
        bool OnTopOfColliderCanSpawnOn(BlockState state, BlockState above) {
            const bool aboveEmpty = !BlockRegistry::HasCollision(above.Block()) ||
                                    BlockRegistry::GetBlockCollisionShapeSet(above).count == 0;
            if (!aboveEmpty) return false;
            return BlockRegistry::HasCollision(state.Block()) &&
                   BlockRegistry::GetBlockCollisionShapeSet(state).IsFaceSturdyUp();
        }

        // MC SpawnUtil.moveToPossibleSpawnPosition: from `spawnRangeY` above
        // down to `spawnRangeY` below, the first cell whose floor the
        // strategy accepts; `searchPos` ends on the cell above that floor.
        bool MoveToPossibleSpawnPosition(const IBlockAccess& level, int spawnRangeY, glm::ivec3& searchPos) {
            BlockState aboveState = level.GetBlockState(searchPos.x, searchPos.y, searchPos.z);
            for (int y = spawnRangeY; y >= -spawnRangeY; --y) {
                searchPos.y -= 1;
                const BlockState current = level.GetBlockState(searchPos.x, searchPos.y, searchPos.z);
                if (OnTopOfColliderCanSpawnOn(current, aboveState)) {
                    searchPos.y += 1;
                    return true;
                }
                aboveState = current;
            }
            return false;
        }

    } // namespace

    // ══ Sculk sensor ════════════════════════════════════════════════════════

    SculkSensorBlockEntity::SculkSensorBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
        : BlockEntity(type, worldPos, blockId), m_user(std::make_unique<User>(*this)), m_listener(*this) {}

    void SculkSensorBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        if (!PrepareListenerTick(*world, GetWorldPos())) return;
        VibrationTicker::Tick(*world, m_data, *m_user);
    }

    bool SculkSensorBlockEntity::User::CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                                           const GameEventContext& /*context*/) {
        const glm::ivec3& blockPos = m_owner.GetWorldPos();
        // Its own block being placed or broken is not something it hears.
        if (pos == blockPos && (event == GameEventId::BlockDestroy || event == GameEventId::BlockPlace)) {
            return false;
        }
        if (GameEvents::Frequency(event) == 0) return false;
        return SculkSensor::CanActivate(level.GetBlockState(blockPos.x, blockPos.y, blockPos.z));
    }

    void SculkSensorBlockEntity::User::OnReceiveVibration(World& level, const glm::ivec3& /*pos*/, GameEventId event,
                                                          Entity* sourceEntity, Entity* /*projectileOwner*/,
                                                          float receivingDistance) {
        const glm::ivec3 blockPos = m_owner.GetWorldPos();
        const BlockState state = level.GetBlockState(blockPos.x, blockPos.y, blockPos.z);
        if (!SculkSensor::CanActivate(state)) return;
        const int eventFrequency = GameEvents::Frequency(event);
        m_owner.SetLastVibrationFrequency(eventFrequency);
        const int calculatedPower = Vibrations::RedstoneStrengthForDistance(receivingDistance, GetListenerRadius());
        SculkSensor::Activate(sourceEntity, level, blockPos, state, calculatedPower, eventFrequency);
    }

    // ══ Calibrated sculk sensor ═════════════════════════════════════════════

    CalibratedSculkSensorBlockEntity::CalibratedSculkSensorBlockEntity(const BlockEntityType* type,
                                                                       glm::ivec3 worldPos, BlockID blockId)
        : SculkSensorBlockEntity(type, worldPos, blockId) {
        SetUser(std::make_unique<CalibratedUser>(*this));
    }

    bool CalibratedSculkSensorBlockEntity::CalibratedUser::CanReceiveVibration(World& level, const glm::ivec3& pos,
                                                                               GameEventId event,
                                                                               const GameEventContext& context) {
        // MC getBackSignal: the signal reaching it from behind (the side
        // opposite its facing) picks the one frequency it hears.
        const glm::ivec3& blockPos = m_owner.GetWorldPos();
        const BlockState state = level.GetBlockState(blockPos.x, blockPos.y, blockPos.z);
        const Direction back = Opposite(HorizontalFacingOf(state));
        const int comparisonType = GetSignal(level, Relative(blockPos, back), back);
        if (comparisonType != 0 && GameEvents::Frequency(event) != comparisonType) return false;
        return User::CanReceiveVibration(level, pos, event, context);
    }

    // ══ Sculk shrieker ══════════════════════════════════════════════════════

    SculkShriekerBlockEntity::SculkShriekerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos,
                                                       BlockID blockId)
        : BlockEntity(type, worldPos, blockId), m_user(*this), m_listener(*this) {}

    void SculkShriekerBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        if (!PrepareListenerTick(*world, GetWorldPos())) return;
        VibrationTicker::Tick(*world, m_data, m_user);
    }

    LivingEntity* SculkShriekerBlockEntity::TryGetPlayer(Entity* sourceEntity) {
        if (!sourceEntity) return nullptr;
        if (sourceEntity->IsPlayer()) return sourceEntity->AsLiving();
        if (Entity* rider = sourceEntity->GetControllingPassenger(); rider && rider->IsPlayer()) {
            return rider->AsLiving();
        }
        if (auto* projectile = dynamic_cast<Projectile*>(sourceEntity)) {
            if (Entity* owner = projectile->GetOwner(); owner && owner->IsPlayer()) return owner->AsLiving();
        }
        // (MC's ItemEntity owner branch: dropped items are not entities that
        // raise game events in this engine.)
        return nullptr;
    }

    bool SculkShriekerBlockEntity::User::CanReceiveVibration(World& level, const glm::ivec3& /*pos*/,
                                                             GameEventId /*event*/, const GameEventContext& context) {
        const glm::ivec3& p = m_owner.GetWorldPos();
        const BlockState state = level.GetBlockState(p.x, p.y, p.z);
        return !BoolOf(state, PropertyId::SHRIEKING) && TryGetPlayer(context.sourceEntity) != nullptr;
    }

    void SculkShriekerBlockEntity::User::OnReceiveVibration(World& level, const glm::ivec3& /*pos*/,
                                                            GameEventId /*event*/, Entity* sourceEntity,
                                                            Entity* projectileOwner, float /*receivingDistance*/) {
        m_owner.TryShriek(level, TryGetPlayer(projectileOwner ? projectileOwner : sourceEntity));
    }

    void SculkShriekerBlockEntity::TryShriek(World& level, LivingEntity* player) {
        if (!player) return;
        const glm::ivec3& p = GetWorldPos();
        const BlockState state = level.GetBlockState(p.x, p.y, p.z);
        if (state.Block() != BlockID::SculkShrieker || BoolOf(state, PropertyId::SHRIEKING)) return;
        m_warningLevel = 0;
        if (!CanRespond(level, state) || TryToWarn(level, *player)) Shriek(level, player);
    }

    bool SculkShriekerBlockEntity::TryToWarn(World& level, LivingEntity& player) {
        EntityLevel* entities = level.Entities();
        if (!entities) return false;
        const std::optional<int> warningLevel = WardenSpawnTracker::TryWarn(*entities, GetWorldPos(), player);
        if (warningLevel) m_warningLevel = *warningLevel;
        return warningLevel.has_value();
    }

    void SculkShriekerBlockEntity::Shriek(World& level, Entity* sourceEntity) {
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::SHRIEKING, true), 2);
        if (ScheduledTickAccess* ticks = level.Ticks()) ticks->ScheduleTick(pos, state.Block(), kShriekingTicks);
        level.PlayLevelEvent(SoundExcept(nullptr), LevelEvent::PARTICLES_SCULK_SHRIEK, pos, 0);
        level.GameEvent(GameEventId::Shriek, pos, GameEventContext::Of(sourceEntity));
        MarkDirty();
    }

    bool SculkShriekerBlockEntity::CanRespond(World& level, BlockState state) const {
        return state.Block() == BlockID::SculkShrieker && BoolOf(state, PropertyId::CAN_SUMMON) &&
               level.GetDifficulty() != Difficulty::Peaceful && Rules::GetBool(Rules::Id::SpawnWardens);
    }

    void SculkShriekerBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& /*pos*/,
                                                        BlockState oldState) {
        // MC reads the entity's own (pre-removal) state here, which is the
        // shrieker being broken — the level already holds its replacement.
        if (!BoolOf(oldState, PropertyId::SHRIEKING)) return;
        if (auto* world = dynamic_cast<World*>(&level)) TryRespond(*world, oldState);
    }

    void SculkShriekerBlockEntity::TryRespond(World& level) {
        const glm::ivec3& p = GetWorldPos();
        TryRespond(level, level.GetBlockState(p.x, p.y, p.z));
    }

    void SculkShriekerBlockEntity::TryRespond(World& level, BlockState state) {
        if (!CanRespond(level, state) || m_warningLevel <= 0) return;
        if (!TrySummonWarden(level)) PlayWardenReplySound(level);
        // MC Warden.applyDarknessAround(level, center, null, 40).
        if (EntityLevel* entities = level.Entities()) {
            const glm::ivec3& p = GetWorldPos();
            AddEffectToPlayersAround(*entities, nullptr, glm::dvec3(p.x + 0.5, p.y + 0.5, p.z + 0.5),
                                     static_cast<double>(kDarknessRadius),
                                     MobEffectInstance(MobEffectId::Darkness, 260, 0, /*ambient=*/false,
                                                       /*visible=*/false),
                                     200);
        }
    }

    void SculkShriekerBlockEntity::PlayWardenReplySound(World& level) {
        // MC SOUND_BY_LEVEL.
        const char* sound = nullptr;
        switch (m_warningLevel) {
            case 1: sound = SoundEvents::WARDEN_NEARBY_CLOSE; break;
            case 2: sound = SoundEvents::WARDEN_NEARBY_CLOSER; break;
            case 3: sound = SoundEvents::WARDEN_NEARBY_CLOSEST; break;
            case 4: sound = SoundEvents::WARDEN_LISTENING_ANGRY; break;
            default: return;
        }
        JavaRandom* random = level.Random();
        if (!random) return;
        const glm::ivec3& pos = GetWorldPos();
        const int x = pos.x + RandomBetweenInclusive(*random, -kWarningSoundRadius, kWarningSoundRadius);
        const int y = pos.y + RandomBetweenInclusive(*random, -kWarningSoundRadius, kWarningSoundRadius);
        const int z = pos.z + RandomBetweenInclusive(*random, -kWarningSoundRadius, kWarningSoundRadius);
        level.PlaySound(SoundExcept(nullptr), glm::dvec3(x, y, z), sound, SoundSource::Hostile, 5.0f, 1.0f);
    }

    bool SculkShriekerBlockEntity::TrySummonWarden(World& level) {
        if (m_warningLevel < WardenSpawnTracker::kMaxWarningLevel) return false;
        // MC SpawnUtil.trySpawnMob(WARDEN, TRIGGERED, level, pos, 20, 5, 6,
        // ON_TOP_OF_COLLIDER, false).
        EntityLevel* entities = level.Entities();
        JavaRandom* random = level.Random();
        if (!entities || !random) return false;
        const glm::ivec3 start = GetWorldPos();
        for (int attempt = 0; attempt < kWardenSpawnAttempts; ++attempt) {
            const int dx = RandomBetweenInclusive(*random, -kWardenSpawnRangeXZ, kWardenSpawnRangeXZ);
            const int dz = RandomBetweenInclusive(*random, -kWardenSpawnRangeXZ, kWardenSpawnRangeXZ);
            glm::ivec3 search(start.x + dx, start.y + kWardenSpawnRangeY, start.z + dz);
            if (!MoveToPossibleSpawnPosition(level, kWardenSpawnRangeY, search)) continue;

            // EntityType.create: snapped to the cell centre, a random yaw,
            // then finalizeSpawn(TRIGGERED) — which starts the emerge.
            auto warden = std::make_unique<Warden>(entities);
            warden->position = glm::dvec3(search.x + 0.5, search.y, search.z + 0.5);
            warden->oldPosition = warden->position;
            warden->yRot = Mth::WrapDegrees(random->NextFloat() * 360.0f);
            warden->SetYHeadRot(warden->yRot);
            warden->yBodyRot = warden->yRot;
            warden->FinalizeSpawn(SpawnReason::Triggered, nullptr);
            if (!warden->CheckSpawnRules(*entities, SpawnReason::Triggered) ||
                !warden->CheckSpawnObstruction(*entities)) {
                continue;   // mob.discard()
            }
            Warden* raw = warden.get();
            entities->AddFreshEntity(std::move(warden));
            raw->PlayAmbientSound();
            return true;
        }
        return false;
    }

    // ══ Sculk catalyst ══════════════════════════════════════════════════════

    SculkCatalystBlockEntity::SculkCatalystBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos,
                                                       BlockID blockId)
        : BlockEntity(type, worldPos, blockId), m_listener(worldPos) {}

    void SculkCatalystBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        if (!PrepareListenerTick(*world, GetWorldPos())) return;
        JavaRandom* random = world->Random();
        if (!random) return;
        const bool had = !m_listener.spreader.Cursors().empty();
        m_listener.spreader.UpdateCursors(*world, GetWorldPos(), *random, true);
        // The cursors are saved state: a catalyst mid-spread must be written.
        if (had) MarkDirty();
    }

    bool SculkCatalystBlockEntity::CatalystListener::HandleGameEvent(World& level, GameEventId event,
                                                                     const GameEventContext& context,
                                                                     const glm::dvec3& sourcePosition) {
        if (event != GameEventId::EntityDie || !context.sourceEntity) return false;
        LivingEntity* mob = context.sourceEntity->AsLiving();
        if (!mob) return false;
        if (!mob->WasExperienceConsumed()) {
            EntityLevel* entities = level.Entities();
            Entity* killer = entities ? entities->ResolveEntityById(mob->GetKillerId()) : nullptr;
            const int experienceWouldDrop = entities ? mob->GetExperienceReward(*entities, killer) : 0;
            if (mob->ShouldDropExperience() && experienceWouldDrop > 0) {
                // sourcePosition.relative(UP, 0.5): the charge starts in the
                // block the corpse lay in, half a block up.
                spreader.AddCursors(BlockContaining(sourcePosition + glm::dvec3(0.0, 0.5, 0.0)),
                                    experienceWouldDrop);
                // The killer, a server player, gets
                // CriteriaTriggers.KILL_MOB_NEAR_SCULK_CATALYST.
                if (Server::ServerPlayer* player = Server::CriteriaTriggers::PlayerOf(killer)) {
                    Server::CriteriaTriggers::KillMobNearSculkCatalyst(
                        *player, *mob, DamageSourceInfo::Of(mob->GetLastDamageSource(), killer, nullptr));
                }
            }
            mob->SkipDropExperience();
            if (const std::optional<glm::dvec3> p = m_source.GetPosition()) Bloom(level, BlockContaining(*p));
        }
        return true;
    }

    void SculkCatalystBlockEntity::CatalystListener::Bloom(World& level, const glm::ivec3& pos) {
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != BlockID::SculkCatalyst) return;
        level.SetBlock(pos.x, pos.y, pos.z, WithBool(state, PropertyId::BLOOM, true), 3);
        if (ScheduledTickAccess* ticks = level.Ticks()) ticks->ScheduleTick(pos, state.Block(), kPulseTicks);
        level.SendParticles(ParticleOptions(ParticleKind::SculkSoul), pos.x + 0.5, pos.y + 1.15, pos.z + 0.5, 2,
                            0.2, 0.0, 0.2, 0.0);
        JavaRandom* random = level.Random();
        const float pitch = 0.6f + (random ? random->NextFloat() : 0.5f) * 0.4f;
        level.PlaySound(SoundExcept(nullptr), pos, SoundEvents::SCULK_CATALYST_BLOOM, SoundSource::Blocks, 2.0f,
                        pitch);
    }

} // namespace Game
