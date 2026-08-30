#include "levelgen/structure/PieceBehaviors.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "random/LegacyRandomSource.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// NetherFortressPieces.java - block placement half (Part D2). Layout lives in
// NetherFortressLayout.cpp.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;
using world::level::block::FenceBlock;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;

class FortressBehaviorBase : public OrientedPieceBehavior {
public:
    explicit FortressBehaviorBase(int orientation) : OrientedPieceBehavior(orientation) {}

protected:
    static BlockState* bricks() {
        return Blocks::getDefaultState("minecraft:nether_bricks");
    }
    static BlockState* air() {
        return Blocks::AIR->defaultBlockState();
    }
    static BlockState* fence() {
        return Blocks::getDefaultState("minecraft:nether_brick_fence");
    }
    static BlockState* fenceWith(bool north, bool south, bool west, bool east) {
        BlockState* state = fence();
        if (north) state = state->setValue(*FenceBlock::NORTH, true);
        if (south) state = state->setValue(*FenceBlock::SOUTH, true);
        if (west) state = state->setValue(*FenceBlock::WEST, true);
        if (east) state = state->setValue(*FenceBlock::EAST, true);
        return state;
    }
    static BlockState* nsFence() { return fenceWith(true, true, false, false); }
    static BlockState* weFence() { return fenceWith(false, false, true, true); }
    static BlockState* stairsFacing(Direction facing) {
        return Blocks::getDefaultState("minecraft:nether_brick_stairs")
            ->setValue(*BlockStateProperties::HORIZONTAL_FACING, facing);
    }

    void brickBox(WorldGenLevel* level, const BoundingBox& bb,
                  int x0, int y0, int z0, int x1, int y1, int z1) const {
        generateBox(level, bb, x0, y0, z0, x1, y1, z1, bricks(), bricks(), false);
    }
    void airBox(WorldGenLevel* level, const BoundingBox& bb,
                int x0, int y0, int z0, int x1, int y1, int z1) const {
        generateBox(level, bb, x0, y0, z0, x1, y1, z1, air(), air(), false);
    }
    void fenceBox(WorldGenLevel* level, const BoundingBox& bb, BlockState* state,
                  int x0, int y0, int z0, int x1, int y1, int z1) const {
        generateBox(level, bb, x0, y0, z0, x1, y1, z1, state, state, false);
    }
    void columnsDown(WorldGenLevel* level, const BoundingBox& bb,
                     int x0, int x1, int z0, int z1) const {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
            }
        }
    }
};

// Reference: BridgeStraight.postProcess (also used by nothing else).
class FortressBridgeStraight final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 3, 0, 4, 4, 18);
        airBox(level, bb, 1, 5, 0, 3, 7, 18);
        brickBox(level, bb, 0, 5, 0, 0, 5, 18);
        brickBox(level, bb, 4, 5, 0, 4, 5, 18);
        brickBox(level, bb, 0, 2, 0, 4, 2, 5);
        brickBox(level, bb, 0, 2, 13, 4, 2, 18);
        brickBox(level, bb, 0, 0, 0, 4, 1, 3);
        brickBox(level, bb, 0, 0, 15, 4, 1, 18);
        for (int x = 0; x <= 4; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
                fillColumnDown(level, bricks(), x, -1, 18 - z, bb);
            }
        }
        BlockState* nseFence = fenceWith(true, true, false, true);
        BlockState* nswFence = fenceWith(true, true, true, false);
        fenceBox(level, bb, nseFence, 0, 1, 1, 0, 4, 1);
        fenceBox(level, bb, nseFence, 0, 3, 4, 0, 4, 4);
        fenceBox(level, bb, nseFence, 0, 3, 14, 0, 4, 14);
        fenceBox(level, bb, nseFence, 0, 1, 17, 0, 4, 17);
        fenceBox(level, bb, nswFence, 4, 1, 1, 4, 4, 1);
        fenceBox(level, bb, nswFence, 4, 3, 4, 4, 4, 4);
        fenceBox(level, bb, nswFence, 4, 3, 14, 4, 4, 14);
        fenceBox(level, bb, nswFence, 4, 1, 17, 4, 4, 17);
    }
};

// Reference: BridgeEndFiller.postProcess - its own RandomSource from selfSeed.
class FortressBridgeEndFiller final : public FortressBehaviorBase {
public:
    FortressBridgeEndFiller(int orientation, int selfSeed)
        : FortressBehaviorBase(orientation), m_selfSeed(selfSeed) {}
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        LegacyRandomSource selfRandom(static_cast<int64_t>(m_selfSeed));
        for (int x = 0; x <= 4; ++x) {
            for (int y = 3; y <= 4; ++y) {
                int z = selfRandom.nextInt(8);
                brickBox(level, bb, x, y, 0, x, y, z);
            }
        }
        int z = selfRandom.nextInt(8);
        brickBox(level, bb, 0, 5, 0, 0, 5, z);
        z = selfRandom.nextInt(8);
        brickBox(level, bb, 4, 5, 0, 4, 5, z);
        for (int x = 0; x <= 4; ++x) {
            int zz = selfRandom.nextInt(5);
            brickBox(level, bb, x, 2, 0, x, 2, zz);
        }
        for (int x = 0; x <= 4; ++x) {
            for (int y = 0; y <= 1; ++y) {
                int zz = selfRandom.nextInt(3);
                brickBox(level, bb, x, y, 0, x, y, zz);
            }
        }
    }
private:
    int m_selfSeed;
};

// Reference: BridgeCrossing.postProcess (StartPiece shares it).
class FortressBridgeCrossing final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 7, 3, 0, 11, 4, 18);
        brickBox(level, bb, 0, 3, 7, 18, 4, 11);
        airBox(level, bb, 8, 5, 0, 10, 7, 18);
        airBox(level, bb, 0, 5, 8, 18, 7, 10);
        brickBox(level, bb, 7, 5, 0, 7, 5, 7);
        brickBox(level, bb, 7, 5, 11, 7, 5, 18);
        brickBox(level, bb, 11, 5, 0, 11, 5, 7);
        brickBox(level, bb, 11, 5, 11, 11, 5, 18);
        brickBox(level, bb, 0, 5, 7, 7, 5, 7);
        brickBox(level, bb, 11, 5, 7, 18, 5, 7);
        brickBox(level, bb, 0, 5, 11, 7, 5, 11);
        brickBox(level, bb, 11, 5, 11, 18, 5, 11);
        brickBox(level, bb, 7, 2, 0, 11, 2, 5);
        brickBox(level, bb, 7, 2, 13, 11, 2, 18);
        brickBox(level, bb, 7, 0, 0, 11, 1, 3);
        brickBox(level, bb, 7, 0, 15, 11, 1, 18);
        for (int x = 7; x <= 11; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
                fillColumnDown(level, bricks(), x, -1, 18 - z, bb);
            }
        }
        brickBox(level, bb, 0, 2, 7, 5, 2, 11);
        brickBox(level, bb, 13, 2, 7, 18, 2, 11);
        brickBox(level, bb, 0, 0, 7, 3, 1, 11);
        brickBox(level, bb, 15, 0, 7, 18, 1, 11);
        for (int x = 0; x <= 2; ++x) {
            for (int z = 7; z <= 11; ++z) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
                fillColumnDown(level, bricks(), 18 - x, -1, z, bb);
            }
        }
    }
};

// Reference: RoomCrossing.postProcess.
class FortressRoomCrossing final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 6, 1, 6);
        airBox(level, bb, 0, 2, 0, 6, 7, 6);
        brickBox(level, bb, 0, 2, 0, 1, 6, 0);
        brickBox(level, bb, 0, 2, 6, 1, 6, 6);
        brickBox(level, bb, 5, 2, 0, 6, 6, 0);
        brickBox(level, bb, 5, 2, 6, 6, 6, 6);
        brickBox(level, bb, 0, 2, 0, 0, 6, 1);
        brickBox(level, bb, 0, 2, 5, 0, 6, 6);
        brickBox(level, bb, 6, 2, 0, 6, 6, 1);
        brickBox(level, bb, 6, 2, 5, 6, 6, 6);
        brickBox(level, bb, 2, 6, 0, 4, 6, 0);
        fenceBox(level, bb, weFence(), 2, 5, 0, 4, 5, 0);
        brickBox(level, bb, 2, 6, 6, 4, 6, 6);
        fenceBox(level, bb, weFence(), 2, 5, 6, 4, 5, 6);
        brickBox(level, bb, 0, 6, 2, 0, 6, 4);
        fenceBox(level, bb, nsFence(), 0, 5, 2, 0, 5, 4);
        brickBox(level, bb, 6, 6, 2, 6, 6, 4);
        fenceBox(level, bb, nsFence(), 6, 5, 2, 6, 5, 4);
        columnsDown(level, bb, 0, 6, 0, 6);
    }
};

// Reference: StairsRoom.postProcess.
class FortressStairsRoom final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 6, 1, 6);
        airBox(level, bb, 0, 2, 0, 6, 10, 6);
        brickBox(level, bb, 0, 2, 0, 1, 8, 0);
        brickBox(level, bb, 5, 2, 0, 6, 8, 0);
        brickBox(level, bb, 0, 2, 1, 0, 8, 6);
        brickBox(level, bb, 6, 2, 1, 6, 8, 6);
        brickBox(level, bb, 1, 2, 6, 5, 8, 6);
        fenceBox(level, bb, nsFence(), 0, 3, 2, 0, 5, 4);
        fenceBox(level, bb, nsFence(), 6, 3, 2, 6, 5, 2);
        fenceBox(level, bb, nsFence(), 6, 3, 4, 6, 5, 4);
        placeBlock(level, bricks(), 5, 2, 5, bb);
        brickBox(level, bb, 4, 2, 5, 4, 3, 5);
        brickBox(level, bb, 3, 2, 5, 3, 4, 5);
        brickBox(level, bb, 2, 2, 5, 2, 5, 5);
        brickBox(level, bb, 1, 2, 5, 1, 6, 5);
        brickBox(level, bb, 1, 7, 1, 5, 7, 4);
        airBox(level, bb, 6, 8, 2, 6, 8, 4);
        brickBox(level, bb, 2, 6, 0, 4, 8, 0);
        fenceBox(level, bb, weFence(), 2, 5, 0, 4, 5, 0);
        columnsDown(level, bb, 0, 6, 0, 6);
    }
};

// Reference: MonsterThrone.postProcess - blaze spawner, hasPlacedSpawner state.
class FortressMonsterThrone final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        airBox(level, bb, 0, 2, 0, 6, 7, 7);
        brickBox(level, bb, 1, 0, 0, 5, 1, 7);
        brickBox(level, bb, 1, 2, 1, 5, 2, 7);
        brickBox(level, bb, 1, 3, 2, 5, 3, 7);
        brickBox(level, bb, 1, 4, 3, 5, 4, 7);
        brickBox(level, bb, 1, 2, 0, 1, 4, 2);
        brickBox(level, bb, 5, 2, 0, 5, 4, 2);
        brickBox(level, bb, 1, 5, 2, 1, 5, 3);
        brickBox(level, bb, 5, 5, 2, 5, 5, 3);
        brickBox(level, bb, 0, 5, 3, 0, 5, 8);
        brickBox(level, bb, 6, 5, 3, 6, 5, 8);
        brickBox(level, bb, 1, 5, 8, 5, 5, 8);
        placeBlock(level, fenceWith(false, false, true, false), 1, 6, 3, bb);
        placeBlock(level, fenceWith(false, false, false, true), 5, 6, 3, bb);
        placeBlock(level, fenceWith(true, false, false, true), 0, 6, 3, bb);
        placeBlock(level, fenceWith(true, false, true, false), 6, 6, 3, bb);
        fenceBox(level, bb, nsFence(), 0, 6, 4, 0, 6, 7);
        fenceBox(level, bb, nsFence(), 6, 6, 4, 6, 6, 7);
        placeBlock(level, fenceWith(false, true, false, true), 0, 6, 8, bb);
        placeBlock(level, fenceWith(false, true, true, false), 6, 6, 8, bb);
        fenceBox(level, bb, weFence(), 1, 6, 8, 5, 6, 8);
        placeBlock(level, fenceWith(false, false, false, true), 1, 7, 8, bb);
        fenceBox(level, bb, weFence(), 2, 7, 8, 4, 7, 8);
        placeBlock(level, fenceWith(false, false, true, false), 5, 7, 8, bb);
        placeBlock(level, fenceWith(false, false, false, true), 2, 8, 8, bb);
        placeBlock(level, weFence(), 3, 8, 8, bb);
        placeBlock(level, fenceWith(false, false, true, false), 4, 8, 8, bb);
        if (!m_hasPlacedSpawner) {
            core::BlockPos pos = worldPos(3, 5, 5);
            if (bb.isInside(pos.getX(), pos.getY(), pos.getZ())) {
                m_hasPlacedSpawner = true;
                level->setBlock(pos, Blocks::getDefaultState("minecraft:spawner"), 2);
                // SpawnerBlockEntity.setEntityId(BLAZE, random) draws nothing.
                if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                    chunk->setBlockEntityNbt(pos,
                        "{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,"
                        "MinSpawnDelay:200s,RequiredPlayerRange:16s,SpawnCount:4s,"
                        "SpawnData:{entity:{id:\"minecraft:blaze\"}},"
                        "SpawnPotentials:[],SpawnRange:4s,components:{},"
                        "id:\"minecraft:mob_spawner\"}");
                }
            }
        }
        columnsDown(level, bb, 0, 6, 0, 6);
    }
private:
    bool m_hasPlacedSpawner = false;
};

// Reference: CastleEntrance.postProcess.
class FortressCastleEntrance : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        castleBigRoomShell(level, bb);
        fenceBox(level, bb, fence(), 5, 8, 0, 7, 8, 0);
        railings(level, bb);
        for (int z = 3; z <= 9; z += 2) {
            fenceBox(level, bb, fenceWith(true, true, true, false), 1, 7, z, 1, 8, z);
            fenceBox(level, bb, fenceWith(true, true, false, true), 11, 7, z, 11, 8, z);
        }
        castleBigRoomFloor(level, bb);
        brickBox(level, bb, 5, 5, 5, 7, 5, 7);
        airBox(level, bb, 6, 1, 6, 6, 4, 6);
        placeBlock(level, bricks(), 6, 0, 6, bb);
        placeBlock(level, Blocks::getDefaultState("minecraft:lava"), 6, 5, 6, bb);
        // scheduleTick(pos, LAVA, 0) is dump-invisible at generation phases.
    }

protected:
    // Shared shell (identical prefix of CastleEntrance and CastleStalkRoom).
    void castleBigRoomShell(WorldGenLevel* level, const BoundingBox& bb) const {
        brickBox(level, bb, 0, 3, 0, 12, 4, 12);
        airBox(level, bb, 0, 5, 0, 12, 13, 12);
        brickBox(level, bb, 0, 5, 0, 1, 12, 12);
        brickBox(level, bb, 11, 5, 0, 12, 12, 12);
        brickBox(level, bb, 2, 5, 11, 4, 12, 12);
        brickBox(level, bb, 8, 5, 11, 10, 12, 12);
        brickBox(level, bb, 5, 9, 11, 7, 12, 12);
        brickBox(level, bb, 2, 5, 0, 4, 12, 1);
        brickBox(level, bb, 8, 5, 0, 10, 12, 1);
        brickBox(level, bb, 5, 9, 0, 7, 12, 1);
        brickBox(level, bb, 2, 11, 2, 10, 12, 10);
    }

    // Shared roof railings (the i = 1..11 step-2 loop + 4 corners).
    void railings(WorldGenLevel* level, const BoundingBox& bb) const {
        for (int i = 1; i <= 11; i += 2) {
            fenceBox(level, bb, weFence(), i, 10, 0, i, 11, 0);
            fenceBox(level, bb, weFence(), i, 10, 12, i, 11, 12);
            fenceBox(level, bb, nsFence(), 0, 10, i, 0, 11, i);
            fenceBox(level, bb, nsFence(), 12, 10, i, 12, 11, i);
            placeBlock(level, bricks(), i, 13, 0, bb);
            placeBlock(level, bricks(), i, 13, 12, bb);
            placeBlock(level, bricks(), 0, 13, i, bb);
            placeBlock(level, bricks(), 12, 13, i, bb);
            if (i != 11) {
                placeBlock(level, weFence(), i + 1, 13, 0, bb);
                placeBlock(level, weFence(), i + 1, 13, 12, bb);
                placeBlock(level, nsFence(), 0, 13, i + 1, bb);
                placeBlock(level, nsFence(), 12, 13, i + 1, bb);
            }
        }
        placeBlock(level, fenceWith(true, false, false, true), 0, 13, 0, bb);
        placeBlock(level, fenceWith(false, true, false, true), 0, 13, 12, bb);
        placeBlock(level, fenceWith(false, true, true, false), 12, 13, 12, bb);
        placeBlock(level, fenceWith(true, false, true, false), 12, 13, 0, bb);
    }

    // Shared floor cross + column fills (tail of both castle big rooms).
    void castleBigRoomFloor(WorldGenLevel* level, const BoundingBox& bb) const {
        brickBox(level, bb, 4, 2, 0, 8, 2, 12);
        brickBox(level, bb, 0, 2, 4, 12, 2, 8);
        brickBox(level, bb, 4, 0, 0, 8, 1, 3);
        brickBox(level, bb, 4, 0, 9, 8, 1, 12);
        brickBox(level, bb, 0, 0, 4, 3, 1, 8);
        brickBox(level, bb, 9, 0, 4, 12, 1, 8);
        for (int x = 4; x <= 8; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
                fillColumnDown(level, bricks(), x, -1, 12 - z, bb);
            }
        }
        for (int x = 0; x <= 2; ++x) {
            for (int z = 4; z <= 8; ++z) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
                fillColumnDown(level, bricks(), 12 - x, -1, z, bb);
            }
        }
    }
};

// Reference: CastleStalkRoom.postProcess.
class FortressCastleStalkRoom final : public FortressCastleEntrance {
public:
    using FortressCastleEntrance::FortressCastleEntrance;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        castleBigRoomShell(level, bb);
        BlockState* nswFence = fenceWith(true, true, true, false);
        BlockState* nseFence = fenceWith(true, true, false, true);
        railings(level, bb);
        for (int z = 3; z <= 9; z += 2) {
            fenceBox(level, bb, nswFence, 1, 7, z, 1, 8, z);
            fenceBox(level, bb, nseFence, 11, 7, z, 11, 8, z);
        }
        BlockState* northStairs = stairsFacing(Direction::NORTH);
        for (int i = 0; i <= 6; ++i) {
            int z = i + 4;
            for (int x = 5; x <= 7; ++x) {
                placeBlock(level, northStairs, x, 5 + i, z, bb);
            }
            if (z >= 5 && z <= 8) {
                brickBox(level, bb, 5, 5, z, 7, i + 4, z);
            } else if (z >= 9 && z <= 10) {
                brickBox(level, bb, 5, 8, z, 7, i + 4, z);
            }
            if (i >= 1) {
                airBox(level, bb, 5, 6 + i, z, 7, 9 + i, z);
            }
        }
        for (int x = 5; x <= 7; ++x) {
            placeBlock(level, northStairs, x, 12, 11, bb);
        }
        fenceBox(level, bb, nseFence, 5, 6, 7, 5, 7, 7);
        fenceBox(level, bb, nswFence, 7, 6, 7, 7, 7, 7);
        airBox(level, bb, 5, 13, 12, 7, 13, 12);
        brickBox(level, bb, 2, 5, 2, 3, 5, 3);
        brickBox(level, bb, 2, 5, 9, 3, 5, 10);
        brickBox(level, bb, 2, 5, 4, 2, 5, 8);
        brickBox(level, bb, 9, 5, 2, 10, 5, 3);
        brickBox(level, bb, 9, 5, 9, 10, 5, 10);
        brickBox(level, bb, 10, 5, 4, 10, 5, 8);
        BlockState* eastStairs = stairsFacing(Direction::EAST);
        BlockState* westStairs = stairsFacing(Direction::WEST);
        placeBlock(level, westStairs, 4, 5, 2, bb);
        placeBlock(level, westStairs, 4, 5, 3, bb);
        placeBlock(level, westStairs, 4, 5, 9, bb);
        placeBlock(level, westStairs, 4, 5, 10, bb);
        placeBlock(level, eastStairs, 8, 5, 2, bb);
        placeBlock(level, eastStairs, 8, 5, 3, bb);
        placeBlock(level, eastStairs, 8, 5, 9, bb);
        placeBlock(level, eastStairs, 8, 5, 10, bb);
        BlockState* soulSand = Blocks::getDefaultState("minecraft:soul_sand");
        BlockState* wart = Blocks::getDefaultState("minecraft:nether_wart");
        generateBox(level, bb, 3, 4, 4, 4, 4, 8, soulSand, soulSand, false);
        generateBox(level, bb, 8, 4, 4, 9, 4, 8, soulSand, soulSand, false);
        generateBox(level, bb, 3, 5, 4, 4, 5, 8, wart, wart, false);
        generateBox(level, bb, 8, 5, 4, 9, 5, 8, wart, wart, false);
        castleBigRoomFloor(level, bb);
    }
};

// Reference: CastleSmallCorridorPiece.postProcess.
class FortressSmallCorridor final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 4, 1, 4);
        airBox(level, bb, 0, 2, 0, 4, 5, 4);
        brickBox(level, bb, 0, 2, 0, 0, 5, 4);
        brickBox(level, bb, 4, 2, 0, 4, 5, 4);
        fenceBox(level, bb, nsFence(), 0, 3, 1, 0, 4, 1);
        fenceBox(level, bb, nsFence(), 0, 3, 3, 0, 4, 3);
        fenceBox(level, bb, nsFence(), 4, 3, 1, 4, 4, 1);
        fenceBox(level, bb, nsFence(), 4, 3, 3, 4, 4, 3);
        brickBox(level, bb, 0, 6, 0, 4, 6, 4);
        columnsDown(level, bb, 0, 4, 0, 4);
    }
};

// Reference: CastleSmallCorridorCrossingPiece.postProcess.
class FortressSmallCorridorCrossing final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 4, 1, 4);
        airBox(level, bb, 0, 2, 0, 4, 5, 4);
        brickBox(level, bb, 0, 2, 0, 0, 5, 0);
        brickBox(level, bb, 4, 2, 0, 4, 5, 0);
        brickBox(level, bb, 0, 2, 4, 0, 5, 4);
        brickBox(level, bb, 4, 2, 4, 4, 5, 4);
        brickBox(level, bb, 0, 6, 0, 4, 6, 4);
        columnsDown(level, bb, 0, 4, 0, 4);
    }
};

// Reference: CastleSmallCorridorRightTurnPiece.postProcess.
class FortressSmallCorridorRightTurn final : public FortressBehaviorBase {
public:
    FortressSmallCorridorRightTurn(int orientation, bool needsChest)
        : FortressBehaviorBase(orientation), m_isNeedingChest(needsChest) {}
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom& random,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 4, 1, 4);
        airBox(level, bb, 0, 2, 0, 4, 5, 4);
        brickBox(level, bb, 0, 2, 0, 0, 5, 4);
        fenceBox(level, bb, nsFence(), 0, 3, 1, 0, 4, 1);
        fenceBox(level, bb, nsFence(), 0, 3, 3, 0, 4, 3);
        brickBox(level, bb, 4, 2, 0, 4, 5, 0);
        brickBox(level, bb, 1, 2, 4, 4, 5, 4);
        fenceBox(level, bb, weFence(), 1, 3, 4, 1, 4, 4);
        fenceBox(level, bb, weFence(), 3, 3, 4, 3, 4, 4);
        if (m_isNeedingChest) {
            core::BlockPos pos = worldPos(1, 2, 3);
            if (bb.isInside(pos.getX(), pos.getY(), pos.getZ())) {
                m_isNeedingChest = false;
                createChest(level, bb, random, 1, 2, 3,
                            "minecraft:chests/nether_bridge");
            }
        }
        brickBox(level, bb, 0, 6, 0, 4, 6, 4);
        columnsDown(level, bb, 0, 4, 0, 4);
    }
private:
    bool m_isNeedingChest;
};

// Reference: CastleSmallCorridorLeftTurnPiece.postProcess.
class FortressSmallCorridorLeftTurn final : public FortressBehaviorBase {
public:
    FortressSmallCorridorLeftTurn(int orientation, bool needsChest)
        : FortressBehaviorBase(orientation), m_isNeedingChest(needsChest) {}
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom& random,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 4, 1, 4);
        airBox(level, bb, 0, 2, 0, 4, 5, 4);
        brickBox(level, bb, 4, 2, 0, 4, 5, 4);
        fenceBox(level, bb, nsFence(), 4, 3, 1, 4, 4, 1);
        fenceBox(level, bb, nsFence(), 4, 3, 3, 4, 4, 3);
        brickBox(level, bb, 0, 2, 0, 0, 5, 0);
        brickBox(level, bb, 0, 2, 4, 3, 5, 4);
        fenceBox(level, bb, weFence(), 1, 3, 4, 1, 4, 4);
        fenceBox(level, bb, weFence(), 3, 3, 4, 3, 4, 4);
        if (m_isNeedingChest) {
            core::BlockPos pos = worldPos(3, 2, 3);
            if (bb.isInside(pos.getX(), pos.getY(), pos.getZ())) {
                m_isNeedingChest = false;
                createChest(level, bb, random, 3, 2, 3,
                            "minecraft:chests/nether_bridge");
            }
        }
        brickBox(level, bb, 0, 6, 0, 4, 6, 4);
        columnsDown(level, bb, 0, 4, 0, 4);
    }
private:
    bool m_isNeedingChest;
};

// Reference: CastleCorridorStairsPiece.postProcess.
class FortressCorridorStairs final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        BlockState* southStairs = stairsFacing(Direction::SOUTH);
        for (int step = 0; step <= 9; ++step) {
            int floor = std::max(1, 7 - step);
            int roof = std::min(std::max(floor + 5, 14 - step), 13);
            brickBox(level, bb, 0, 0, step, 4, floor, step);
            airBox(level, bb, 1, floor + 1, step, 3, roof - 1, step);
            if (step <= 6) {
                placeBlock(level, southStairs, 1, floor + 1, step, bb);
                placeBlock(level, southStairs, 2, floor + 1, step, bb);
                placeBlock(level, southStairs, 3, floor + 1, step, bb);
            }
            brickBox(level, bb, 0, roof, step, 4, roof, step);
            brickBox(level, bb, 0, floor + 1, step, 0, roof - 1, step);
            brickBox(level, bb, 4, floor + 1, step, 4, roof - 1, step);
            if ((step & 1) == 0) {
                fenceBox(level, bb, nsFence(), 0, floor + 2, step, 0, floor + 3, step);
                fenceBox(level, bb, nsFence(), 4, floor + 2, step, 4, floor + 3, step);
            }
            for (int x = 0; x <= 4; ++x) {
                fillColumnDown(level, bricks(), x, -1, step, bb);
            }
        }
    }
};

// Reference: CastleCorridorTBalconyPiece.postProcess.
class FortressCorridorTBalcony final : public FortressBehaviorBase {
public:
    using FortressBehaviorBase::FortressBehaviorBase;
    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom&,
                     const BoundingBox& bb, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData& self) override {
        bind(self);
        brickBox(level, bb, 0, 0, 0, 8, 1, 8);
        airBox(level, bb, 0, 2, 0, 8, 5, 8);
        brickBox(level, bb, 0, 6, 0, 8, 6, 5);
        brickBox(level, bb, 0, 2, 0, 2, 5, 0);
        brickBox(level, bb, 6, 2, 0, 8, 5, 0);
        fenceBox(level, bb, weFence(), 1, 3, 0, 1, 4, 0);
        fenceBox(level, bb, weFence(), 7, 3, 0, 7, 4, 0);
        brickBox(level, bb, 0, 2, 4, 8, 2, 8);
        airBox(level, bb, 1, 1, 4, 2, 2, 4);
        airBox(level, bb, 6, 1, 4, 7, 2, 4);
        fenceBox(level, bb, weFence(), 1, 3, 8, 7, 3, 8);
        placeBlock(level, fenceWith(false, true, false, true), 0, 3, 8, bb);
        placeBlock(level, fenceWith(false, true, true, false), 8, 3, 8, bb);
        fenceBox(level, bb, nsFence(), 0, 3, 6, 0, 3, 7);
        fenceBox(level, bb, nsFence(), 8, 3, 6, 8, 3, 7);
        brickBox(level, bb, 0, 3, 4, 0, 5, 5);
        brickBox(level, bb, 8, 3, 4, 8, 5, 5);
        brickBox(level, bb, 1, 3, 5, 2, 5, 5);
        brickBox(level, bb, 6, 3, 5, 7, 5, 5);
        fenceBox(level, bb, weFence(), 1, 4, 5, 1, 5, 5);
        fenceBox(level, bb, weFence(), 7, 4, 5, 7, 5, 5);
        for (int z = 0; z <= 5; ++z) {
            for (int x = 0; x <= 8; ++x) {
                fillColumnDown(level, bricks(), x, -1, z, bb);
            }
        }
    }
};

}  // namespace

std::shared_ptr<StructurePieceBehavior> fortressPiece(
    const std::string& pieceTypeId, int orientation, int selfSeed, bool needsChest) {
    if (pieceTypeId == "minecraft:nebs") {
        return std::make_shared<FortressBridgeStraight>(orientation);
    }
    if (pieceTypeId == "minecraft:nebcr") {
        return std::make_shared<FortressBridgeCrossing>(orientation);
    }
    if (pieceTypeId == "minecraft:nebef") {
        return std::make_shared<FortressBridgeEndFiller>(orientation, selfSeed);
    }
    if (pieceTypeId == "minecraft:nerc") {
        return std::make_shared<FortressRoomCrossing>(orientation);
    }
    if (pieceTypeId == "minecraft:nesr") {
        return std::make_shared<FortressStairsRoom>(orientation);
    }
    if (pieceTypeId == "minecraft:nemt") {
        return std::make_shared<FortressMonsterThrone>(orientation);
    }
    if (pieceTypeId == "minecraft:nece") {
        return std::make_shared<FortressCastleEntrance>(orientation);
    }
    if (pieceTypeId == "minecraft:necsr") {
        return std::make_shared<FortressCastleStalkRoom>(orientation);
    }
    if (pieceTypeId == "minecraft:nesc") {
        return std::make_shared<FortressSmallCorridor>(orientation);
    }
    if (pieceTypeId == "minecraft:nescsc") {
        return std::make_shared<FortressSmallCorridorCrossing>(orientation);
    }
    if (pieceTypeId == "minecraft:nescrt") {
        return std::make_shared<FortressSmallCorridorRightTurn>(orientation, needsChest);
    }
    if (pieceTypeId == "minecraft:nesclt") {
        return std::make_shared<FortressSmallCorridorLeftTurn>(orientation, needsChest);
    }
    if (pieceTypeId == "minecraft:neccs") {
        return std::make_shared<FortressCorridorStairs>(orientation);
    }
    if (pieceTypeId == "minecraft:nectb") {
        return std::make_shared<FortressCorridorTBalcony>(orientation);
    }
    return nullptr;
}

}  // namespace PieceBehaviors
}  // namespace structure
}  // namespace levelgen
}  // namespace minecraft
