#pragma once

#include "levelgen/structure/StructurePlacement.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/Structures.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ENGINE EXTENSION (no Minecraft counterpart): companion structures, and the
// first of them — the outskirts of Aurelith, the Hush's city (docs/the-hush.md
// "Aurelith — the outskirts"; tools/gen_aurelith_outskirts.py writes the data).
//
// WHY. A city ~220 blocks across wants ruined roads running a few hundred
// blocks out from its gates, a necropolis, a waystation and broken farmsteads
// along the way. None of it can belong to the city's own start: the jigsaw
// caps max_distance_from_center at 128, and — the real limit — a start is
// only ever referenced by chunks within 8 chunks of it
// (StructureGeneration::createReferences, MC ChunkGenerator.createReferences),
// so a piece further out than that is simply never placed. Raising that radius
// would change the chunk-status dependency grid for every structure.
//
// WHAT. A companion structure set places many small starts AROUND an anchor
// structure's start, at fixed offsets in the anchor's DESIGN frame, turned by
// the anchor's actual rotation:
//
//   * AnchoredStructurePlacement ("obeycraft:anchored") — a structure set
//     placement. A chunk is a placement chunk when, for a potential start
//     chunk A of the anchor set's random_spread grid and some rotation r, one
//     of the placement's `slots` (a design-frame block offset from the
//     anchor's reference point, `anchor_origin` in the anchor's start piece)
//     lands in it:  chunk = A + floor(R_r(slot + anchor_origin) / 16).
//     R_r is MC Rotation.rotate (StructureTemplate.transform with pivot 0 —
//     how the jigsaw turns the start piece). The anchor placement's own
//     frequency and exclusion rules are applied to A. Cheap: a handful of LCG
//     draws per chunk, no generation.
//
//   * The structure type ("obeycraft:aurelith_outskirts") settles what the
//     placement could not: it runs the anchor's own generation for A (the
//     biome check and the level-site test Aurelith needs — "does the city
//     exist here?"), reads the start piece's rotation, and keeps only the
//     slots whose chunk matches under THAT rotation. The anchor's result is
//     cached per (seed, A), so the dozen companion starts of one city pay for
//     the city's layout once. Every piece is then placed relative to the
//     anchor's reference point (the Heart), turned by the city's rotation,
//     so the roads leave along the gates' real axes.
//
// PIECES. A slot carries:
//   roads   — code pieces (AurelithRoad below): a straight road in the design
//             frame from `a` to `b`, the stretch t0..t1 of it (the stretches
//             of one road are separate slots, one start per ~64 blocks, so no
//             piece is further than the reference radius from its start).
//             Each column is set on the column's OWN surface (the chunk's
//             WORLD_SURFACE_WG heightmap at decoration time), so a road
//             drapes over the ground instead of floating or cutting; only
//             natural ground is paved (never water, never another
//             structure's blocks), and the paving thins with distance
//             (`keep` at a .. at b), cracks, sinks, grows moss — broken and
//             fading. Road pieces are pool-element pieces with non-rigid
//             projection, so the Beardifier never flattens the land under
//             them (Beardifier.forStructuresInChunk's rule for
//             terrain-matching pieces).
//   pieces  — structure templates (single-element semantics, rigid) at a
//             design position (their centre) and design rotation, set on the
//             median surface height of their footprint; they take the
//             structure's terrain_adaptation (beard_thin) like any rigid
//             jigsaw piece. `chance` drops a piece per city (a hash of the
//             world seed, the anchor chunk and the piece), so no two cities'
//             outskirts are the same; `dry` refuses a piece whose centre is
//             under water.
//
// DECORATION ORDER. Engine structures (namespace "obeycraft:") are appended
// AFTER every vanilla and mod structure of their step in the decoration
// order (ChunkGenerator::applyBiomeDecoration), so adding one never shifts
// the per-structure feature seed of a vanilla, Twilight Forest or Aether
// structure.

namespace minecraft {
namespace levelgen {
namespace structure {

class AnchoredStructurePlacement : public StructurePlacement {
public:
    AnchoredStructurePlacement(int32_t salt, std::string anchorSet,
                               int32_t originX, int32_t originZ,
                               std::vector<std::pair<int32_t, int32_t>> slots);

    const std::string& anchorSet() const { return m_anchorSet; }
    int32_t originX() const { return m_originX; }
    int32_t originZ() const { return m_originZ; }
    const std::vector<std::pair<int32_t, int32_t>>& slots() const { return m_slots; }

    bool isPlacementChunk(const ChunkGeneratorStructureState& state,
                          int32_t sourceX, int32_t sourceZ) const override;

    // The start chunk of design-frame slot `slot` for an anchor start at
    // chunk (ax, az) turned by `rotation` (0..3).
    static std::pair<int32_t, int32_t> slotChunk(int32_t ax, int32_t az, int rotation,
                                                 int32_t originX, int32_t originZ,
                                                 int32_t slotX, int32_t slotZ);

private:
    std::string m_anchorSet;
    int32_t m_originX;
    int32_t m_originZ;
    std::vector<std::pair<int32_t, int32_t>> m_slots;
    int32_t m_reachChunks = 0;   // max |chunk offset| of any slot, any rotation
};

namespace AurelithOutskirts {

/** True for the engine's companion structure types. */
bool isOutskirtsType(const std::string& type);

/** Layout + pieces for one companion start (see the file comment). */
bool generate(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out);

} // namespace AurelithOutskirts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
