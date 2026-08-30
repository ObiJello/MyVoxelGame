// File: src/common/world/spawn/SpawnPlacements.hpp
//
// MC net.minecraft.world.entity.SpawnPlacements + SpawnPlacementTypes — the
// per-type answer to "may this mob type spawn AT this position?", split the
// same way MC splits it:
//
//   GetPlacementType / IsSpawnPositionOk   WHERE the type may stand: on the
//                                          ground with two free blocks, in
//                                          water, in lava, or anywhere.
//   CheckSpawnRules                        The type's OWN veto — light for
//                                          monsters, grass for animals, the
//                                          slime-chunk test, the drowned's
//                                          1-in-40 roll, and so on. Default is
//                                          TRUE for an unregistered type,
//                                          exactly as in MC.
//
// The registration table is transcribed from SpawnPlacements.java's static
// block. Types MC leaves unregistered (armor stands, projectiles — and of the
// mobs, none) fall through to NO_RESTRICTIONS + true.
//
// The tag sets the predicates consult are baked by tools/gen_spawn_tags.py.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string_view>

namespace Game {

    struct EntityLevel;
    struct IBlockAccess;
    class JavaRandom;

    enum class SpawnPlacementType : uint8_t {
        NoRestrictions,
        OnGround,
        InWater,
        InLava,
    };

    // MC Heightmap.Types, only the two values the placement table uses. Only
    // consulted by chunk-generation spawning (which this engine does not do
    // yet); stored so the table is a complete transcription.
    enum class SpawnHeightmap : uint8_t {
        MotionBlockingNoLeaves,
        MotionBlocking,          // ocelot and parrot only
    };

    // What a spawn-rule predicate may consult beyond the level: the rules are
    // static (they run before the mob exists), so everything positional has to
    // arrive through here.
    struct SpawnRuleContext {
        EntityLevel&        level;
        const IBlockAccess& blocks;
        JavaRandom&         rng;      // MC passes level.random; same stream here
        SpawnReason         reason = SpawnReason::Natural;

        // Biome slug at a world position ("plains") — Slime, Drowned,
        // TropicalFish and PolarBear branch on biome tags.
        const std::function<std::string_view(int, int, int)>* biomeAt = nullptr;

        // MC LevelAccessor.getHeightmapPos(WORLD_SURFACE, pos).getY() — the
        // bat rule spawns only strictly below the surface.
        const std::function<int(int, int)>* surfaceHeight = nullptr;

        // MC LevelReader.getSeaLevel — the water-surface rules are all
        // expressed relative to it.
        int seaLevel = 63;

        // MC WorldGenLevel.getSeed, for the slime-chunk hash.
        int64_t worldSeed = 0;
    };

    SpawnPlacementType GetSpawnPlacementType(EntityTypeId type);

    // MC SpawnPlacements.isSpawnPositionOk — dispatches on the placement type.
    bool IsSpawnPositionOk(EntityTypeId type, const IBlockAccess& blocks,
                           int x, int y, int z);

    // MC SpawnPlacements.checkSpawnRules — the per-type predicate, true when
    // the type is unregistered.
    bool CheckSpawnRules(EntityTypeId type, const SpawnRuleContext& ctx,
                         const glm::ivec3& pos);

    // ── Shared position tests (MC NaturalSpawner / BlockBehaviour) ─────────
    //
    // Exposed because NaturalSpawner needs the first two directly (the start-
    // block conductor test and Mob::CheckSpawnRules' block-below test), and
    // because keeping one definition is the point of moving them here.

    // MC BlockBehaviour.isCollisionShapeFullBlock. NOT the same as "has
    // collision": a slab, a stair or a fence all collide but none is a full
    // cube, and MC lets a mob's body occupy every one of them.
    bool IsCollisionShapeFullBlock(const IBlockAccess& blocks, int x, int y, int z);

    // MC BlockStateBase.isValidSpawn's default: the block's TOP face is sturdy
    // (so a bottom slab counts, a fence post does not). MC also requires
    // getLightEmission() < 14 (no spawning on glowstone/magma); this engine
    // stores no light emission, so that clause is absent until it does.
    bool IsValidSpawnBlock(const IBlockAccess& blocks, int x, int y, int z);

    // MC NaturalSpawner.isValidEmptySpawnBlock — the position itself must be
    // free: not a full collision cube, not a redstone signal source, no fluid,
    // not inside PREVENT_MOB_SPAWNING_INSIDE (rails), and not a block this
    // type finds dangerous (fire/cactus/berry bush/wither rose/powder snow,
    // minus the type's immunities).
    bool IsValidEmptySpawnBlock(EntityTypeId type, const IBlockAccess& blocks,
                                int x, int y, int z);

    // MC BlockBehaviour.isSignalSource for this engine's registry: buttons,
    // pressure plates, levers, redstone torches/wire/block, observers, and the
    // rest of the emitting set. Mobs never spawn inside one.
    bool IsSignalSource(BlockID block);

    // MC EntityType.canSpawnFarFromPlayer — CREATURE and MISC categories, plus
    // the two explicit builder opt-ins (pillager, shulker).
    bool CanSpawnFarFromPlayer(EntityTypeId type);

    // MC EntityType.getSpawnAABB's spawnDimensionsScale — 4.0 for slime and
    // magma cube (they spawn at up to size 4), 1.0 for everything else. The
    // spawn-box collision test must use the scaled box or big slimes spawn
    // embedded in walls.
    float GetSpawnDimensionsScale(EntityTypeId type);

} // namespace Game
