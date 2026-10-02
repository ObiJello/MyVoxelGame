// File: src/common/entity/ai/goals/GolemGoals.cpp
//
// See GolemGoals.hpp. Every function names the MC method it ports.
#include "common/entity/ai/goals/GolemGoals.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/npc/Villager.hpp"
#include "common/world/tags/DataTags.hpp"

#include <limits>
#include <vector>

namespace Game {

    namespace {

        // MC SectionPos.of(BlockPos).
        glm::ivec3 SectionOf(const glm::ivec3& b) { return glm::ivec3(b.x >> 4, b.y >> 4, b.z >> 4); }

        // MC SectionPos.center(): the section's middle block.
        glm::ivec3 SectionCenter(const glm::ivec3& s) { return s * 16 + glm::ivec3(8); }

        // MC Vec3.atBottomCenterOf.
        glm::dvec3 AtBottomCenterOf(const glm::ivec3& p) {
            return glm::dvec3(static_cast<double>(p.x) + 0.5, static_cast<double>(p.y),
                              static_cast<double>(p.z) + 0.5);
        }

        // MC BehaviorUtils.findSectionClosestToVillage: of the sections in
        // the cube nearer a village than `center`, the nearest (the first in
        // SectionPos.cube's walk on a tie), else `center` itself.
        glm::ivec3 FindSectionClosestToVillage(const PoiManager& poi, const glm::ivec3& center, int radius) {
            const int distToVillage = poi.SectionsToVillage(center.x, center.y, center.z);
            glm::ivec3 best = center;
            int bestDist = std::numeric_limits<int>::max();
            bool found = false;
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const glm::ivec3 s = center + glm::ivec3(dx, dy, dz);
                        const int d = poi.SectionsToVillage(s.x, s.y, s.z);
                        if (d >= distToVillage) continue;
                        if (!found || d < bestDist) { best = s; bestDist = d; found = true; }
                    }
                }
            }
            return best;
        }

        bool HasEntityTypeTag(const Entity& entity, std::string_view tag) {
            return DataTags::HasTag(DataTags::Registry::EntityType, entity.TypeInfo().slug, tag);
        }

        AABB Inflate(const AABB& box, float x, float y, float z) {
            AABB out = box;
            out.min -= glm::vec3(x, y, z);
            out.max += glm::vec3(x, y, z);
            return out;
        }

    } // namespace

    // ── MoveTowardsTargetGoal ───────────────────────────────────────────────

    MoveTowardsTargetGoal::MoveTowardsTargetGoal(PathfinderMob* mob, double speedModifier, float within)
        : m_mob(mob), m_speedModifier(speedModifier), m_within(within) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool MoveTowardsTargetGoal::CanUse() {
        m_target = m_mob->GetTarget();
        if (m_target == nullptr) return false;
        const double within = static_cast<double>(m_within * m_within);
        if (m_target->DistanceToSqr(*m_mob) > within) return false;
        // DefaultRandomPos.getPosTowards(mob, 16, 7, target.position(), PI/2).
        const std::optional<glm::dvec3> pos =
            RandomPos::GetPosTowards(*m_mob, 16, 7, m_target->position, 1.5707963705062866);
        if (!pos) return false;
        m_wantedX = pos->x;
        m_wantedY = pos->y;
        m_wantedZ = pos->z;
        return true;
    }

    bool MoveTowardsTargetGoal::CanContinueToUse() {
        return m_target != nullptr && !m_mob->GetNavigation().IsDone() && m_target->IsAlive() &&
               m_target->DistanceToSqr(*m_mob) < static_cast<double>(m_within * m_within);
    }

    void MoveTowardsTargetGoal::Stop() {
        m_target = nullptr;
    }

    void MoveTowardsTargetGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_wantedX, m_wantedY, m_wantedZ, m_speedModifier);
    }

    void MoveTowardsTargetGoal::ClearReferenceTo(const Entity* entity) {
        if (entity && static_cast<const Entity*>(m_target) == entity) m_target = nullptr;
    }

    // ── MoveBackToVillageGoal ───────────────────────────────────────────────

    MoveBackToVillageGoal::MoveBackToVillageGoal(PathfinderMob* mob, double speedModifier,
                                                 bool checkNoActionTime)
        : RandomStrollGoal(mob, speedModifier, /*interval=*/10, checkNoActionTime) {}

    bool MoveBackToVillageGoal::CanUse() {
        EntityLevel* level = m_mob->Level();
        PoiManager* poi = level ? level->GetPoiManager() : nullptr;
        // level.isVillage(pos) ? false : super.canUse().
        if (poi && poi->IsVillage(m_mob->BlockPosition())) return false;
        return RandomStrollGoal::CanUse();
    }

    bool MoveBackToVillageGoal::GetPosition(glm::dvec3& out) {
        EntityLevel* level = m_mob->Level();
        PoiManager* poi = level ? level->GetPoiManager() : nullptr;
        if (!poi) return false;
        const glm::ivec3 sectionPos = SectionOf(m_mob->BlockPosition());
        const glm::ivec3 optimalSectionPos = FindSectionClosestToVillage(*poi, sectionPos, 2);
        if (optimalSectionPos == sectionPos) return false;
        // DefaultRandomPos.getPosTowards(mob, 10, 7,
        // atBottomCenterOf(optimalSectionPos.center()), PI/2).
        const std::optional<glm::dvec3> pos = RandomPos::GetPosTowards(
            *m_mob, 10, 7, AtBottomCenterOf(SectionCenter(optimalSectionPos)), 1.5707963705062866);
        if (!pos) return false;
        out = *pos;
        return true;
    }

    // ── GolemRandomStrollInVillageGoal ──────────────────────────────────────

    GolemRandomStrollInVillageGoal::GolemRandomStrollInVillageGoal(PathfinderMob* mob, double speedModifier)
        : RandomStrollGoal(mob, speedModifier, /*interval=*/240, /*checkNoActionTime=*/false) {}

    bool GolemRandomStrollInVillageGoal::GetPosition(glm::dvec3& out) {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        JavaRandom& random = level->Random();
        std::optional<glm::dvec3> target;
        if (random.NextFloat() < 0.3f) {
            target = GetPositionTowardsAnywhere();
        } else {
            if (random.NextFloat() < 0.7f) {
                target = GetPositionTowardsVillagerWhoWantsGolem();
                if (!target) target = GetPositionTowardsPoi();
            } else {
                target = GetPositionTowardsPoi();
                if (!target) target = GetPositionTowardsVillagerWhoWantsGolem();
            }
            if (!target) target = GetPositionTowardsAnywhere();
        }
        if (!target) return false;
        out = *target;
        return true;
    }

    std::optional<glm::dvec3> GolemRandomStrollInVillageGoal::GetPositionTowardsAnywhere() {
        // LandRandomPos.getPos(mob, 10, 7).
        return RandomPos::GetLandPos(*m_mob, 10, 7);
    }

    std::optional<glm::dvec3> GolemRandomStrollInVillageGoal::GetPositionTowardsVillagerWhoWantsGolem() {
        // level.getEntities(VILLAGER, box.inflate(32), doesVillagerWantGolem).
        EntityLevel* level = m_mob->Level();
        const int64_t gameTime = level->GetGameTime();
        std::vector<Entity*> found;
        level->GetEntitiesInBox(Inflate(m_mob->GetAABB(), 32.0f, 32.0f, 32.0f), m_mob, found);
        std::vector<Villager*> villagers;
        for (Entity* e : found) {
            if (!e || e->GetType() != EntityTypeId::Villager || e->IsRemoved()) continue;
            auto* villager = static_cast<Villager*>(e);
            if (villager->WantsToSpawnGolem(gameTime)) villagers.push_back(villager);
        }
        if (villagers.empty()) return std::nullopt;
        Villager* villager = villagers[static_cast<size_t>(
            level->Random().NextInt(static_cast<int>(villagers.size())))];
        // LandRandomPos.getPosTowards(mob, 10, 7, villager.position()).
        return RandomPos::GetLandPosTowards(*m_mob, 10, 7, villager->position);
    }

    std::optional<glm::dvec3> GolemRandomStrollInVillageGoal::GetPositionTowardsPoi() {
        EntityLevel* level = m_mob->Level();
        PoiManager* poi = level->GetPoiManager();
        if (!poi) return std::nullopt;

        // getRandomVillageSection: the sections of the 5x5x5 cube around the
        // golem's own that ARE village (sectionsToVillage == 0).
        const glm::ivec3 center = SectionOf(m_mob->BlockPosition());
        std::vector<glm::ivec3> villageSections;
        for (int dz = -2; dz <= 2; ++dz) {
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const glm::ivec3 s = center + glm::ivec3(dx, dy, dz);
                    if (poi->SectionsToVillage(s.x, s.y, s.z) == 0) villageSections.push_back(s);
                }
            }
        }
        if (villageSections.empty()) return std::nullopt;
        const glm::ivec3 targetSection = villageSections[static_cast<size_t>(
            level->Random().NextInt(static_cast<int>(villageSections.size())))];

        // getRandomPoiWithinSection: any type, within 8 of the section
        // centre, IS_OCCUPIED.
        const std::vector<const PoiManager::Record*> pois = poi->GetInRange(
            [](PoiType) { return true; }, SectionCenter(targetSection), 8, PoiManager::Occupancy::IsOccupied);
        if (pois.empty()) return std::nullopt;
        const glm::ivec3 targetPos = pois[static_cast<size_t>(
            level->Random().NextInt(static_cast<int>(pois.size())))]->pos;
        // LandRandomPos.getPosTowards(mob, 10, 7, atBottomCenterOf(targetPos)).
        return RandomPos::GetLandPosTowards(*m_mob, 10, 7, AtBottomCenterOf(targetPos));
    }

    // ── OfferFlowerGoal ─────────────────────────────────────────────────────

    OfferFlowerGoal::OfferFlowerGoal(IronGolem* golem)
        : m_golem(golem), m_offerTargetContext(TargetingConditions::ForNonCombat().Range(6.0)) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    AABBd OfferFlowerGoal::GolemBoundingBox() const {
        AABBd box = m_golem->GetAABBd();
        box.min -= glm::dvec3(6.0, 2.0, 6.0);
        box.max += glm::dvec3(6.0, 2.0, 6.0);
        return box;
    }

    bool OfferFlowerGoal::CanUse() {
        EntityLevel* level = m_golem->Level();
        if (!level || !m_golem->IsBrightOutside()) return false;
        if (level->Random().NextInt(8000) != 0) return false;

        // getNearestEntity(#candidate_for_iron_golem_gift,
        // OFFER_TARGET_CONTEXT, golem, x, y, z, getGolemBoundingBox()).
        const AABBd searchBox = GolemBoundingBox();
        AABB queryBox;
        queryBox.min = glm::vec3(searchBox.min);
        queryBox.max = glm::vec3(searchBox.max);
        std::vector<Entity*> found;
        level->GetEntitiesInBox(queryBox, m_golem, found);
        m_entity = nullptr;
        double bestDistSq = -1.0;
        for (Entity* e : found) {
            LivingEntity* living = e ? e->AsLiving() : nullptr;
            if (!living || living->IsRemoved()) continue;
            if (!living->GetAABBd().Intersects(searchBox)) continue;
            if (!HasEntityTypeTag(*living, "minecraft:candidate_for_iron_golem_gift")) continue;
            if (!m_offerTargetContext.Test(m_golem, *living)) continue;
            const double d = living->DistanceToSqr(m_golem->position.x, m_golem->position.y, m_golem->position.z);
            if (bestDistSq == -1.0 || d < bestDistSq) {
                bestDistSq = d;
                m_entity = living;
            }
        }
        return m_entity != nullptr;
    }

    bool OfferFlowerGoal::CanContinueToUse() {
        return m_tick > 0;
    }

    void OfferFlowerGoal::Start() {
        m_tick = AdjustedTickDelay(kOfferTicks);
        m_golem->OfferFlower(true);
    }

    void OfferFlowerGoal::Stop() {
        m_golem->OfferFlower(false);
        if (m_tick == 0 && m_entity) {
            // A copper golem (#accepts_iron_golem_gift) with a bare antenna,
            // still inside the golem's offer box, takes the poppy.
            if (auto* mob = dynamic_cast<Mob*>(m_entity);
                mob && !mob->IsRemoved() && HasEntityTypeTag(*mob, "minecraft:accepts_iron_golem_gift") &&
                mob->GetEquipment(CopperGolem::kAntennaSlot).IsEmpty() &&
                GolemBoundingBox().Intersects(mob->GetAABBd())) {
                ItemStack poppy;
                poppy.itemId = static_cast<ItemID>(ItemRegistry::FromBlock(BlockID::Poppy));
                poppy.count = 1;
                mob->SetEquipment(CopperGolem::kAntennaSlot, poppy);
                mob->SetGuaranteedDrop(CopperGolem::kAntennaSlot);
            }
        }
        m_entity = nullptr;
    }

    void OfferFlowerGoal::Tick() {
        if (m_entity) {
            m_golem->GetLookControl().SetLookAt(m_entity->position.x, m_entity->GetEyeY(), m_entity->position.z,
                                                30.0f, 30.0f);
        }
        --m_tick;
    }

    void OfferFlowerGoal::ClearReferenceTo(const Entity* entity) {
        if (entity && static_cast<const Entity*>(m_entity) == entity) m_entity = nullptr;
    }

    // ── DefendVillageTargetGoal ─────────────────────────────────────────────

    DefendVillageTargetGoal::DefendVillageTargetGoal(IronGolem* golem)
        : TargetGoal(golem, /*mustSee=*/false, /*mustReach=*/true), m_golem(golem),
          m_attackTargeting(TargetingConditions::ForCombat().Range(64.0)) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Target));
    }

    bool DefendVillageTargetGoal::CanUse() {
        EntityLevel* level = m_golem->Level();
        if (!level) return false;
        const AABB grow = Inflate(m_golem->GetAABB(), 10.0f, 8.0f, 10.0f);

        // level.getNearbyEntities(Villager.class, attackTargeting, golem, grow)
        // and level.getNearbyPlayers(attackTargeting, golem, grow).
        std::vector<Entity*> found;
        level->GetEntitiesInBox(grow, m_golem, found);
        std::vector<Villager*> villagers;
        for (Entity* e : found) {
            if (!e || e->GetType() != EntityTypeId::Villager || e->IsRemoved()) continue;
            auto* villager = static_cast<Villager*>(e);
            if (m_attackTargeting.Test(m_golem, *villager)) villagers.push_back(villager);
        }
        std::vector<LivingEntity*> allPlayers;
        level->GetPlayers(allPlayers);
        std::vector<LivingEntity*> players;
        for (LivingEntity* player : allPlayers) {
            // getNearbyPlayers: the box CONTAINS the player's position.
            if (!player) continue;
            const glm::dvec3& p = player->position;
            if (!(p.x >= grow.min.x && p.x < grow.max.x && p.y >= grow.min.y && p.y < grow.max.y &&
                  p.z >= grow.min.z && p.z < grow.max.z)) {
                continue;
            }
            if (m_attackTargeting.Test(m_golem, *player)) players.push_back(player);
        }

        for (Villager* villager : villagers) {
            for (LivingEntity* player : players) {
                if (villager->GetPlayerReputation(*player) <= -100) m_potentialTarget = player;
            }
        }

        if (m_potentialTarget == nullptr) return false;
        if (m_potentialTarget->IsPlayer() &&
            (m_potentialTarget->IsSpectator() || m_potentialTarget->IsCreative())) {
            return false;
        }
        return true;
    }

    void DefendVillageTargetGoal::Start() {
        m_golem->SetTarget(m_potentialTarget);
        TargetGoal::Start();
    }

    void DefendVillageTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (entity && static_cast<const Entity*>(m_potentialTarget) == entity) m_potentialTarget = nullptr;
    }

} // namespace Game
