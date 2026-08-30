#pragma once

#include "levelgen/structure/Structures.h"
#include <functional>

// Per-family layout builders (piece graphs, bounding boxes, RNG - no block
// placement; postProcess parity lands in later batches). Each mirrors its
// Java Structure.findGenerationPoint + Pieces classes exactly, including RNG
// draw order. Dispatched from Structures::generate().

namespace minecraft {
namespace levelgen {
namespace structure {
namespace StructureLayouts {

/**
 * Reference: MineshaftStructure + MineshaftPieces. Pieces are built EAGERLY
 * (GenerationStub Either.right), so `validBiomeAt` runs LAST, at the adjusted
 * stub position (middleBlockX, 50 + yOffset, minBlockZ).
 */
bool generateMineshaft(const StructureInfo& info, GenerationContext& ctx,
                       StructureStartData& out,
                       const std::function<bool(int x, int y, int z)>& validBiomeAt);

/**
 * Reference: StrongholdStructure + StrongholdPieces. The stub is a LAZY
 * consumer at (minBlockX, 0, minBlockZ) - the caller must run the biome check
 * BEFORE calling this. Retries with setLargeFeatureSeed(seed + tries, cx, cz)
 * until pieces are nonempty and a portal room exists.
 */
bool generateStronghold(const StructureInfo& info, GenerationContext& ctx,
                        StructureStartData& out);

/**
 * Reference: NetherFortressStructure + NetherFortressPieces (layout half).
 * The stub is a LAZY consumer at (minBlockX, 64, minBlockZ) - the caller must
 * run the biome check BEFORE calling this. Draws: nextInt(4) direction,
 * weighted bridge/castle piece tables, pendingChildren BFS, then
 * moveInsideHeights(random, 48, 70).
 */
bool generateNetherFortress(const StructureInfo& info, GenerationContext& ctx,
                            StructureStartData& out);

/**
 * Reference: NetherFossilStructure + NetherFossilPieces. Runs the full
 * findGenerationPoint flow (x/z/height draws + base-column walk); calls the
 * biome check itself at the stub position before the rotation/template draws.
 * Returns false when the walk hits sea level or the biome rejects.
 */
bool generateNetherFossil(const StructureInfo& info, GenerationContext& ctx,
                          StructureStartData& out,
                          std::function<bool(int, int, int)> biomeCheck);

/**
 * Reference: EndCityStructure + EndCityPieces. Piece-building only - the
 * DISPATCH runs the rotation draw, the 5x5-lowest-y stub, the y<60 reject and
 * the biome check (mansion flow). rotation: Rotation.values() ordinal.
 */
bool generateEndCity(const StructureInfo& info, GenerationContext& ctx,
                     StructureStartData& out, int rotation,
                     int blockX, int startY, int blockZ);

/**
 * Reference: IglooStructure/IglooPieces. Piece-building only - the caller
 * runs it inside the onTopOfChunkCenter stub flow (biome check first).
 * Draws: Rotation.getRandom nextInt(4); nextDouble()<0.5 lab branch with
 * nextInt(8)+4 depth.
 */
bool generateIgloo(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out);

/**
 * Reference: ShipwreckStructure/ShipwreckPieces. Piece-building only (runs
 * inside the stub flow; heightmap type is WORLD_SURFACE_WG when beached,
 * OCEAN_FLOOR_WG otherwise - the CALLER picks it).
 */
bool generateShipwreck(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                       bool isBeached);

/** Reference: OceanRuinStructure/OceanRuinPieces (large + cluster logic). */
bool generateOceanRuin(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out);

/**
 * Reference: RuinedPortalStructure. Runs its own full stub flow: all draws
 * (setup pick, air pocket, template, rotation, mirror, findSuitableY) happen
 * BEFORE the biome check at (minBlockX, projectedY, minBlockZ), then the
 * single RUPO piece is built. Returns false on biome rejection.
 */
bool generateRuinedPortal(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                          const std::function<bool(int x, int y, int z)>& validBiomeAt);

/**
 * Reference: WoodlandMansionPieces.generateMansion. Piece-building only; the
 * caller draws rotation, computes startPos (getLowestYIn5by5BoxOffset7Blocks),
 * rejects y < 60, and runs the biome check first. rotation: 0..3 in
 * Rotation.values() order.
 */
bool generateMansion(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                     int rotation, int startPosX, int startPosY, int startPosZ);

/**
 * Reference: JigsawStructure.findGenerationPoint + JigsawPlacement. Runs its
 * own stub flow: center-piece selection/anchoring draws happen first, then
 * the biome check at the stub, then the BFS placer (lazy consumer). Aliases
 * and uniform start heights (trial_chambers) are not yet supported - callers
 * must gate via isImplemented.
 */
bool generateJigsaw(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                    const std::function<bool(int x, int y, int z)>& validBiomeAt);

} // namespace StructureLayouts
} // namespace structure
} // namespace levelgen
} // namespace minecraft
