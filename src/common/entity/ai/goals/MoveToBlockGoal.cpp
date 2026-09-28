// File: src/common/entity/ai/goals/MoveToBlockGoal.cpp
#include "common/entity/ai/goals/MoveToBlockGoal.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"

#include <cmath>

namespace Game {

    // ── MoveToBlockGoal ────────────────────────────────────────────────────

    MoveToBlockGoal::MoveToBlockGoal(PathfinderMob* mob, double speedModifier,
                                     int searchRange, int verticalSearchRange)
        : m_mob(mob), m_speedModifier(speedModifier),
          m_searchRange(searchRange), m_verticalSearchRange(verticalSearchRange) {
        SetFlags(GoalFlag::Move | GoalFlag::Jump);
    }

    int MoveToBlockGoal::NextStartTick() const {
        return ReducedTickDelay(kIntervalTicks +
                                m_mob->Level()->Random().NextInt(kIntervalTicks));
    }

    bool MoveToBlockGoal::CanUse() {
        if (m_nextStartTick > 0) {
            --m_nextStartTick;
            return false;
        }
        m_nextStartTick = NextStartTick();
        return FindNearestBlock();
    }

    bool MoveToBlockGoal::CanContinueToUse() {
        const IBlockAccess* blocks = m_mob->Level() ? m_mob->Level()->Blocks() : nullptr;
        if (!blocks) return false;
        return m_tryTicks >= -m_maxStayTicks && m_tryTicks <= kGiveUpTicks &&
               IsValidTarget(*blocks, m_blockPos);
    }

    void MoveToBlockGoal::Start() {
        MoveMobToBlock();
        m_tryTicks = 0;
        JavaRandom& rng = m_mob->Level()->Random();
        m_maxStayTicks = rng.NextInt(rng.NextInt(kStayTicks) + kStayTicks) + kStayTicks;
    }

    void MoveToBlockGoal::MoveMobToBlock() {
        m_mob->GetNavigation().MoveTo(m_blockPos.x + 0.5, m_blockPos.y + 1,
                                      m_blockPos.z + 0.5, m_speedModifier);
    }

    glm::ivec3 MoveToBlockGoal::GetMoveToTarget() const {
        return { m_blockPos.x, m_blockPos.y + 1, m_blockPos.z };
    }

    void MoveToBlockGoal::Tick() {
        const glm::ivec3 target = GetMoveToTarget();
        // MC BlockPos.closerToCenterThan(mob.position(), acceptedDistance()).
        const double dx = (target.x + 0.5) - m_mob->position.x;
        const double dy = target.y - m_mob->position.y;
        const double dz = (target.z + 0.5) - m_mob->position.z;
        const double acceptedSq = AcceptedDistance() * AcceptedDistance();

        if (dx * dx + dy * dy + dz * dz > acceptedSq) {
            m_reachedTarget = false;
            ++m_tryTicks;
            if (ShouldRecalculatePath()) {
                m_mob->GetNavigation().MoveTo(target.x + 0.5, target.y,
                                              target.z + 0.5, m_speedModifier);
            }
        } else {
            m_reachedTarget = true;
            --m_tryTicks;
        }
    }

    bool MoveToBlockGoal::FindNearestBlock() {
        const IBlockAccess* blocks = m_mob->Level() ? m_mob->Level()->Blocks() : nullptr;
        if (!blocks) return false;

        const glm::ivec3 mobPos = m_mob->BlockPosition();

        // MC's expanding-ring scan, transcribed exactly: y alternates
        // 0, -1, 1, -2, ... within the vertical range; each ring r sweeps x
        // and z the same alternating way, so the FIRST hit is the nearest.
        for (int y = m_verticalSearchStart; y <= m_verticalSearchRange;
             y = y > 0 ? -y : 1 - y) {
            for (int r = 0; r < m_searchRange; ++r) {
                for (int x = 0; x <= r; x = x > 0 ? -x : 1 - x) {
                    for (int z = (x < r && x > -r) ? r : 0; z <= r;
                         z = z > 0 ? -z : 1 - z) {
                        // MC also checks mob.isWithinHome; no home-restriction
                        // system exists here, so every position qualifies.
                        const glm::ivec3 pos(mobPos.x + x, mobPos.y + y - 1,
                                             mobPos.z + z);
                        if (IsValidTarget(*blocks, pos)) {
                            m_blockPos = pos;
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    // ── RemoveBlockGoal ────────────────────────────────────────────────────

    RemoveBlockGoal::RemoveBlockGoal(BlockID blockToRemove, PathfinderMob* mob,
                                     double speedModifier, int verticalSearchRange)
        : MoveToBlockGoal(mob, speedModifier, 24, verticalSearchRange),
          m_blockToRemove(blockToRemove) {}

    bool RemoveBlockGoal::CanUse() {
        if (!m_mob->Level() || !m_mob->Level()->MobGriefing()) return false;
        if (m_nextStartTick > 0) {
            --m_nextStartTick;
            return false;
        }
        if (FindNearestBlock()) {
            m_nextStartTick = ReducedTickDelay(20);
            return true;
        }
        m_nextStartTick = NextStartTick();
        return false;
    }

    void RemoveBlockGoal::Start() {
        MoveToBlockGoal::Start();
        m_ticksSinceReachedGoal = 0;
    }

    void RemoveBlockGoal::Stop() {
        // MC: leaving the goal pins fallDistance to 1 so the last stomp's hop
        // cannot bank fall damage.
        m_mob->fallDistance = 1.0f;
    }

    void RemoveBlockGoal::Tick() {
        MoveToBlockGoal::Tick();

        EntityLevel* level = m_mob->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;

        glm::ivec3 eatPos;
        if (!IsReachedTarget() ||
            !GetPosWithBlock(*blocks, m_mob->BlockPosition(), eatPos)) {
            return;
        }

        // The stomp: alternate hops (+0.3 up, then -0.3 down every other
        // tick), a puff of egg crumbs on every hop, the progress sound on
        // every sixth tick, and after three seconds the block is gone in a
        // cloud of POOF with the break sound (RemoveBlockGoal.tick).
        JavaRandom& random = level->Random();   // removerMob.getRandom()
        if (m_ticksSinceReachedGoal > 0) {
            m_mob->velocity.y = 0.3;
            if (!level->IsClientSide()) {
                // sendParticles(ITEM egg, eat + (0.5, 0.7, 0.5), 3, three
                // (nextFloat - 0.5) * 0.08 spreads, speed 0.15).
                const double dx = (static_cast<double>(random.NextFloat()) - 0.5) * 0.08;
                const double dy = (static_cast<double>(random.NextFloat()) - 0.5) * 0.08;
                const double dz = (static_cast<double>(random.NextFloat()) - 0.5) * 0.08;
                level->SendParticles(ParticleOptions::Item(Items::Egg), eatPos.x + 0.5, eatPos.y + 0.7,
                                     eatPos.z + 0.5, 3, dx, dy, dz, 0.15000000596046448);
            }
        }
        if (m_ticksSinceReachedGoal % 2 == 0) {
            m_mob->velocity.y = -0.3;
            if (m_ticksSinceReachedGoal % 6 == 0) {
                PlayDestroyProgressSound(*level, m_blockPos);
            }
        }

        if (m_ticksSinceReachedGoal > 60) {
            // MC level.removeBlock(pos, false) — no drops.
            level->SetBlock(eatPos, BlockID::Air);
            if (!level->IsClientSide()) {
                for (int i = 0; i < 20; ++i) {
                    const double xa = random.NextGaussian() * 0.02;
                    const double ya = random.NextGaussian() * 0.02;
                    const double za = random.NextGaussian() * 0.02;
                    level->SendParticles(ParticleKind::Poof, eatPos.x + 0.5, eatPos.y, eatPos.z + 0.5, 1,
                                         xa, ya, za, 0.15000000596046448);
                }
                PlayBreakSound(*level, eatPos);
            }
        }

        ++m_ticksSinceReachedGoal;
    }

    bool RemoveBlockGoal::GetPosWithBlock(const IBlockAccess& blocks,
                                          const glm::ivec3& pos, glm::ivec3& out) const {
        const glm::ivec3 candidates[] = {
            pos,
            { pos.x, pos.y - 1, pos.z },
            { pos.x - 1, pos.y, pos.z },
            { pos.x + 1, pos.y, pos.z },
            { pos.x, pos.y, pos.z - 1 },
            { pos.x, pos.y, pos.z + 1 },
            { pos.x, pos.y - 2, pos.z },
        };
        for (const glm::ivec3& c : candidates) {
            if (blocks.GetBlock(c.x, c.y, c.z) == m_blockToRemove) {
                out = c;
                return true;
            }
        }
        return false;
    }

    bool RemoveBlockGoal::IsValidTarget(const IBlockAccess& blocks,
                                        const glm::ivec3& pos) const {
        if (!blocks.IsChunkLoaded(pos.x >> 4, pos.z >> 4)) return false;
        return blocks.GetBlock(pos.x, pos.y, pos.z) == m_blockToRemove &&
               blocks.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Air &&
               blocks.GetBlock(pos.x, pos.y + 2, pos.z) == BlockID::Air;
    }

    // ── RaidGardenGoal ─────────────────────────────────────────────────────

    // ── ZombieAttackTurtleEggGoal's sounds ────────────────────────────────

    void ZombieAttackTurtleEggGoal::PlayDestroyProgressSound(EntityLevel& level, const glm::ivec3& pos) {
        level.PlaySound(nullptr, pos, SoundEvents::ZOMBIE_DESTROY_EGG, SoundSource::Hostile, 0.5f,
                        0.9f + level.Random().NextFloat() * 0.2f);
    }

    void ZombieAttackTurtleEggGoal::PlayBreakSound(EntityLevel& level, const glm::ivec3& pos) {
        level.PlaySound(nullptr, pos, SoundEvents::TURTLE_EGG_BREAK, SoundSource::Blocks, 0.7f,
                        0.9f + level.Random().NextFloat() * 0.2f);
    }

    RaidGardenGoal::RaidGardenGoal(Rabbit* rabbit)
        : MoveToBlockGoal(rabbit, 0.7, 16), m_rabbit(rabbit) {}

    bool RaidGardenGoal::CanUse() {
        // MC Rabbit.RaidGardenGoal.canUse, transcribed: on a fresh scan tick
        // roll the appetite and the griefing gate, then let the base scan.
        if (m_nextStartTick <= 0) {
            if (!m_rabbit->Level() || !m_rabbit->Level()->MobGriefing()) {
                return false;
            }
            m_canRaid = false;
            m_wantsToRaid = m_rabbit->WantsMoreFood();
        }
        return MoveToBlockGoal::CanUse();
    }

    bool RaidGardenGoal::CanContinueToUse() {
        return m_canRaid && MoveToBlockGoal::CanContinueToUse();
    }

    void RaidGardenGoal::Tick() {
        MoveToBlockGoal::Tick();
        m_rabbit->GetLookControl().SetLookAt(
            m_blockPos.x + 0.5, m_blockPos.y + 1, m_blockPos.z + 0.5, 10.0f,
            static_cast<float>(m_rabbit->GetMaxHeadXRot()));

        if (!IsReachedTarget()) return;

        EntityLevel* level = m_rabbit->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;

        const glm::ivec3 cropsPos(m_blockPos.x, m_blockPos.y + 1, m_blockPos.z);
        const BlockState state =
            blocks->GetBlockState(cropsPos.x, cropsPos.y, cropsPos.z);
        if (m_canRaid && state.Is(BlockID::Carrots)) {
            const int age = state.GetIndex(PropertyId::AGE_7);
            if (age == 0) {
                // MC, verbatim ODDITY: setBlock(AIR, 2) FIRST and then
                // destroyBlock(pos, true) — which re-reads air and therefore
                // drops nothing. A vanilla rabbit eats the last carrot stage
                // without loot, and so does this one.
                level->SetBlock(cropsPos, BlockID::Air);
                level->DestroyBlock(cropsPos, true);
            } else {
                // MC: setBlock(state.setValue(AGE, age - 1), 2) + level event
                // 2001 (the break particles and sound of the old state).
                level->SetBlockState(cropsPos,
                                     state.SetIndex(PropertyId::AGE_7, age - 1));
                PlayLevelEventSound(*level, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, cropsPos,
                                    static_cast<int>(state.RawId()), &level->Random());
                // MC: level.gameEvent(BLOCK_CHANGE, cropsPos, Context.of(rabbit)).
                if (ILevelWrite* write = level->MutableBlocks()) {
                    write->GameEvent(GameEventId::BlockChange, cropsPos, GameEventContext::Of(m_rabbit));
                }
            }
            m_rabbit->SetMoreCarrotTicks(40);
        }

        m_canRaid = false;
        m_nextStartTick = 10;
    }

    bool RaidGardenGoal::IsValidTarget(const IBlockAccess& blocks,
                                       const glm::ivec3& pos) const {
        // MC isValidTarget: farmland below a MAX-AGE carrot crop, only while
        // the rabbit wants more food and has not already locked a target.
        const BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Is(BlockID::Farmland) && m_wantsToRaid && !m_canRaid) {
            const BlockState above =
                blocks.GetBlockState(pos.x, pos.y + 1, pos.z);
            if (above.Is(BlockID::Carrots) &&
                above.GetIndex(PropertyId::AGE_7) == 7) {
                m_canRaid = true;
                return true;
            }
        }
        return false;
    }

} // namespace Game
