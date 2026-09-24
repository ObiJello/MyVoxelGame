// File: src/common/world/block/entity/PotentSulfurBlockEntity.cpp
//
// MC PotentSulfurBlockEntity's tickers (SERVER_NAUSEA_EFFECT_TICKER,
// SERVER_WAITING_COUNTDOWN_TICKER, LAUNCH_ENTITY_TICKER, CLIENT_NOXIOUS_GAS_
// TICKER, CLIENT_GEYSER_PLUME_TICKER), dispatched per state as PotentSulfur-
// Block.getTicker does. See the header for the split and the deviations.
#include "PotentSulfurBlockEntity.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/physics/Physics.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/block/PotentSulfurBlock.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include "random/XoroshiroRandomSource.h"   // terrain library: MC XoroshiroRandomSource

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        using PotentSulfur::State;

        // MC EFFECT_PREDICATE: EntitySelector.NO_SPECTATORS and
        // ENTITY_STILL_ALIVE.
        bool EffectPredicate(const Entity& e) {
            return !e.IsSpectator() && e.IsAlive();
        }

        // MC `entity.is(EntityTypeTags.NOT_AFFECTED_BY_GEYSERS)` (the ender
        // dragon), from the data pack.
        bool NotAffectedByGeysers(const Entity& e) {
            const std::vector<std::string>& tags = DataTags::TagsFor(
                DataTags::Registry::EntityType, GetEntityTypeInfo(e.GetType()).slug);
            return std::binary_search(tags.begin(), tags.end(),
                                      std::string("#minecraft:not_affected_by_geysers"));
        }

        // MC `waterBlocks = sourceBlock.getY() - pos.getY() - 1`: the water
        // column between the sulfur and the open cell above it.
        int WaterBlocks(const glm::ivec3& pos, const glm::ivec3& sourceBlock) {
            return sourceBlock.y - pos.y - 1;
        }

        // MC `entityVelocity.y < GEYSER_BASE_LAUNCH_SPEED + waterBlocks * 0.1`
        // — only what is not already rising fast enough gets the push.
        bool BelowLaunchSpeed(double vy, int waterBlocks) {
            return vy < PotentSulfurBlockEntity::kGeyserBaseLaunchSpeed +
                        static_cast<double>(waterBlocks) * 0.1;
        }

        // The launch column: `new AABB(pos.above()).expandTowards(0,
        // geyserForceHeight - 1, 0)` — the cell above the sulfur, stretched
        // up by the unobstructed height (and, as expandTowards does for a
        // negative delta, DOWN by one when nothing above is passable).
        AABBd LaunchBox(const IBlockAccess& level, const glm::ivec3& pos, int waterBlocks) {
            const glm::ivec3 above(pos.x, pos.y + 1, pos.z);
            const int height = PotentSulfur::GetUnobstructedBlockCount(level, above, waterBlocks);
            AABBd box = AABBd::FromMinMax(glm::dvec3(above), glm::dvec3(above) + glm::dvec3(1.0));
            const double dy = static_cast<double>(height - 1);
            if (dy < 0.0) box.min.y += dy; else box.max.y += dy;
            return box;
        }

        // MC geyserPositional: `new XoroshiroRandomSource(level.getSeed() ^
        // GEYSER_SALT).forkPositional().at(pos)` — the same stream every time
        // for a given geyser, which is what makes its rhythm its own.
        minecraft::XoroshiroRandomSource GeyserPositional(const World& world, const glm::ivec3& pos) {
            minecraft::XoroshiroRandomSource base(world.GetGenerationSeed() ^
                                                  PotentSulfurBlockEntity::kGeyserSalt);
            return base.forkPositional().at(pos.x, pos.y, pos.z);
        }

        // ── Server tickers ────────────────────────────────────────────────

        // MC SERVER_NAUSEA_EFFECT_TICKER: every 10 ticks, NAUSEA (80 ticks,
        // ambient, visible) for every living thing within 2.5 blocks of the
        // gas source's cell whose eyes the gas can reach.
        void ServerNauseaEffectTick(World& world, const glm::ivec3& pos) {
            if (world.GetGameTime() % PotentSulfurBlockEntity::kEffectApplicationFrequencyTicks != 0) return;
            const std::optional<glm::ivec3> source = PotentSulfur::FindNoxiousGasSourceBlock(world, pos);
            if (!source) return;
            EntityLevel* entities = world.Entities();
            if (!entities) return;
            // getNearbyLivingEntities: new AABB(sourceBlock).inflate(2.5, 0.0, 2.5).
            const glm::dvec3 lo(source->x - 2.5, source->y, source->z - 2.5);
            const glm::dvec3 hi(source->x + 3.5, source->y + 1.0, source->z + 3.5);
            std::vector<Entity*> found;
            entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(lo), glm::vec3(hi)), nullptr, found);
            for (Entity* e : found) {
                LivingEntity* living = e ? e->AsLiving() : nullptr;
                if (!living || !EffectPredicate(*living)) continue;
                if (!PotentSulfur::CanBeReachedByNoxiousGas(world, *source, living->GetEyePosition())) continue;
                living->AddEffect(MobEffectInstance(MobEffectId::Nausea,
                                                    PotentSulfurBlockEntity::kEffectDurationTicks, 0,
                                                    /*ambient=*/true, /*visible=*/true));
            }
        }

        // MC SERVER_WAITING_COUNTDOWN_TICKER: once a second, roll a countdown
        // when none is running — DORMANT waits 10·(water−1) + 15..30 seconds,
        // ERUPTING lasts (water−1) + 1..2 — count it down, and at zero flip
        // DORMANT <-> ERUPTING with a full update (the flip into ERUPTING
        // reaches PotentSulfurBlock's onPlace: the block event and the sound).
        void ServerWaitingCountdownTick(World& world, const glm::ivec3& pos, BlockState state,
                                        PotentSulfurBlockEntity& entity) {
            if (world.GetGameTime() % PotentSulfurBlockEntity::kParticleFrequencyTicks != 0) return;
            const std::optional<glm::ivec3> source = PotentSulfur::FindNoxiousGasSourceBlock(world, pos);
            if (!source) return;
            const bool dormant = PotentSulfur::StateOf(state) == State::Dormant;
            if (entity.waitingCountdown <= 0) {
                const int waterBlocks = WaterBlocks(pos, *source);
                minecraft::XoroshiroRandomSource random = GeyserPositional(world, pos);
                if (dormant) {
                    entity.waitingCountdown = 10 * (waterBlocks - 1) + random.nextIntBetweenInclusive(15, 30);
                } else {
                    (void)random.nextInt();
                    entity.waitingCountdown = waterBlocks - 1 + random.nextIntBetweenInclusive(1, 2);
                }
            }
            if (entity.waitingCountdown > 0) --entity.waitingCountdown;
            if (entity.waitingCountdown == 0) {
                const State next = dormant ? State::Erupting : State::Dormant;
                world.SetBlock(pos.x, pos.y, pos.z, PotentSulfur::WithState(state, next),
                               World::UpdateFlags::All);
                // DORMANT: level.gameEvent(BLOCK_DEACTIVATE) — no game-event
                // system here (see PotentSulfurBlock.cpp).
            }
        }

        // MC LAUNCH_ENTITY_TICKER, the server's half: its mobs and dropped
        // items. Players are left to their own client's half (the header).
        void ServerLaunchEntityTick(World& world, const glm::ivec3& pos) {
            const std::optional<glm::ivec3> source = PotentSulfur::FindNoxiousGasSourceBlock(world, pos);
            if (!source) return;
            EntityLevel* entities = world.Entities();
            if (!entities) return;
            const int waterBlocks = WaterBlocks(pos, *source);
            const AABBd box = LaunchBox(world, pos, waterBlocks);
            const glm::dvec3 launch(0.0, PotentSulfurBlockEntity::kGeyserLaunchForce, 0.0);

            std::vector<Entity*> found;
            entities->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(box.min), glm::vec3(box.max)),
                                       nullptr, found);
            for (Entity* e : found) {
                if (!e || !EffectPredicate(*e) || e->IsPlayer()) continue;
                const glm::dvec3 velocity = e->velocity;
                // checkFallDistanceAccumulation.
                if (velocity.y > -0.5 && e->fallDistance > 1.0f) e->fallDistance = 1.0f;
                if (e->IsPassenger() || NotAffectedByGeysers(*e)) continue;
                if (BelowLaunchSpeed(velocity.y, waterBlocks)) e->AddDeltaMovement(launch);
            }

            // Item entities are not Entities here; the same test on the
            // level's dropped items (an item has no fall distance, rides
            // nothing and is never in the geyser tag).
            std::vector<EntityLevel::NearbyItemEntity> items;
            entities->GetItemEntitiesInBox(box, items);
            for (const EntityLevel::NearbyItemEntity& item : items) {
                if (BelowLaunchSpeed(item.velocity.y, waterBlocks)) {
                    entities->AddItemEntityDeltaMovement(item.id, launch);
                }
            }
        }

        // ── Client tickers ────────────────────────────────────────────────

        // MC CLIENT_NOXIOUS_GAS_TICKER: once a second, a NOXIOUS_GAS_CLOUD
        // (the particle that seeds the drifting gas) at the source cell's
        // centre.
        void ClientNoxiousGasTick(ILevelWrite& level, const glm::ivec3& pos) {
            if (level.GameTime() % PotentSulfurBlockEntity::kParticleFrequencyTicks != 0) return;
            const std::optional<glm::ivec3> source = PotentSulfur::FindNoxiousGasSourceBlock(level, pos);
            if (!source) return;
            level.AddParticle(ParticleKind::NoxiousGasCloud,
                              source->x + 0.5, source->y + 0.5, source->z + 0.5, 0.0, 0.0, 0.0);
        }

        // MC CLIENT_GEYSER_PLUME_TICKER(sound): phased from the eruption's
        // start — a GEYSER particle (the plume's emitter; its water column
        // rides the vx slot, see ParticleKind::Geyser) at the source every
        // second, the running sound every two.
        void ClientGeyserPlumeTick(ILevelWrite& level, const glm::ivec3& pos,
                                   const PotentSulfurBlockEntity& entity, std::string_view sound) {
            const std::optional<glm::ivec3> source = PotentSulfur::FindNoxiousGasSourceBlock(level, pos);
            if (!source) return;
            const int64_t eruptionTime = level.GameTime() - entity.eruptionTick;
            if (eruptionTime % PotentSulfurBlockEntity::kParticleFrequencyTicks == 0) {
                level.AddParticle(ParticleKind::Geyser, source->x + 0.5, static_cast<double>(source->y),
                                  source->z + 0.5, static_cast<double>(WaterBlocks(pos, *source)),
                                  0.0, 0.0);
            }
            if (eruptionTime % PotentSulfurBlockEntity::kSoundFrequencyTicks == 0) {
                level.PlayLocalSound(glm::dvec3(source->x + 0.5, source->y + 0.5, source->z + 0.5),
                                     sound, SoundSource::Blocks, 1.0f, 1.0f, false);
            }
        }

        // MC LAUNCH_ENTITY_TICKER, the client's half: the one entity the
        // client simulates the movement of, its own player.
        void ClientLaunchEntityTick(ILevelWrite& level, const glm::ivec3& pos) {
            const std::optional<glm::ivec3> source = PotentSulfur::FindNoxiousGasSourceBlock(level, pos);
            if (!source) return;
            const int waterBlocks = WaterBlocks(pos, *source);
            const AABBd box = LaunchBox(level, pos, waterBlocks);
            glm::dvec3 lo, hi;
            if (!level.GetLocalPlayerBox(lo, hi)) return;
            // getEntitiesOfClass: AABB.intersects.
            if (!(lo.x < box.max.x && hi.x > box.min.x && lo.y < box.max.y && hi.y > box.min.y &&
                  lo.z < box.max.z && hi.z > box.min.z)) {
                return;
            }
            glm::dvec3 velocity;
            bool flying = false;
            if (!level.GetLocalPlayerMovement(velocity, flying)) return;
            level.CheckLocalPlayerFallDistanceAccumulation();
            // `player.getAbilities().flying` skips; a client player never
            // rides here and is never in the geyser tag.
            if (flying) return;
            if (BelowLaunchSpeed(velocity.y, waterBlocks)) {
                level.AddLocalPlayerDeltaMovement(
                    glm::dvec3(0.0, PotentSulfurBlockEntity::kGeyserLaunchForce, 0.0));
            }
        }

    } // namespace

    void PotentSulfurBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        // MC setLevel: the clock starts when the entity meets its level.
        if (eruptionTick == -1) eruptionTick = world->GetGameTime();
        const glm::ivec3 pos = GetWorldPos();
        // The state is read once, as MC's bound ticker reads it once and
        // hands the same state down an `andThen` chain.
        const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
        if (!state.Is(BlockID::PotentSulfur)) return;
        switch (PotentSulfur::StateOf(state)) {
            case State::Dry:
                break;
            case State::Wet:
                ServerNauseaEffectTick(*world, pos);
                break;
            case State::Dormant:
                ServerWaitingCountdownTick(*world, pos, state, *this);
                ServerNauseaEffectTick(*world, pos);
                break;
            case State::Erupting:
                ServerLaunchEntityTick(*world, pos);
                ServerWaitingCountdownTick(*world, pos, state, *this);
                break;
            case State::Continuous:
                ServerLaunchEntityTick(*world, pos);
                break;
        }
    }

    void PotentSulfurBlockEntity::ClientTick(ILevelWrite& level) {
        if (eruptionTick == -1) eruptionTick = level.GameTime();
        const glm::ivec3 pos = GetWorldPos();
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        if (!state.Is(BlockID::PotentSulfur)) return;
        switch (PotentSulfur::StateOf(state)) {
            case State::Dry:
                break;
            case State::Wet:
            case State::Dormant:
                ClientNoxiousGasTick(level, pos);
                break;
            case State::Erupting:
                ClientGeyserPlumeTick(level, pos, *this, SoundEvents::GEYSER_ERUPTION_ACTIVE);
                ClientLaunchEntityTick(level, pos);
                break;
            case State::Continuous:
                ClientGeyserPlumeTick(level, pos, *this, SoundEvents::GEYSER_CONTINUOUS_ACTIVE);
                ClientLaunchEntityTick(level, pos);
                break;
        }
    }

    void PotentSulfurBlockEntity::CarryClientState(const BlockEntity& previous) {
        if (const auto* old = dynamic_cast<const PotentSulfurBlockEntity*>(&previous)) {
            eruptionTick = old->eruptionTick;
        }
    }

} // namespace Game
