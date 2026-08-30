#include "levelgen/structure/PieceBehaviors.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/Heightmap.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "world/biome/Biome.h"

#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// MineshaftPieces.java - block placement half (B6). Layout lives in
// MineshaftLayout.cpp; these behaviors receive the per-piece fields the
// layout computed (type, orientation, corridor flags, room entrances).

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;
using world::level::block::FenceBlock;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;

constexpr Direction kAllDirections[6] = {
    Direction::DOWN, Direction::UP, Direction::NORTH,
    Direction::SOUTH, Direction::WEST, Direction::EAST};

int stepY(Direction dir) {
    return dir == Direction::DOWN ? -1 : (dir == Direction::UP ? 1 : 0);
}

// Reference: FallingBlock subclasses that can appear underground.
bool isFallingBlock(const BlockState* state) {
    const std::string& name = state->getBlock()->getIdentifier();
    return name == "minecraft:sand" || name == "minecraft:red_sand"
        || name == "minecraft:gravel" || name == "minecraft:suspicious_sand"
        || name == "minecraft:suspicious_gravel";
}

// Reference: MineshaftPieces.MineShaftPiece - shared type states + guards.
class MineShaftBehaviorBase : public OrientedPieceBehavior {
public:
    MineShaftBehaviorBase(int orientation, bool mesa)
        : OrientedPieceBehavior(orientation), m_mesa(mesa) {}

protected:
    bool m_mesa;

    // Reference: MineshaftStructure.Type - NORMAL(oak), MESA(dark_oak).
    BlockState* woodState() const {
        return Blocks::getDefaultState(m_mesa ? "minecraft:dark_oak_log"
                                              : "minecraft:oak_log");
    }
    BlockState* planksState() const {
        return Blocks::getDefaultState(m_mesa ? "minecraft:dark_oak_planks"
                                              : "minecraft:oak_planks");
    }
    BlockState* fenceState() const {
        return Blocks::getDefaultState(m_mesa ? "minecraft:dark_oak_fence"
                                              : "minecraft:oak_fence");
    }
    BlockState* caveAir() const {
        return Blocks::getDefaultState("minecraft:cave_air");
    }

    // Reference: MineShaftPiece.canBeReplaced - protect own planks/wood/
    // fence/chains.
    bool canBeReplaced(WorldGenLevel* level, int x, int y, int z,
                       const BoundingBox& chunkBB) const override {
        BlockState* state = getBlock(level, x, y, z, chunkBB);
        return !state->is(planksState()->getBlock())
            && !state->is(woodState()->getBlock())
            && !state->is(fenceState()->getBlock())
            && !state->is(Blocks::getBlock("minecraft:iron_chain"));
    }

    // Reference: MineShaftPiece.isSupportingBox.
    bool isSupportingBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                         int x0, int x1, int y1, int z0) const {
        for (int x = x0; x <= x1; ++x) {
            if (getBlock(level, x, y1 + 1, z0, chunkBB)->isAir()) return false;
        }
        return true;
    }

    // Reference: MineShaftPiece.isInInvalidLocation - center biome in
    // #minecraft:mineshaft_blocking, then liquid on the clamped shell.
    bool isInInvalidLocation(WorldGenLevel* level, const BoundingBox& chunkBB) const {
        const BoundingBox& box = m_self->boundingBox;
        int x0 = std::max(box.minX - 1, chunkBB.minX);
        int y0 = std::max(box.minY - 1, chunkBB.minY);
        int z0 = std::max(box.minZ - 1, chunkBB.minZ);
        int x1 = std::min(box.maxX + 1, chunkBB.maxX);
        int y1 = std::min(box.maxY + 1, chunkBB.maxY);
        int z1 = std::min(box.maxZ + 1, chunkBB.maxZ);
        core::BlockPos center((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        const world::biome::Biome* biome = level->getBiome(center);
        if (biome != nullptr) {
            static const std::unordered_set<std::string>& blocking =
                BiomeTags::resolve("#minecraft:mineshaft_blocking");
            if (blocking.count(biome->getName())) return true;
        }
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                if (level->getBlockState(core::BlockPos(x, y0, z))->isFluid()) return true;
                if (level->getBlockState(core::BlockPos(x, y1, z))->isFluid()) return true;
            }
        }
        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) {
                if (level->getBlockState(core::BlockPos(x, y, z0))->isFluid()) return true;
                if (level->getBlockState(core::BlockPos(x, y, z1))->isFluid()) return true;
            }
        }
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                if (level->getBlockState(core::BlockPos(x0, y, z))->isFluid()) return true;
                if (level->getBlockState(core::BlockPos(x1, y, z))->isFluid()) return true;
            }
        }
        return false;
    }

    // Reference: MineShaftPiece.setPlanksBlock - only interior positions
    // whose existing block is not up-face-sturdy.
    void setPlanksBlock(WorldGenLevel* level, const BoundingBox& chunkBB,
                        BlockState* planks, int x, int y, int z) const {
        if (!isInterior(level, x, y, z, chunkBB)) return;
        core::BlockPos pos = worldPos(x, y, z);
        BlockState* existing = level->getBlockState(pos);
        if (!existing->isFaceSturdy(*level, pos, Direction::UP)) {
            level->setBlock(pos, planks, 2);
        }
    }
};

// Reference: MineshaftPieces.MineShaftRoom.postProcess.
class MineShaftRoomBehavior final : public MineShaftBehaviorBase {
public:
    MineShaftRoomBehavior(bool mesa, std::vector<BoundingBox> entrances)
        : MineShaftBehaviorBase(-1, mesa), m_entrances(std::move(entrances)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        if (isInInvalidLocation(level, chunkBB)) return;
        const BoundingBox& box = self.boundingBox;
        BlockState* air = caveAir();
        generateBox(level, chunkBB, box.minX, box.minY + 1, box.minZ, box.maxX,
                    std::min(box.minY + 3, box.maxY), box.maxZ, air, air, false);
        for (const BoundingBox& entrance : m_entrances) {
            generateBox(level, chunkBB, entrance.minX, entrance.maxY - 2, entrance.minZ,
                        entrance.maxX, entrance.maxY, entrance.maxZ, air, air, false);
        }
        generateUpperHalfSphere(level, chunkBB, box.minX, box.minY + 4, box.minZ,
                                box.maxX, box.maxY, box.maxZ, air, false);
    }

private:
    std::vector<BoundingBox> m_entrances;
};

// Reference: MineshaftPieces.MineShaftCorridor.postProcess + helpers.
class MineShaftCorridorBehavior final : public MineShaftBehaviorBase {
public:
    MineShaftCorridorBehavior(bool mesa, int orientation, bool hasRails,
                              bool spiderCorridor, int numSections)
        : MineShaftBehaviorBase(orientation, mesa), m_hasRails(hasRails),
          m_spiderCorridor(spiderCorridor), m_numSections(numSections) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        if (isInInvalidLocation(level, chunkBB)) return;
        int length = m_numSections * 5 - 1;
        BlockState* planks = planksState();
        BlockState* air = caveAir();
        generateBox(level, chunkBB, 0, 0, 0, 2, 1, length, air, air, false);
        generateMaybeBox(level, chunkBB, random, 0.8f, 0, 2, 0, 2, 2, length,
                         air, air, false, false);
        if (m_spiderCorridor) {
            generateMaybeBox(level, chunkBB, random, 0.6f, 0, 0, 0, 2, 1, length,
                             Blocks::getDefaultState("minecraft:cobweb"), air,
                             false, true);
        }
        for (int section = 0; section < m_numSections; ++section) {
            int z = 2 + section * 5;
            placeSupport(level, chunkBB, 0, 0, z, 2, 2, random);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z - 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z - 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z + 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z + 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z - 2);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z - 2);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z + 2);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z + 2);
            if (random.nextInt(100) == 0) {
                createCorridorChest(level, chunkBB, random, 2, 0, z - 1);
            }
            if (random.nextInt(100) == 0) {
                createCorridorChest(level, chunkBB, random, 0, 0, z + 1);
            }
            if (m_spiderCorridor && !m_hasPlacedSpider) {
                int newZ = z - 1 + random.nextInt(3);
                core::BlockPos pos = worldPos(1, 0, newZ);
                if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
                    && isInterior(level, 1, 0, newZ, chunkBB)) {
                    m_hasPlacedSpider = true;
                    level->setBlock(pos, Blocks::getDefaultState("minecraft:spawner"), 2);
                    // SpawnerBlockEntity.setEntityId(CAVE_SPIDER, random)
                    // draws nothing (empty spawnPotentials); the saved BE
                    // carries the BaseSpawner defaults + cave_spider (B8).
                    if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                        chunk->setBlockEntityNbt(pos,
                            "{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,"
                            "MinSpawnDelay:200s,RequiredPlayerRange:16s,SpawnCount:4s,"
                            "SpawnData:{entity:{id:\"minecraft:cave_spider\"}},"
                            "SpawnPotentials:[],SpawnRange:4s,components:{},"
                            "id:\"minecraft:mob_spawner\"}");
                    }
                }
            }
        }
        for (int x = 0; x <= 2; ++x) {
            for (int z = 0; z <= length; ++z) {
                setPlanksBlock(level, chunkBB, planks, x, -1, z);
            }
        }
        placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, 2);
        if (m_numSections > 1) {
            placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, length - 2);
        }
        if (m_hasRails) {
            using RS = world::level::block::state::properties::RailShape;
            BlockState* rail = Blocks::getDefaultState("minecraft:rail")
                ->setValue(*BlockStateProperties::RAIL_SHAPE, RS(RS::NORTH_SOUTH));
            for (int z = 0; z <= length; ++z) {
                BlockState* floor = getBlock(level, 1, -1, z, chunkBB);
                if (!floor->isAir() && floor->isSolidRender()) {
                    float probability =
                        isInterior(level, 1, 0, z, chunkBB) ? 0.7f : 0.9f;
                    maybeGenerateBlock(level, chunkBB, random, probability, 1, 0, z, rail);
                }
            }
        }
    }

private:
    bool m_hasRails;
    bool m_spiderCorridor;
    bool m_hasPlacedSpider = false;
    int m_numSections;

    // Reference: MineShaftCorridor.createChest override - rail with random
    // straight shape + chest MINECART entity (skipped; loot nextLong drawn).
    bool createCorridorChest(WorldGenLevel* level, const BoundingBox& chunkBB,
                             WorldgenRandom& random, int x, int y, int z) const {
        core::BlockPos pos = worldPos(x, y, z);
        if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
            && level->getBlockState(pos)->isAir()
            && !level->getBlockState(pos.below())->isAir()) {
            using RS = world::level::block::state::properties::RailShape;
            BlockState* rail = Blocks::getDefaultState("minecraft:rail")
                ->setValue(*BlockStateProperties::RAIL_SHAPE,
                           random.nextBoolean() ? RS(RS::NORTH_SOUTH)
                                                : RS(RS::EAST_WEST));
            placeBlock(level, rail, x, y, z, chunkBB);
            (void)random.nextLong();  // MinecartChest.setLootTable seed
            return true;
        }
        return false;
    }

    // Reference: MineShaftCorridor.placeDoubleLowerOrUpperSupport.
    void placeDoubleLowerOrUpperSupport(WorldGenLevel* level, const BoundingBox& chunkBB,
                                        int x, int y, int z) const {
        if (getBlock(level, x, y, z, chunkBB)->is(planksState()->getBlock())) {
            fillPillarDownOrChainUp(level, woodState(), x, y, z, chunkBB);
        }
        if (getBlock(level, x + 2, y, z, chunkBB)->is(planksState()->getBlock())) {
            fillPillarDownOrChainUp(level, woodState(), x + 2, y, z, chunkBB);
        }
    }

    // Reference: MineShaftCorridor.fillPillarDownOrChainUp.
    void fillPillarDownOrChainUp(WorldGenLevel* level, BlockState* pillarState,
                                 int x, int y, int z, const BoundingBox& chunkBB) const {
        core::BlockPos base = worldPos(x, y, z);
        if (!chunkBB.isInside(base.getX(), base.getY(), base.getZ())) return;
        int worldY = base.getY();
        int distance = 1;
        bool checkBelow = true;
        bool checkAbove = true;
        while (checkBelow || checkAbove) {
            if (checkBelow) {
                core::BlockPos pos(base.getX(), worldY - distance, base.getZ());
                BlockState* belowState = level->getBlockState(pos);
                bool emptyBelow = isReplaceableByStructures(belowState)
                    && !belowState->is(Blocks::getBlock("minecraft:lava"));
                if (!emptyBelow && belowState->isFaceSturdy(*level, pos, Direction::UP)) {
                    fillColumnBetween(level, pillarState, base.getX(), base.getZ(),
                                      worldY - distance + 1, worldY);
                    return;
                }
                checkBelow = distance <= 20 && emptyBelow
                    && pos.getY() > level->getMinY() + 1;
            }
            if (checkAbove) {
                core::BlockPos pos(base.getX(), worldY + distance, base.getZ());
                BlockState* aboveState = level->getBlockState(pos);
                bool emptyAbove = isReplaceableByStructures(aboveState);
                if (!emptyAbove && canHangChainBelow(level, pos, aboveState)) {
                    level->setBlock(core::BlockPos(base.getX(), worldY + 1, base.getZ()),
                                    fenceState(), 2);
                    fillColumnBetween(level,
                                      Blocks::getDefaultState("minecraft:iron_chain"),
                                      base.getX(), base.getZ(), worldY + 2,
                                      worldY + distance);
                    return;
                }
                checkAbove = distance <= 50 && emptyAbove
                    && pos.getY() < level->getMaxY();
            }
            ++distance;
        }
    }

    static void fillColumnBetween(WorldGenLevel* level, BlockState* pillarState,
                                  int x, int z, int bottomInclusive, int topExclusive) {
        for (int y = bottomInclusive; y < topExclusive; ++y) {
            level->setBlock(core::BlockPos(x, y, z), pillarState, 2);
        }
    }

    // Reference: MineShaftCorridor.canHangChainBelow - Block.canSupportCenter
    // uses SupportType.CENTER: post-like blocks (fences/walls/bars/chains)
    // qualify even though their full faces are not sturdy.
    bool canHangChainBelow(WorldGenLevel* level, const core::BlockPos& pos,
                           BlockState* state) const {
        const std::string& id = state->getBlock()->getIdentifier();
        bool centerPost =
            (id.size() > 6 && id.compare(id.size() - 6, 6, "_fence") == 0)
            || (id.size() > 5 && id.compare(id.size() - 5, 5, "_wall") == 0)
            || id == "minecraft:iron_bars" || id == "minecraft:iron_chain"
            || id == "minecraft:chain";
        return (centerPost || state->isFaceSturdy(*level, pos, Direction::DOWN))
            && !isFallingBlock(state);
    }

    // Reference: MineShaftCorridor.placeSupport.
    void placeSupport(WorldGenLevel* level, const BoundingBox& chunkBB,
                      int x0, int y0, int z, int y1, int x1,
                      WorldgenRandom& random) const {
        if (!isSupportingBox(level, chunkBB, x0, x1, y1, z)) return;
        BlockState* planks = planksState();
        BlockState* fence = fenceState();
        BlockState* air = caveAir();
        generateBox(level, chunkBB, x0, y0, z, x0, y1 - 1, z,
                    fence->setValue(*FenceBlock::WEST, true), air, false);
        generateBox(level, chunkBB, x1, y0, z, x1, y1 - 1, z,
                    fence->setValue(*FenceBlock::EAST, true), air, false);
        if (random.nextInt(4) == 0) {
            generateBox(level, chunkBB, x0, y1, z, x0, y1, z, planks, air, false);
            generateBox(level, chunkBB, x1, y1, z, x1, y1, z, planks, air, false);
        } else {
            generateBox(level, chunkBB, x0, y1, z, x1, y1, z, planks, air, false);
            BlockState* torch = Blocks::getDefaultState("minecraft:wall_torch");
            maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z - 1,
                torch->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH));
            maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z + 1,
                torch->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH));
        }
    }

    // Reference: MineShaftCorridor.maybePlaceCobWeb - isInterior gates the
    // nextFloat draw (short-circuit order matters).
    void maybePlaceCobWeb(WorldGenLevel* level, const BoundingBox& chunkBB,
                          WorldgenRandom& random, float probability,
                          int x, int y, int z) const {
        if (isInterior(level, x, y, z, chunkBB) && random.nextFloat() < probability
            && hasSturdyNeighbours(level, chunkBB, x, y, z, 2)) {
            placeBlock(level, Blocks::getDefaultState("minecraft:cobweb"),
                       x, y, z, chunkBB);
        }
    }

    // Reference: MineShaftCorridor.hasSturdyNeighbours.
    bool hasSturdyNeighbours(WorldGenLevel* level, const BoundingBox& chunkBB,
                             int x, int y, int z, int count) const {
        core::BlockPos base = worldPos(x, y, z);
        int sturdy = 0;
        for (Direction direction : kAllDirections) {
            core::BlockPos pos = base.offset(core::getStepX(direction),
                                             stepY(direction),
                                             core::getStepZ(direction));
            if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
                && level->getBlockState(pos)->isFaceSturdy(
                       *level, pos, core::getOpposite(direction))) {
                ++sturdy;
                if (sturdy >= count) return true;
            }
        }
        return false;
    }
};

// Reference: MineshaftPieces.MineShaftCrossing.postProcess (world coords,
// null orientation).
class MineShaftCrossingBehavior final : public MineShaftBehaviorBase {
public:
    MineShaftCrossingBehavior(bool mesa, bool isTwoFloored)
        : MineShaftBehaviorBase(-1, mesa), m_isTwoFloored(isTwoFloored) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        if (isInInvalidLocation(level, chunkBB)) return;
        const BoundingBox& box = self.boundingBox;
        BlockState* planks = planksState();
        BlockState* air = caveAir();
        if (m_isTwoFloored) {
            generateBox(level, chunkBB, box.minX + 1, box.minY, box.minZ,
                        box.maxX - 1, box.minY + 3 - 1, box.maxZ, air, air, false);
            generateBox(level, chunkBB, box.minX, box.minY, box.minZ + 1,
                        box.maxX, box.minY + 3 - 1, box.maxZ - 1, air, air, false);
            generateBox(level, chunkBB, box.minX + 1, box.maxY - 2, box.minZ,
                        box.maxX - 1, box.maxY, box.maxZ, air, air, false);
            generateBox(level, chunkBB, box.minX, box.maxY - 2, box.minZ + 1,
                        box.maxX, box.maxY, box.maxZ - 1, air, air, false);
            generateBox(level, chunkBB, box.minX + 1, box.minY + 3, box.minZ + 1,
                        box.maxX - 1, box.minY + 3, box.maxZ - 1, air, air, false);
        } else {
            generateBox(level, chunkBB, box.minX + 1, box.minY, box.minZ,
                        box.maxX - 1, box.maxY, box.maxZ, air, air, false);
            generateBox(level, chunkBB, box.minX, box.minY, box.minZ + 1,
                        box.maxX, box.maxY, box.maxZ - 1, air, air, false);
        }
        placeSupportPillar(level, chunkBB, box.minX + 1, box.minY, box.minZ + 1, box.maxY);
        placeSupportPillar(level, chunkBB, box.minX + 1, box.minY, box.maxZ - 1, box.maxY);
        placeSupportPillar(level, chunkBB, box.maxX - 1, box.minY, box.minZ + 1, box.maxY);
        placeSupportPillar(level, chunkBB, box.maxX - 1, box.minY, box.maxZ - 1, box.maxY);
        int y = box.minY - 1;
        for (int x = box.minX; x <= box.maxX; ++x) {
            for (int z = box.minZ; z <= box.maxZ; ++z) {
                setPlanksBlock(level, chunkBB, planks, x, y, z);
            }
        }
    }

private:
    bool m_isTwoFloored;

    void placeSupportPillar(WorldGenLevel* level, const BoundingBox& chunkBB,
                            int x, int y0, int z, int y1) const {
        if (!getBlock(level, x, y1 + 1, z, chunkBB)->isAir()) {
            generateBox(level, chunkBB, x, y0, z, x, y1, z, planksState(),
                        caveAir(), false);
        }
    }
};

// Reference: MineshaftPieces.MineShaftStairs.postProcess.
class MineShaftStairsBehavior final : public MineShaftBehaviorBase {
public:
    MineShaftStairsBehavior(bool mesa, int orientation)
        : MineShaftBehaviorBase(orientation, mesa) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        if (isInInvalidLocation(level, chunkBB)) return;
        BlockState* air = caveAir();
        generateBox(level, chunkBB, 0, 5, 0, 2, 7, 1, air, air, false);
        generateBox(level, chunkBB, 0, 0, 7, 2, 2, 8, air, air, false);
        for (int i = 0; i < 5; ++i) {
            generateBox(level, chunkBB, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i,
                        2, 7 - i, 2 + i, air, air, false);
        }
    }
};

} // namespace

std::shared_ptr<StructurePieceBehavior> mineshaftRoom(
    bool mesa, std::vector<BoundingBox> entrances) {
    return std::make_shared<MineShaftRoomBehavior>(mesa, std::move(entrances));
}

std::shared_ptr<StructurePieceBehavior> mineshaftCorridor(
    bool mesa, int orientation, bool hasRails, bool spiderCorridor, int numSections) {
    return std::make_shared<MineShaftCorridorBehavior>(mesa, orientation, hasRails,
                                                       spiderCorridor, numSections);
}

std::shared_ptr<StructurePieceBehavior> mineshaftCrossing(bool mesa, bool isTwoFloored) {
    return std::make_shared<MineShaftCrossingBehavior>(mesa, isTwoFloored);
}

std::shared_ptr<StructurePieceBehavior> mineshaftStairs(bool mesa, int orientation) {
    return std::make_shared<MineShaftStairsBehavior>(mesa, orientation);
}

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
