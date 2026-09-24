// File: src/common/entity/Monster.cpp
#include "common/entity/Monster.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

namespace Game {

    Monster::Monster(EntityTypeId type, EntityLevel* level)
        : PathfinderMob(type, level) {
        CreateMonsterAttributes(m_attributes);
        m_health = GetMaxHealth();
    }

    float Monster::GetWalkTargetValue(const glm::ivec3& pos) const {
        if (!m_level) return 0.0f;
        // MC: -getPathfindingCostFromLightLevels. NEGATED, unlike Animal —
        // RandomPos keeps the HIGHEST-scoring candidate, so this makes a
        // monster prefer dark positions. The sign also feeds PathfinderMob's
        // post-spawn CheckSpawnRules (walk value >= 0), which for a monster
        // means brightness <= 12.
        return -PathfindingCostFromLightLevels(*m_level, pos.x, pos.y, pos.z);
    }

    void Monster::UpdateNoActionTime() {
        // MC: +2 extra when getLightLevelDependentMagicValue() > 0.5F — i.e.
        // brightness >= 13, not >= 8 (the curve crosses 0.5 at 12). Combined
        // with the +1 that ServerAiStep always adds, a monster in daylight
        // reaches the 600-tick despawn threshold three times sooner.
        if (!m_level) return;
        const glm::ivec3 p = BlockPosition();
        if (LightLevelDependentMagicValue(*m_level, p.x, p.y, p.z) > 0.5f) {
            m_noActionTime += 2;
        }
    }

    void Monster::AiStep() {
        UpdateNoActionTime();
        PathfinderMob::AiStep();
    }

    bool Monster::IsDarkEnoughToSpawn(EntityLevel& level, const glm::ivec3& pos,
                                      JavaRandom& rng) {
        // MC Monster.isDarkEnoughToSpawn, all three tests in order.

        // 1. RAW sky light vs a 0..31 roll. This is the one that is easy to
        //    miss and it matters a lot: raw sky light ignores time of day, so
        //    outdoors it is 15 at midnight too, and 15 > nextInt(32) rejects
        //    roughly 47% of every open-sky attempt around the clock. It is a
        //    large part of why caves out-spawn the surface even at night.
        if (level.GetSkyBrightness(pos.x, pos.y, pos.z) > rng.NextInt(32)) {
            return false;
        }

        // The dimension type's two spawn-light fields (26.x dimension_type
        // JSON): monster_spawn_block_light_limit and monster_spawn_light_level.
        // Overworld (and the engine's overworld-like dimensions): 0 and
        // UniformInt(0, 7). Nether: 15 and the constant 7. End: 0 and the
        // constant 15. A constant draws no random number — MC's
        // ConstantInt.sample never touches the RandomSource.
        int blockLightLimit = 0;
        int constantLightLevel = -1;             // -1: UniformInt(0, 7)
        switch (level.Dimension()) {
            case DimensionId::Nether: blockLightLimit = 15; constantLightLevel = 7;  break;
            case DimensionId::End:    blockLightLimit = 0;  constantLightLevel = 15; break;
            default: break;
        }

        // 2. Block light vs the dimension's limit — a torch-lit cave stops
        //    spawning here.
        if (blockLightLimit < 15 && level.GetBlockBrightness(pos.x, pos.y, pos.z) > blockLightLimit) {
            return false;
        }

        // 3. Effective brightness vs the dimension's light-level sample.
        //    Thunder subtracts 10 instead of the usual skyDarken, which is what
        //    lets mobs spawn on the surface during a daytime storm.
        const int brightness = level.IsThundering()
            ? level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, 10)
            : level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z);

        return brightness <= (constantLightLevel >= 0 ? constantLightLevel : rng.NextInt(8));
    }

    bool Monster::CheckMonsterSpawnRules(EntityLevel& level, SpawnReason reason,
                                         const glm::ivec3& pos, JavaRandom& rng) {
        return level.GetDifficulty() != Difficulty::Peaceful &&
               (IgnoresLightRequirements(reason) || IsDarkEnoughToSpawn(level, pos, rng)) &&
               CheckAnyLightMonsterSpawnRules(level, reason, pos, rng);
    }

    bool Monster::CheckAnyLightMonsterSpawnRules(EntityLevel& level, SpawnReason reason,
                                                 const glm::ivec3& pos, JavaRandom& rng) {
        // MC checkAnyLightMonsterSpawnRules = difficulty != PEACEFUL &&
        // checkMobSpawnRules. The block-below test is MC's
        // BlockState.isValidSpawn default: sturdy TOP face (a slab counts, a
        // fence post does not), shared with SpawnPlacements' ON_GROUND check.
        (void)rng;
        if (level.GetDifficulty() == Difficulty::Peaceful) return false;
        if (IsSpawner(reason)) return true;
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;
        return IsValidSpawnBlock(*blocks, pos.x, pos.y - 1, pos.z);
    }

    bool Monster::CheckSurfaceMonstersSpawnRules(EntityLevel& level, SpawnReason reason,
                                                 const glm::ivec3& pos, JavaRandom& rng) {
        return CheckMonsterSpawnRules(level, reason, pos, rng) &&
               (IsSpawner(reason) || level.CanSeeSky(pos.x, pos.y, pos.z));
    }

} // namespace Game
