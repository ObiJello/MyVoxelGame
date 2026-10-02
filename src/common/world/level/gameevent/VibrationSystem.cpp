// File: src/common/world/level/gameevent/VibrationSystem.cpp
#include "VibrationSystem.hpp"
#include "server/advancements/CriteriaTriggers.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Game {

    namespace {

        glm::ivec3 BlockContaining(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }

        enum BlockTagBit : uint8_t {
            kOccludes  = 1 << 0,   // #minecraft:occludes_vibration_signals
            kDampens   = 1 << 1,   // #minecraft:dampens_vibrations
            kResonator = 1 << 2,   // #minecraft:vibration_resonators
        };

        const std::vector<uint8_t>& BlockTagTable() {
            static const std::vector<uint8_t> table = [] {
                std::vector<uint8_t> t(BlockRegistry::Size, 0);
                for (size_t i = 0; i < t.size(); ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    for (const std::string& tag : DataTags::TagsFor(DataTags::Registry::Block, slug)) {
                        if (tag == "#minecraft:occludes_vibration_signals") t[i] |= kOccludes;
                        else if (tag == "#minecraft:dampens_vibrations")    t[i] |= kDampens;
                        else if (tag == "#minecraft:vibration_resonators")  t[i] |= kResonator;
                    }
                }
                return t;
            }();
            return table;
        }

        bool HasBlockTag(BlockID block, uint8_t bit) {
            const auto& t = BlockTagTable();
            const size_t i = static_cast<size_t>(block);
            return i < t.size() && (t[i] & bit) != 0;
        }

        // MC Mth.lerp(double).
        double Lerp(double alpha, double a, double b) { return a + alpha * (b - a); }

        // MC ChunkPos-around "areAdjacentChunksTicking".
        bool AreAdjacentChunksTicking(const World& level, const glm::ivec3& listenerPos) {
            const int cx = listenerPos.x >> 4;
            const int cz = listenerPos.z >> 4;
            for (int x = cx - 1; x <= cx + 1; ++x) {
                for (int z = cz - 1; z <= cz + 1; ++z) {
                    if (!level.ShouldTickBlocksAt(glm::ivec3(x << 4, 0, z << 4)) ||
                        !level.IsChunkLoaded(x, z)) {
                        return false;
                    }
                }
            }
            return true;
        }

    } // namespace

    // ── VibrationInfo ───────────────────────────────────────────────────────

    VibrationInfo VibrationInfo::Of(GameEventId event, float distance, const glm::dvec3& pos,
                                    Entity* entity) {
        VibrationInfo info;
        info.gameEvent = event;
        info.distance = distance;
        info.pos = pos;
        if (entity) {
            info.uuid = entity->GetUuid();
            // MC getProjectileOwner(entity): a projectile's owner, if it has one.
            if (auto* projectile = dynamic_cast<Projectile*>(entity)) {
                if (Entity* owner = projectile->GetOwner()) info.projectileOwnerUuid = owner->GetUuid();
            }
        }
        return info;
    }

    Entity* VibrationInfo::ResolveEntity(EntityLevel* level) const {
        if (!uuid || !level || UuidIsNil(*uuid)) return nullptr;
        Entity* e = level->ResolveEntity(*uuid);
        if (!e) e = level->ResolvePlayer(*uuid);
        return e;
    }

    Entity* VibrationInfo::ResolveProjectileOwner(EntityLevel* level) const {
        // MC: the entity, if it is a projectile, names its owner; otherwise
        // the owner UUID recorded when the vibration was heard.
        if (auto* projectile = dynamic_cast<Projectile*>(ResolveEntity(level))) {
            if (Entity* owner = projectile->GetOwner()) return owner;
        }
        if (!projectileOwnerUuid || !level || UuidIsNil(*projectileOwnerUuid)) return nullptr;
        Entity* e = level->ResolveEntity(*projectileOwnerUuid);
        if (!e) e = level->ResolvePlayer(*projectileOwnerUuid);
        return e;
    }

    // ── VibrationSelector ───────────────────────────────────────────────────

    void VibrationSelector::AddCandidate(const VibrationInfo& vibration, int64_t tickTime) {
        if (ShouldReplaceVibration(vibration, tickTime)) m_current = Candidate{vibration, tickTime};
    }

    bool VibrationSelector::ShouldReplaceVibration(const VibrationInfo& vibration, int64_t tickTime) const {
        if (!m_current) return true;
        if (tickTime != m_current->tick) return false;
        const VibrationInfo& previous = m_current->event;
        if (vibration.distance < previous.distance) return true;
        if (vibration.distance > previous.distance) return false;
        return GameEvents::Frequency(vibration.gameEvent) > GameEvents::Frequency(previous.gameEvent);
    }

    std::optional<VibrationInfo> VibrationSelector::ChosenCandidate(int64_t time) const {
        if (!m_current) return std::nullopt;
        return m_current->tick < time ? std::optional<VibrationInfo>(m_current->event) : std::nullopt;
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    namespace Vibrations {

        int RedstoneStrengthForDistance(float distance, int listenerRadius) {
            const double powerScale = 15.0 / static_cast<double>(listenerRadius);
            return std::max(1, 15 - static_cast<int>(std::floor(powerScale * static_cast<double>(distance))));
        }

        float DistanceBetweenInBlocks(const glm::ivec3& origin, const glm::ivec3& dest) {
            const double dx = static_cast<double>(origin.x - dest.x);
            const double dy = static_cast<double>(origin.y - dest.y);
            const double dz = static_cast<double>(origin.z - dest.z);
            return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
        }

        bool OccludesVibrationSignals(BlockID block) { return HasBlockTag(block, kOccludes); }
        bool DampensVibrations(BlockID block)        { return HasBlockTag(block, kDampens); }
        bool IsVibrationResonator(BlockID block)     { return HasBlockTag(block, kResonator); }

    } // namespace Vibrations

    // ── VibrationUser defaults ──────────────────────────────────────────────

    int VibrationUser::CalculateTravelTimeInTicks(float distanceToDestination) const {
        return static_cast<int>(std::floor(distanceToDestination));
    }

    bool VibrationUser::IsValidVibration(GameEventId event, const GameEventContext& context) const {
        if (!GameEvents::Is(event, GetListenableEvents())) return false;
        if (const Entity* source = context.sourceEntity) {
            if (source->IsSpectator()) return false;
            // MC Entity.isSteppingCarefully (isShiftKeyDown): a sneaking
            // entity makes no #ignore_vibrations_sneaking event heard — and a
            // sneaking server player that a sculk sensor would have heard
            // gets CriteriaTriggers.AVOID_VIBRATION.
            const LivingEntity* living = source->AsLiving();
            if (living && living->IsDiscrete() &&
                GameEvents::Is(event, GameEvents::Tag::IgnoreVibrationsSneaking)) {
                if (CanTriggerAvoidVibration()) {
                    if (Server::ServerPlayer* player = Server::CriteriaTriggers::PlayerOf(source)) {
                        Server::CriteriaTriggers::AvoidVibration(*player);
                    }
                }
                return false;
            }
            if (source->DampensVibrations()) return false;
        }
        if (context.affectedState) {
            return !Vibrations::DampensVibrations(context.affectedState->Block());
        }
        return true;
    }

    // ── VibrationListener ───────────────────────────────────────────────────

    bool VibrationListener::HandleGameEvent(World& level, GameEventId event, const GameEventContext& context,
                                            const glm::dvec3& sourcePosition) {
        VibrationData& data = m_system.GetVibrationData();
        VibrationUser& user = m_system.GetVibrationUser();
        if (data.CurrentVibration()) return false;
        if (!user.IsValidVibration(event, context)) return false;
        const std::optional<glm::dvec3> listenerPos = user.GetPositionSource().GetPosition();
        if (!listenerPos) return false;
        const glm::dvec3 destination = *listenerPos;
        if (!user.CanReceiveVibration(level, BlockContaining(sourcePosition), event, context)) return false;
        if (IsOccluded(level, sourcePosition, destination)) return false;
        ScheduleVibration(level, data, event, context, sourcePosition, destination);
        return true;
    }

    void VibrationListener::ForceScheduleVibration(World& level, GameEventId event,
                                                   const GameEventContext& context, const glm::dvec3& origin) {
        if (const std::optional<glm::dvec3> p = m_system.GetVibrationUser().GetPositionSource().GetPosition()) {
            ScheduleVibration(level, m_system.GetVibrationData(), event, context, origin, *p);
        }
    }

    void VibrationListener::ScheduleVibration(World& level, VibrationData& data, GameEventId event,
                                              const GameEventContext& context, const glm::dvec3& origin,
                                              const glm::dvec3& dest) {
        const glm::dvec3 d = origin - dest;
        const float distance = static_cast<float>(std::sqrt(glm::dot(d, d)));
        data.Selection().AddCandidate(VibrationInfo::Of(event, distance, origin, context.sourceEntity),
                                      level.GameTime());
    }

    bool VibrationListener::IsOccluded(const World& level, const glm::dvec3& origin, const glm::dvec3& dest) {
        const glm::dvec3 from(std::floor(origin.x) + 0.5, std::floor(origin.y) + 0.5, std::floor(origin.z) + 0.5);
        const glm::dvec3 to(std::floor(dest.x) + 0.5, std::floor(dest.y) + 0.5, std::floor(dest.z) + 0.5);
        // MC's nudge: Vec3.relative(direction, 9.999999747378752E-6).
        constexpr double kNudge = 9.999999747378752E-6;
        static constexpr glm::ivec3 kSteps[6] = {
            {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
        };
        const auto occludes = [&level](const glm::ivec3& cell, const glm::dvec3&, const glm::dvec3&) {
            return Vibrations::OccludesVibrationSignals(level.GetBlock(cell.x, cell.y, cell.z));
        };
        for (const glm::ivec3& step : kSteps) {
            const glm::dvec3 nudged = from + glm::dvec3(step) * kNudge;
            // isBlockInLine(...).getType() != BLOCK → a clear ray: heard.
            if (!TraverseBlocks(nudged, to, occludes)) return false;
        }
        return true;
    }

    // ── VibrationTicker ─────────────────────────────────────────────────────

    namespace VibrationTicker {

        namespace {

            void TrySelectAndScheduleVibration(World& level, VibrationData& data, VibrationUser& user) {
                const std::optional<VibrationInfo> chosen = data.Selection().ChosenCandidate(level.GameTime());
                if (!chosen) return;
                data.SetCurrentVibration(*chosen);
                const glm::dvec3 origin = chosen->pos;
                data.SetTravelTimeInTicks(user.CalculateTravelTimeInTicks(chosen->distance));
                level.SendParticles(user.GetPositionSource().VibrationParticle(data.GetTravelTimeInTicks()),
                                    origin.x, origin.y, origin.z, 1, 0.0, 0.0, 0.0, 0.0);
                user.OnDataChanged();
                data.Selection().StartOver();
            }

            void TryReloadVibrationParticle(World& level, VibrationData& data, VibrationUser& user) {
                if (!data.ShouldReloadVibrationParticle()) return;
                if (!data.CurrentVibration()) {
                    data.SetReloadVibrationParticle(false);
                    return;
                }
                const glm::dvec3 origin = data.CurrentVibration()->pos;
                const PositionSource& source = user.GetPositionSource();
                const glm::dvec3 destination = source.GetPosition().value_or(origin);
                const int travelTimeInTicks = data.GetTravelTimeInTicks();
                const int initialTravelTime = user.CalculateTravelTimeInTicks(data.CurrentVibration()->distance);
                const double alpha = initialTravelTime != 0
                    ? 1.0 - static_cast<double>(travelTimeInTicks) / static_cast<double>(initialTravelTime)
                    : 1.0;
                const double x = Lerp(alpha, origin.x, destination.x);
                const double y = Lerp(alpha, origin.y, destination.y);
                const double z = Lerp(alpha, origin.z, destination.z);
                const bool sent = level.SendParticles(source.VibrationParticle(travelTimeInTicks),
                                                      x, y, z, 1, 0.0, 0.0, 0.0, 0.0) > 0;
                if (sent) data.SetReloadVibrationParticle(false);
            }

            bool ReceiveVibration(World& level, VibrationData& data, VibrationUser& user,
                                  const VibrationInfo& current) {
                const glm::ivec3 origin = BlockContaining(current.pos);
                const std::optional<glm::dvec3> destPos = user.GetPositionSource().GetPosition();
                const glm::ivec3 destination = destPos ? BlockContaining(*destPos) : origin;
                if (user.RequiresAdjacentChunksToBeTicking() && !AreAdjacentChunksTicking(level, destination)) {
                    return false;
                }
                EntityLevel* entities = level.Entities();
                user.OnReceiveVibration(level, origin, current.gameEvent, current.ResolveEntity(entities),
                                        current.ResolveProjectileOwner(entities),
                                        Vibrations::DistanceBetweenInBlocks(origin, destination));
                data.SetCurrentVibration(std::nullopt);
                return true;
            }

        } // namespace

        void Tick(World& level, VibrationData& data, VibrationUser& user) {
            if (!data.CurrentVibration()) TrySelectAndScheduleVibration(level, data, user);
            if (!data.CurrentVibration()) return;
            bool hasChanged = data.GetTravelTimeInTicks() > 0;
            TryReloadVibrationParticle(level, data, user);
            data.DecrementTravelTime();
            if (data.GetTravelTimeInTicks() <= 0) {
                // Copy: onReceiveVibration may reach back into the data.
                const VibrationInfo current = *data.CurrentVibration();
                hasChanged = ReceiveVibration(level, data, user, current);
            }
            if (hasChanged) user.OnDataChanged();
        }

    } // namespace VibrationTicker

} // namespace Game
