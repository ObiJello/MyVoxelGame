// File: src/common/entity/ai/goals/ParrotGoals.cpp
#include "common/entity/ai/goals/ParrotGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/pathfinder/PathType.hpp"
#include "common/world/tags/DataTags.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace Game {

    namespace {

        int FloorInt(double v) { return static_cast<int>(std::floor(v)); }

        // MC `state.getBlock() instanceof LeavesBlock || state.is(BlockTags.LOGS)`
        // — every LeavesBlock is a *_leaves block (oak … pale oak, both
        // azaleas, mangrove, cherry); the log tag comes from the data pack.
        // Resolved once per block id.
        bool CanParrotSitOn(BlockID block) {
            static const std::array<bool, static_cast<size_t>(BlockID::Count)> table = [] {
                std::array<bool, static_cast<size_t>(BlockID::Count)> t{};
                for (size_t i = 0; i < t.size(); ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    const bool leaves = slug.size() >= 7 &&
                                        slug.compare(slug.size() - 7, 7, "_leaves") == 0;
                    t[i] = leaves || DataTags::HasTag(DataTags::Registry::Block, slug, "minecraft:logs");
                }
                return t;
            }();
            return table[static_cast<size_t>(block)];
        }

        // MC Entity.isInPowderSnow: set while the entity's (1e-5 deflated)
        // box overlaps a powder snow cell (PowderSnowBlock.entityInside, via
        // checkInsideBlocks). The engine keeps no such flag, so the cells are
        // asked directly.
        bool IsInPowderSnow(const Entity& entity) {
            const EntityLevel* level = entity.Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            if (!blocks) return false;
            const AABBd box = entity.GetAABBd();
            constexpr double kDeflate = 1.0e-5;
            const int x0 = FloorInt(box.min.x + kDeflate), x1 = FloorInt(box.max.x - kDeflate);
            const int y0 = FloorInt(box.min.y + kDeflate), y1 = FloorInt(box.max.y - kDeflate);
            const int z0 = FloorInt(box.min.z + kDeflate), z1 = FloorInt(box.max.z - kDeflate);
            for (int y = y0; y <= y1; ++y) {
                for (int z = z0; z <= z1; ++z) {
                    for (int x = x0; x <= x1; ++x) {
                        if (blocks->GetBlock(x, y, z) == BlockID::PowderSnow) return true;
                    }
                }
            }
            return false;
        }

        // MC `entity instanceof Mob` for the entities this engine keeps on
        // its Mob base: every non-MISC category, plus the MISC types that
        // are Mob subclasses in MC (the golems and the villager). Armor
        // stands, projectiles, crystals, frames and the rest are not.
        bool IsMcMob(const Entity& entity) {
            switch (entity.GetType()) {
                case EntityTypeId::Villager:
                case EntityTypeId::IronGolem:
                case EntityTypeId::SnowGolem:
                case EntityTypeId::CopperGolem:
                    return true;
                default:
                    return GetEntityTypeInfo(entity.GetType()).category != MobCategory::Misc;
            }
        }

        // MC Entity.isInvisible for a mob: the INVISIBILITY effect.
        bool IsInvisible(const Entity& entity) {
            const auto* living = dynamic_cast<const LivingEntity*>(&entity);
            return living && living->IsEffectInvisible();
        }

    } // namespace

    // ── ParrotWanderGoal ───────────────────────────────────────────────────

    bool ParrotWanderGoal::GetPosition(glm::dvec3& out) {
        // MC ParrotWanderGoal.getPosition, verbatim — including that the
        // tree roll can replace (or null) the in-water land target.
        std::optional<glm::dvec3> pos;
        if (m_mob->IsInWater()) {
            pos = RandomPos::GetLandPos(*m_mob, 15, 15);
        }
        if (m_mob->Level()->Random().NextFloat() >= WaterAvoidingRandomStrollGoal::kProbability) {
            glm::dvec3 tree;
            if (GetTreePos(tree)) pos = tree;
            else                  pos.reset();
        }
        if (!pos) return WaterAvoidingRandomFlyingGoal::GetPosition(out);
        out = *pos;
        return true;
    }

    bool ParrotWanderGoal::GetTreePos(glm::dvec3& out) const {
        // MC ParrotWanderGoal.getTreePos: the first cell of
        // BlockPos.betweenClosed(floor(x-3), floor(y-6), floor(z-3) ..
        // floor(x+3), floor(y+6), floor(z+3)) — x fastest, then y, then z —
        // other than the parrot's own, that stands on leaves or a log with
        // itself and the cell above empty; Vec3.atBottomCenterOf.
        const IBlockAccess* blocks = m_mob->Level() ? m_mob->Level()->Blocks() : nullptr;
        if (!blocks) return false;
        const glm::ivec3 mobPos = m_mob->BlockPosition();
        const glm::dvec3& p = m_mob->position;
        const int minX = FloorInt(p.x - 3.0), maxX = FloorInt(p.x + 3.0);
        const int minY = FloorInt(p.y - 6.0), maxY = FloorInt(p.y + 6.0);
        const int minZ = FloorInt(p.z - 3.0), maxZ = FloorInt(p.z + 3.0);
        for (int z = minZ; z <= maxZ; ++z) {
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    if (x == mobPos.x && y == mobPos.y && z == mobPos.z) continue;
                    if (!CanParrotSitOn(blocks->GetBlock(x, y - 1, z))) continue;
                    if (blocks->GetBlock(x, y, z) != BlockID::Air) continue;       // isEmptyBlock = isAir
                    if (blocks->GetBlock(x, y + 1, z) != BlockID::Air) continue;
                    out = glm::dvec3(x + 0.5, static_cast<double>(y), z + 0.5);
                    return true;
                }
            }
        }
        return false;
    }

    // ── LandOnOwnersShoulderGoal ───────────────────────────────────────────

    bool LandOnOwnersShoulderGoal::CanUse() {
        // MC: the owner must be a ServerPlayer — here a player on a server
        // level — who is not a spectator, not flying, not in water and not in
        // powder snow; the parrot must not be ordered to sit and its ride
        // cooldown must have run out.
        const EntityLevel* level = m_entity->Level();
        if (!level || level->IsClientSide()) return false;
        LivingEntity* owner = m_entity->GetOwner();
        if (!owner || !owner->IsPlayer()) return false;
        const bool ownerThatCanBeSatOn = !owner->IsSpectator() && !owner->IsAbilityFlying() &&
                                         !owner->IsInWater() && !IsInPowderSnow(*owner);
        return !m_entity->IsOrderedToSit() && ownerThatCanBeSatOn && m_entity->CanSitOnShoulder();
    }

    void LandOnOwnersShoulderGoal::Tick() {
        // MC: while not yet seated, not in the sitting pose and not on a
        // lead, touching the owner's box seats it (the player may still
        // refuse — airborne, in water, both shoulders taken).
        if (m_isSittingOnShoulder || m_entity->IsInSittingPose() || m_entity->IsLeashed()) return;
        LivingEntity* owner = m_entity->GetOwner();
        EntityLevel* level = m_entity->Level();
        if (!owner || !owner->IsPlayer() || !level) return;
        if (m_entity->GetAABBd().Intersects(owner->GetAABBd())) {
            m_isSittingOnShoulder = level->SetEntityOnShoulder(*owner, *m_entity);
        }
    }

    // ── FollowMobGoal ──────────────────────────────────────────────────────

    FollowMobGoal::FollowMobGoal(Mob* mob, double speedModifier, float stopDistance,
                                 float areaSize)
        : m_mob(mob), m_speedModifier(speedModifier), m_stopDistance(stopDistance),
          m_areaSize(areaSize) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool FollowMobGoal::CanUse() {
        // MC: getEntitiesOfClass(Mob.class, box.inflate(areaSize),
        // followPredicate) — any Mob of another class (here: another type)
        // — and the first one that is not invisible is followed.
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(m_areaSize);
        box.max += glm::vec3(m_areaSize);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, nullptr, nearby);
        for (Entity* e : nearby) {
            if (!e || e->IsRemoved() || e->GetType() == m_mob->GetType()) continue;
            auto* mob = dynamic_cast<Mob*>(e);
            if (!mob || !IsMcMob(*mob)) continue;
            if (IsInvisible(*mob)) continue;
            m_followingMob = mob;
            return true;
        }
        return false;
    }

    bool FollowMobGoal::CanContinueToUse() {
        return m_followingMob && !m_mob->GetNavigation().IsDone() &&
               m_mob->DistanceToSqr(*m_followingMob) >
                   static_cast<double>(m_stopDistance * m_stopDistance);
    }

    void FollowMobGoal::Start() {
        m_timeToRecalcPath = 0;
        m_oldWaterCost = m_mob->GetPathfindingMalus(PathType::Water);
        m_mob->SetPathfindingMalus(PathType::Water, 0.0f);
    }

    void FollowMobGoal::Stop() {
        m_followingMob = nullptr;
        m_mob->GetNavigation().Stop();
        m_mob->SetPathfindingMalus(PathType::Water, m_oldWaterCost);
    }

    void FollowMobGoal::Tick() {
        // MC FollowMobGoal.tick, verbatim.
        if (!m_followingMob || m_mob->IsLeashed()) return;
        const glm::dvec3 eye = m_followingMob->GetEyePosition();
        m_mob->GetLookControl().SetLookAt(eye.x, eye.y, eye.z, 10.0f,
                                          static_cast<float>(m_mob->GetMaxHeadXRot()));
        if (--m_timeToRecalcPath > 0) return;
        m_timeToRecalcPath = AdjustedTickDelay(10);

        const double xxd = m_mob->position.x - m_followingMob->position.x;
        const double yyd = m_mob->position.y - m_followingMob->position.y;
        const double zzd = m_mob->position.z - m_followingMob->position.z;
        const double distSqr = xxd * xxd + yyd * yyd + zzd * zzd;
        const double stopSqr = static_cast<double>(m_stopDistance * m_stopDistance);
        if (!(distSqr <= stopSqr)) {
            m_mob->GetNavigation().MoveTo(*m_followingMob, m_speedModifier);
            return;
        }
        m_mob->GetNavigation().Stop();
        // Crowding it (MC compares the SQUARED distance against the plain
        // stop distance), or it is looking right at this mob: back off to
        // the mirror-image point.
        const LookControl& look = m_followingMob->GetLookControl();
        if (distSqr <= static_cast<double>(m_stopDistance) ||
            (look.GetWantedX() == m_mob->position.x && look.GetWantedY() == m_mob->position.y &&
             look.GetWantedZ() == m_mob->position.z)) {
            const double deltaX = m_followingMob->position.x - m_mob->position.x;
            const double deltaZ = m_followingMob->position.z - m_mob->position.z;
            m_mob->GetNavigation().MoveTo(m_mob->position.x - deltaX, m_mob->position.y,
                                          m_mob->position.z - deltaZ, m_speedModifier);
        }
    }

    void FollowMobGoal::ClearReferenceTo(const Entity* entity) {
        if (m_followingMob == entity) m_followingMob = nullptr;
    }

} // namespace Game
