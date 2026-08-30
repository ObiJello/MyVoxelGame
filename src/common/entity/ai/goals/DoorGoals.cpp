// File: src/common/entity/ai/goals/DoorGoals.cpp
#include "common/entity/ai/goals/DoorGoals.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/Path.hpp"
#include "common/world/pathfinder/Node.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <string_view>

namespace Game {

    namespace {
        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    } // namespace

    // ── DoorInteractGoal ───────────────────────────────────────────────────

    DoorInteractGoal::DoorInteractGoal(Mob* mob) : m_mob(mob) {
        // MC throws for a mob without ground navigation; every registrant
        // here (zombie family, vindicator) walks, so the guard is a comment.
        // MC's base declares NO goal flags — a pounding zombie keeps walking
        // into the door, which is the vanilla look.
    }

    bool DoorInteractGoal::IsWoodenDoorAt(const EntityLevel& level,
                                          const glm::ivec3& pos) {
        // MC DoorBlock.isWoodenDoor: a DoorBlock whose type canOpenByHand —
        // every "*_door" but iron_door (the copper doors open by hand and MC
        // lets them be broken too).
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;
        const BlockID id = blocks->GetBlock(pos.x, pos.y, pos.z);
        if (id == BlockID::Air || id == BlockID::IronDoor) return false;
        return EndsWith(BlockRegistry::Get(id).registrySlug, "_door");
    }

    bool DoorInteractGoal::IsOpen() {
        // MC DoorInteractGoal.isOpen.
        if (!m_hasDoor) return false;
        EntityLevel* level = m_mob->Level();
        if (!level || !level->Blocks()) return false;
        const BlockState state =
            level->Blocks()->GetBlockState(m_doorPos.x, m_doorPos.y, m_doorPos.z);
        if (!IsWoodenDoorAt(*level, m_doorPos)) {
            m_hasDoor = false;
            return false;
        }
        return state.GetName(PropertyId::OPEN) == "true";
    }

    bool DoorInteractGoal::CanUse() {
        // MC canUse: only while shoving into something, with a live path
        // whose next couple of nodes cross a wooden door (path nodes stand at
        // FEET level; the door block the mob faces is one above).
        if (!m_mob->horizontalCollision) return false;
        EntityLevel* level = m_mob->Level();
        if (!level) return false;

        const Path* path = m_mob->GetNavigation().GetPath();
        if (path && !path->IsDone()) {
            const int count =
                std::min(path->GetNextNodeIndex() + 2, path->GetNodeCount());
            for (int i = 0; i < count; ++i) {
                const Node& node = path->GetNode(i);
                m_doorPos = glm::ivec3(node.x, node.y + 1, node.z);
                if (m_mob->DistanceToSqr(static_cast<double>(m_doorPos.x),
                                         m_mob->position.y,
                                         static_cast<double>(m_doorPos.z)) > 2.25) {
                    continue;
                }
                m_hasDoor = IsWoodenDoorAt(*level, m_doorPos);
                if (m_hasDoor) return true;
            }

            const glm::ivec3 above = m_mob->BlockPosition() + glm::ivec3(0, 1, 0);
            m_doorPos = above;
            m_hasDoor = IsWoodenDoorAt(*level, m_doorPos);
            return m_hasDoor;
        }
        return false;
    }

    bool DoorInteractGoal::CanContinueToUse() { return !m_passed; }

    void DoorInteractGoal::Start() {
        m_passed = false;
        m_doorOpenDirX = static_cast<float>(
            (static_cast<double>(m_doorPos.x) + 0.5) - m_mob->position.x);
        m_doorOpenDirZ = static_cast<float>(
            (static_cast<double>(m_doorPos.z) + 0.5) - m_mob->position.z);
    }

    void DoorInteractGoal::Tick() {
        // MC tick: once the mob is on the far side of the door plane (the dot
        // flips sign), the interaction is over.
        const float newDirX = static_cast<float>(
            (static_cast<double>(m_doorPos.x) + 0.5) - m_mob->position.x);
        const float newDirZ = static_cast<float>(
            (static_cast<double>(m_doorPos.z) + 0.5) - m_mob->position.z);
        if (m_doorOpenDirX * newDirX + m_doorOpenDirZ * newDirZ < 0.0f) {
            m_passed = true;
        }
    }

    // ── BreakDoorGoal ──────────────────────────────────────────────────────

    BreakDoorGoal::BreakDoorGoal(Mob* mob, DifficultyPredicate validDifficulty)
        : DoorInteractGoal(mob), m_validDifficulty(validDifficulty) {}

    BreakDoorGoal::BreakDoorGoal(Mob* mob, int doorBreakTime,
                                 DifficultyPredicate validDifficulty)
        : BreakDoorGoal(mob, validDifficulty) {
        m_doorBreakTime = doorBreakTime;
    }

    bool BreakDoorGoal::IsValidDifficulty() const {
        return m_mob->Level() && m_validDifficulty &&
               m_validDifficulty(m_mob->Level()->GetDifficulty());
    }

    bool BreakDoorGoal::CanUse() {
        if (!DoorInteractGoal::CanUse()) return false;
        // MC: the MOB_GRIEFING game rule, then the difficulty gate, then the
        // door must still be shut.
        if (!m_mob->Level() || !m_mob->Level()->MobGriefing()) return false;
        return IsValidDifficulty() && !IsOpen();
    }

    void BreakDoorGoal::Start() {
        DoorInteractGoal::Start();
        m_breakTime = 0;
    }

    bool BreakDoorGoal::CanContinueToUse() {
        // MC: still counting, still shut, still close (2.0 to the door
        // centre), still the right difficulty.
        const double dx = (static_cast<double>(m_doorPos.x) + 0.5) - m_mob->position.x;
        const double dy = (static_cast<double>(m_doorPos.y) + 0.5) - m_mob->position.y;
        const double dz = (static_cast<double>(m_doorPos.z) + 0.5) - m_mob->position.z;
        return m_breakTime <= GetDoorBreakTime() && !IsOpen() &&
               dx * dx + dy * dy + dz * dz < 4.0 && IsValidDifficulty();
    }

    void BreakDoorGoal::Stop() {
        DoorInteractGoal::Stop();
        // MC resets destroyBlockProgress(-1) — the crack overlay; no
        // block-damage render pipeline exists to reset.
    }

    void BreakDoorGoal::Tick() {
        DoorInteractGoal::Tick();
        EntityLevel* level = m_mob->Level();
        if (!level) return;

        // MC: a pound roughly every 20 ticks — level event 1019 (the wood
        // thud; no sound system) plus the arm swing every watcher sees.
        if (level->Random().NextInt(20) == 0) {
            if (!m_mob->swinging) m_mob->Swing();
        }

        ++m_breakTime;
        // MC destroyBlockProgress(id, doorPos, progress) — the 0..9 crack
        // overlay; the bookkeeping is kept so the wiring reads like MC's, the
        // overlay itself has no render pipeline.
        const int progress = static_cast<int>(
            static_cast<float>(m_breakTime) /
            static_cast<float>(GetDoorBreakTime()) * 10.0f);
        if (progress != m_lastBreakProgress) {
            m_lastBreakProgress = progress;
        }

        if (m_breakTime == GetDoorBreakTime() && IsValidDifficulty()) {
            // MC removeBlock(doorPos, false) — no drops; the UPPER half then
            // falls out via the door's neighbour update. This engine has no
            // double-block linkage (see World::NotifyNeighborBlocks), so both
            // halves are cleared explicitly — same observable result.
            // Level events 1021/2001 (break sound + particles) skipped.
            const IBlockAccess* blocks = level->Blocks();
            if (blocks) {
                const BlockState state = blocks->GetBlockState(
                    m_doorPos.x, m_doorPos.y, m_doorPos.z);
                const glm::ivec3 other =
                    state.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "lower"
                        ? m_doorPos + glm::ivec3(0, 1, 0)
                        : m_doorPos - glm::ivec3(0, 1, 0);
                level->SetBlock(m_doorPos, BlockID::Air);
                if (IsWoodenDoorAt(*level, other)) {
                    level->SetBlock(other, BlockID::Air);
                }
            }
        }
    }

    // ── VindicatorBreakDoorGoal ────────────────────────────────────────────

    VindicatorBreakDoorGoal::VindicatorBreakDoorGoal(Mob* mob)
        : BreakDoorGoal(mob, 6, &BreakDoorGoal::NormalOrHard) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool VindicatorBreakDoorGoal::CanUse() {
        // MC: hasActiveRaid() && rand(reducedTickDelay(10)) == 0 && super —
        // the raid gate is treated as satisfied (DEVIATION, see the header).
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        if (level->Random().NextInt(ReducedTickDelay(10)) != 0) return false;
        return BreakDoorGoal::CanUse();
    }

    void VindicatorBreakDoorGoal::Start() {
        BreakDoorGoal::Start();
        m_mob->SetNoActionTime(0);
    }

} // namespace Game
