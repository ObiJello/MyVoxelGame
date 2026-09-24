#pragma once

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/structure/StructureStartData.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <cstdint>
#include <functional>
#include <string>

// Twilight Forest 4.9 — the placement helpers of the mod's legacy structure
// pieces, ported on top of OrientedPieceBehavior:
//   world/components/structures/TFStructureComponent.java     (placeBlock)
//   world/components/structures/TFStructureComponentOld.java  (setOrientation,
//       getWorldX/getWorldZ, setSpawner(InWorld), placeTreasure*, fillWithBlocks)
//   loot/TFLootTables.java                                     (generateChest,
//       generateChestContents)
//   util/BoundingBoxUtils.java                                 (getComponentToAdd-
//       BoundingBox, getIntersectionOfSBBs)
//
// TFStructureComponentOld differs from vanilla StructurePiece in two ways that
// matter for placement: setOrientation never mirrors (SOUTH = CLOCKWISE_180,
// WEST = COUNTERCLOCKWISE_90, EAST = CLOCKWISE_90), and getWorldX/getWorldZ
// add a NORTH case (maxX - x) and flip EAST's z (maxZ - x). The vanilla
// helpers of OrientedPieceBehavior use the vanilla mapping, so this class
// re-declares every coordinate-taking helper it needs with the TF mapping;
// derived pieces must only call the ones declared here.
//
// Used by the hollow hills and the hedge maze (TwilightHollowHill.cpp,
// TwilightHedgeMaze.cpp). The hollow tree pieces extend vanilla StructurePiece
// in the mod and use OrientedPieceBehavior directly; they share only the free
// helpers below (spawners, loot containers).

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
namespace structure {
namespace twilight_pieces {

// ============================================================================
// Free helpers shared by every Twilight piece in this directory.
// ============================================================================
namespace tf_common {

/** Java Rotation enum constant name for a Rotation ordinal (NONE, CLOCKWISE_90, ...). */
const char* rotationName(int rotationOrdinal);

/**
 * The engine entity id a spawner written for `entityId` ("twilightforest:x"
 * or "minecraft:x") carries. Mobs the engine registers keep their own slug
 * under the engine's "minecraft:" namespace; anything else maps to the
 * nearest vanilla mob (see the table in TwilightPieceBase.cpp).
 */
std::string spawnerEntityId(const std::string& entityId);

/**
 * TFStructureComponentOld.setSpawnerInWorld: inside `chunkBB`, place a
 * spawner (unless one is already there) and set its entity —
 * SpawnerBlockEntity.setEntityId draws nothing (empty spawn potentials); the
 * saved block entity is the BaseSpawner defaults + SpawnData for the mob.
 */
void setSpawnerInWorld(WorldGenLevel* level, const BoundingBox& chunkBB,
                       const std::string& entityId, const core::BlockPos& pos);

/**
 * Write a spawner block entity (BaseSpawner defaults + SpawnData) at `pos`;
 * the caller has placed the spawner block.
 */
void writeSpawnerData(WorldGenLevel* level, const core::BlockPos& pos, const std::string& entityId);

/**
 * RandomizableContainerBlockEntity.setLootTable(lootTable, seed): the saved
 * {LootTable, LootTableSeed, components, id} payload for the container at
 * `pos`. `blockEntityId` is the container's BE id ("minecraft:chest", ...).
 */
void writeLootTable(WorldGenLevel* level, const core::BlockPos& pos, const std::string& blockEntityId,
                    const std::string& lootTable, int64_t seed);

/**
 * The block-entity id of a RandomizableContainerBlockEntity block, or "" when
 * `state` is not a lootable container (then setLootTable never runs).
 */
std::string lootContainerBlockEntityId(const BlockState* state);

/**
 * TFLootTables.generateChestContents(level, pos, table) seed:
 * level.getSeed() * pos.getX() + pos.getY() ^ pos.getZ() (Java precedence:
 * ((seed * x) + y) ^ z, long arithmetic).
 */
int64_t chestContentsSeed(int64_t worldSeed, const core::BlockPos& pos);

/**
 * TFLootTables.generateChest(world, pos, dir, trapped, table): a (trapped)
 * chest facing `facing`, with the table and the position-derived seed. No
 * random draw.
 */
void generateChest(WorldGenLevel* level, const core::BlockPos& pos, core::Direction facing,
                   bool trapped, const std::string& lootTable);

/**
 * BoundingBoxUtils.getComponentToAddBoundingBox(x, y, z, minX, minY, minZ,
 * spanX, spanY, spanZ, dir, centerBounds). `dir` is a core::Direction value
 * or -1 for null.
 */
BoundingBox getComponentToAddBoundingBox(int x, int y, int z, int minX, int minY, int minZ,
                                         int spanX, int spanY, int spanZ, int dir, bool centerBounds);

/** BoundingBoxUtils.getIntersectionOfSBBs; false when the boxes do not intersect. */
bool intersectionOf(const BoundingBox& a, const BoundingBox& b, BoundingBox& out);

/** Java Math.round(float): the closest int, ties toward positive infinity. */
int32_t javaRound(float value);

/** Java Math.round(double) narrowed to int (all callers stay in int range). */
int32_t javaRound(double value);

} // namespace tf_common

// ============================================================================
// TFStructureComponentOld — the legacy TF piece base.
// ============================================================================
class TFStructureComponentOld : public OrientedPieceBehavior {
public:
    /** orientation: -1 = null, else a core::Direction value (TF setOrientation). */
    explicit TFStructureComponentOld(int orientation);

    /**
     * TFStructureComponentOld.setOrientation: no mirror; SOUTH = CLOCKWISE_180,
     * WEST = COUNTERCLOCKWISE_90, EAST = CLOCKWISE_90, else NONE.
     */
    void setOrientation(int orientation);

    int orientation() const { return m_orientation; }
    /** The Rotation ordinal setOrientation produced. */
    int rotation() const { return m_rotation; }

    // Reference: TFStructureComponentOld.getWorldX / getWorldZ (TF mapping),
    // StructurePiece.getWorldY.
    int worldX(int x, int z) const;
    int worldY(int y) const;
    int worldZ(int x, int z) const;
    core::BlockPos worldPos(int x, int y, int z) const {
        return core::BlockPos(worldX(x, z), worldY(y), worldZ(x, z));
    }
    /** TFStructureComponentOld.getBlockPosWithOffset. */
    core::BlockPos getBlockPosWithOffset(int x, int y, int z) const { return worldPos(x, y, z); }

    /**
     * TFStructureComponent.placeBlock: inside `chunkBB`, apply the piece's
     * rotation, set with UPDATE_CLIENTS and schedule the fluid tick. Unlike
     * StructurePiece.placeBlock there is no canBeReplaced test. A null state
     * is ignored (the mod logs and would fail on it).
     */
    void placeBlock(WorldGenLevel* level, BlockState* state, int x, int y, int z,
                    const BoundingBox& chunkBB) const;

    /** StructurePiece.getBlock — AIR outside `chunkBB`. */
    BlockState* getBlock(WorldGenLevel* level, int x, int y, int z, const BoundingBox& chunkBB) const;

    /** StructurePiece.generateBox (edge/fill), through the TF placeBlock. */
    void generateBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                     int x0, int y0, int z0, int x1, int y1, int z1,
                     BlockState* edgeBlock, BlockState* fillBlock, bool skipAir) const;

    /** StructurePiece.generateAirBox, through the TF placeBlock. */
    void generateAirBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                        int x0, int y0, int z0, int x1, int y1, int z1) const;

    /** StructurePiece.fillColumnDown with the TF coordinate mapping. */
    void fillColumnDown(WorldGenLevel* level, BlockState* state, int x, int startY, int z,
                        const BoundingBox& chunkBB) const;

    /**
     * TFStructureComponentOld.fillWithBlocks(world, bb, min.., max.., border,
     * interior, predicate): a cell is a border when its axis has extent and
     * it sits on that axis' face.
     */
    void fillWithBlocks(WorldGenLevel* level, const BoundingBox& chunkBB,
                        int xMin, int yMin, int zMin, int xMax, int yMax, int zMax,
                        BlockState* borderState, BlockState* interiorState,
                        const std::function<bool(BlockState*)>& predicate) const;

    /** TFStructureComponentOld.surroundBlockCardinal. */
    void surroundBlockCardinal(WorldGenLevel* level, BlockState* state, int x, int y, int z,
                               const BoundingBox& chunkBB) const;

    /** TFStructureComponentOld.surroundBlockCorners. */
    void surroundBlockCorners(WorldGenLevel* level, BlockState* state, int x, int y, int z,
                              const BoundingBox& chunkBB) const;

    /** TFStructureComponentOld.setSpawner(world, x, y, z, sbb, monsterID). */
    void setSpawner(WorldGenLevel* level, int x, int y, int z, const BoundingBox& chunkBB,
                    const std::string& entityId) const;

    /** TFStructureComponentOld.placeTreasureAtCurrentPosition. */
    void placeTreasureAtCurrentPosition(WorldGenLevel* level, int x, int y, int z,
                                        const std::string& lootTable, bool trapped,
                                        const BoundingBox& chunkBB) const;

    /**
     * TFStructureComponentOld.placeTreasureAtWorldPosition: inside `chunkBB`
     * and not already that chest, TFLootTables.generateChest facing the
     * piece's orientation (NORTH when the orientation is null).
     */
    void placeTreasureAtWorldPosition(WorldGenLevel* level, const std::string& lootTable, bool trapped,
                                      const BoundingBox& chunkBB, const core::BlockPos& pos) const;

    /** TFStructureComponentOld.findGroundLevel (column at the chunkBB centre). */
    int findGroundLevel(WorldGenLevel* level, const BoundingBox& chunkBB, int start,
                        const std::function<bool(BlockState*)>& predicate) const;
};

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
