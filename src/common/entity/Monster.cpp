// File: src/common/entity/Monster.cpp
#include "common/entity/Monster.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

namespace Game {

    Monster::Monster(EntityTypeId type, EntityLevel* level)
        : PathfinderMob(type, level) {
        CreateMonsterAttributes(m_attributes);
        m_health = GetMaxHealth();
    }

    float Monster::GetWalkTargetValue(const glm::ivec3& pos) const {
        if (!m_level) return 0.0f;
        // NEGATED, unlike Animal. RandomPos keeps the HIGHEST-scoring candidate,
        // so negating the light level makes a monster prefer dark positions.
        return -static_cast<float>(m_level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z));
    }

    void Monster::UpdateNoActionTime() {
        // MC: +2 extra when the light level exceeds half brightness. Combined
        // with the +1 that ServerAiStep always adds, a monster in daylight
        // reaches the 600-tick despawn threshold three times sooner.
        if (!m_level) return;
        const glm::ivec3 p = BlockPosition();
        const int light = m_level->GetMaxLocalRawBrightness(p.x, p.y, p.z);
        if (light > 7) {
            m_noActionTime += 2;
        }
    }

    void Monster::AiStep() {
        UpdateNoActionTime();
        PathfinderMob::AiStep();
    }

    bool Monster::IsDarkEnoughToSpawn(EntityLevel& level, const glm::ivec3& pos) {
        // MC Monster.isDarkEnoughToSpawn, all three tests in order.

        // 1. RAW sky light vs a 0..31 roll. This is the one that is easy to
        //    miss and it matters a lot: raw sky light ignores time of day, so
        //    outdoors it is 15 at midnight too, and 15 > nextInt(32) rejects
        //    roughly 47% of every open-sky attempt around the clock. It is a
        //    large part of why caves out-spawn the surface even at night.
        if (level.GetSkyBrightness(pos.x, pos.y, pos.z) > level.Random().NextInt(32)) {
            return false;
        }

        // 2. Block light vs the dimension's limit (0 in the overworld). No
        //    light engine here means block light is always 0, so this never
        //    rejects yet — it will start working the day torches emit light,
        //    with no change needed at this call site.
        constexpr int kOverworldBlockLightLimit = 0;
        constexpr int kBlockLight = 0;
        if (kOverworldBlockLightLimit < 15 && kBlockLight > kOverworldBlockLightLimit) {
            return false;
        }

        // 3. Effective brightness vs a UniformInt(0, 7) sample — the overworld
        //    dimension type's monster_spawn_light_level. Thunder subtracts 10
        //    instead of the usual skyDarken, which is what lets mobs spawn on
        //    the surface during a daytime storm.
        const int brightness = level.IsThundering()
            ? level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, 10)
            : level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z);

        return brightness <= level.Random().NextInt(8);
    }

    bool Monster::CheckMonsterSpawnRules(EntityLevel& level, const glm::ivec3& pos) {
        if (level.GetDifficulty() == Difficulty::Peaceful) return false;
        if (!IsDarkEnoughToSpawn(level, pos)) return false;

        // MC Mob.checkMobSpawnRules — the block below must be a valid spawn
        // surface, which for everything but a few special cases means solid.
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;
        return BlockRegistry::HasCollision(blocks->GetBlock(pos.x, pos.y - 1, pos.z));
    }

} // namespace Game
