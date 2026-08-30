// File: src/common/world/level/DimensionId.hpp
//
// Which world a level, a chunk request or a player is in.
//
// The numbering is Minecraft's, not an arbitrary enum order: vanilla's
// dimension ids are -1 / 0 / 1 and its save layout follows them
// (`<world>/region`, `<world>/DIM-1/region`, `<world>/DIM1/region`). Keeping
// the same numbers means the save directories are computed from the id, an
// imported Minecraft world's Nether lands where this engine looks for it, and
// the `int m_dimensionId` that ServerPlayer and PlayerSession already carried
// (defaulting to 0) keeps meaning exactly what it always did.
#pragma once

#include <cstdint>
#include <string_view>

namespace Game {

    enum class DimensionId : int8_t {
        Nether    = -1,
        Overworld =  0,
        End       =  1,
    };

    inline constexpr DimensionId kAllDimensions[] = {
        DimensionId::Overworld, DimensionId::Nether, DimensionId::End,
    };
    inline constexpr int kDimensionCount = 3;

    // Dense 0..2 index, for arrays. NOT the wire or save id — use the enum
    // value for those.
    inline constexpr int DimensionSlot(DimensionId d) {
        switch (d) {
            case DimensionId::Overworld: return 0;
            case DimensionId::Nether:    return 1;
            case DimensionId::End:       return 2;
        }
        return 0;
    }

    inline constexpr DimensionId DimensionFromSlot(int slot) {
        switch (slot) {
            case 1:  return DimensionId::Nether;
            case 2:  return DimensionId::End;
            default: return DimensionId::Overworld;
        }
    }

    // An id off the wire or out of a save. Anything unrecognised becomes the
    // overworld rather than a crash — the same failure-safe choice the block
    // and biome loaders make.
    inline constexpr DimensionId DimensionFromRaw(int raw) {
        switch (raw) {
            case -1: return DimensionId::Nether;
            case  1: return DimensionId::End;
            default: return DimensionId::Overworld;
        }
    }

    inline constexpr int DimensionToRaw(DimensionId d) {
        return static_cast<int>(d);
    }

    // The vanilla registry name, for logs and anything that speaks MC ids.
    inline constexpr std::string_view DimensionName(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:    return "the_nether";
            case DimensionId::End:       return "the_end";
            case DimensionId::Overworld: return "overworld";
        }
        return "overworld";
    }

    // The key `GenerationConfig::dimension` is matched against. Deliberately
    // NOT DimensionName: the terrain-generator branch predates the registry
    // names, reads "nether"/"end", and the two strings must not be allowed to
    // drift into each other by accident — a mismatch silently generates
    // overworld terrain rather than failing.
    inline constexpr std::string_view DimensionGeneratorKey(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:    return "nether";
            case DimensionId::End:       return "end";
            case DimensionId::Overworld: return "overworld";
        }
        return "overworld";
    }

    // Sub-directory of the world folder holding this dimension's regions.
    // Empty for the overworld, which lives at the world root — MC's layout.
    inline constexpr std::string_view DimensionSaveSubdir(DimensionId d) {
        switch (d) {
            case DimensionId::Nether: return "DIM-1";
            case DimensionId::End:    return "DIM1";
            default:                  return "";
        }
    }

    // MC DimensionType.getTeleportationScale — the horizontal coordinate ratio
    // applied when moving between dimensions. Only the overworld/nether pair
    // is scaled (8:1); everything else is 1:1.
    //
    // MC computes it as `from.coordinateScale() / to.coordinateScale()` with
    // the nether at 8.0 and the other two at 1.0, so overworld→nether divides
    // by 8 and nether→overworld multiplies by 8.
    inline constexpr double DimensionCoordinateScale(DimensionId d) {
        return d == DimensionId::Nether ? 8.0 : 1.0;
    }
    inline constexpr double TeleportationScale(DimensionId from, DimensionId to) {
        return DimensionCoordinateScale(from) / DimensionCoordinateScale(to);
    }

    // MC DimensionType.logicalHeight — how far above minY a portal or a
    // chorus fruit may place you. The nether's 128 is what keeps a portal
    // from being built on top of the bedrock roof.
    //
    // NOTE: this is NOT the chunk storage height. Every dimension in this
    // engine stores -64..319 because ChunkSection's layout is fixed; the
    // nether simply generates nothing above 127.
    inline constexpr int DimensionMinY(DimensionId d) {
        return d == DimensionId::Overworld ? -64 : 0;
    }
    inline constexpr int DimensionLogicalHeight(DimensionId d) {
        switch (d) {
            case DimensionId::Nether: return 128;
            case DimensionId::End:    return 256;
            default:                  return 384;
        }
    }

    // MC DimensionType.hasSkyLight / hasCeiling. The nether is the only one
    // with a bedrock roof, and the only one lit entirely by block light.
    inline constexpr bool DimensionHasSkyLight(DimensionId d) {
        return d == DimensionId::Overworld;
    }
    inline constexpr bool DimensionHasCeiling(DimensionId d) {
        return d == DimensionId::Nether;
    }

    // MC BaseFireBlock.inPortalDimension (BaseFireBlock.java:157) — a nether
    // portal can only be LIT in the overworld or the nether. Lighting a fire
    // inside an obsidian frame in the End does nothing but burn.
    inline constexpr bool DimensionAllowsNetherPortal(DimensionId d) {
        return d == DimensionId::Overworld || d == DimensionId::Nether;
    }

} // namespace Game
