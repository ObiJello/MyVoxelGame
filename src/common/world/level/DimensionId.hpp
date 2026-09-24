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
//
// 2 is ours: The Hush, the engine's own dimension behind the ancient city's
// reinforced-deepslate frame. Vanilla never assigns 2 (its ids stop at 1), so
// the number can never collide with an imported world, and its save folder is
// MC's custom-dimension layout (`dimensions/<namespace>/<name>`) so a real
// Minecraft opening the world simply ignores it.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Game {

    enum class DimensionId : int8_t {
        Nether    = -1,
        Overworld =  0,
        End       =  1,
        Hush      =  2,
        // Ports of two Java mods, on their own registry namespaces. Values
        // from the mods' dimension types (Twilight Forest 4.9's
        // twilight_forest_type, The Aether 1.5.10's the_aether).
        TwilightForest = 3,
        Aether         = 4,
    };

    inline constexpr DimensionId kAllDimensions[] = {
        DimensionId::Overworld, DimensionId::Nether, DimensionId::End, DimensionId::Hush,
        DimensionId::TwilightForest, DimensionId::Aether,
    };
    inline constexpr int kDimensionCount = 6;

    // Dense 0..3 index, for arrays. NOT the wire or save id — use the enum
    // value for those.
    inline constexpr int DimensionSlot(DimensionId d) {
        switch (d) {
            case DimensionId::Overworld: return 0;
            case DimensionId::Nether:    return 1;
            case DimensionId::End:       return 2;
            case DimensionId::Hush:      return 3;
            case DimensionId::TwilightForest: return 4;
            case DimensionId::Aether:    return 5;
        }
        return 0;
    }

    inline constexpr DimensionId DimensionFromSlot(int slot) {
        switch (slot) {
            case 1:  return DimensionId::Nether;
            case 2:  return DimensionId::End;
            case 3:  return DimensionId::Hush;
            case 4:  return DimensionId::TwilightForest;
            case 5:  return DimensionId::Aether;
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
            case  2: return DimensionId::Hush;
            case  3: return DimensionId::TwilightForest;
            case  4: return DimensionId::Aether;
            default: return DimensionId::Overworld;
        }
    }

    inline constexpr int DimensionToRaw(DimensionId d) {
        return static_cast<int>(d);
    }

    // The bare registry name, for logs and commands.
    inline constexpr std::string_view DimensionName(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:    return "the_nether";
            case DimensionId::End:       return "the_end";
            case DimensionId::Hush:      return "the_hush";
            case DimensionId::TwilightForest: return "twilight_forest";
            case DimensionId::Aether:    return "the_aether";
            case DimensionId::Overworld: return "overworld";
        }
        return "overworld";
    }

    // The namespaced registry name — what a player's `Dimension` NBT key and
    // anything else that speaks MC resource locations carries. The one place
    // the `obeycraft:` namespace lives.
    inline constexpr std::string_view DimensionRegistryName(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:    return "minecraft:the_nether";
            case DimensionId::End:       return "minecraft:the_end";
            case DimensionId::Hush:      return "obeycraft:the_hush";
            case DimensionId::TwilightForest: return "twilightforest:twilight_forest";
            case DimensionId::Aether:    return "aether:the_aether";
            case DimensionId::Overworld: return "minecraft:overworld";
        }
        return "minecraft:overworld";
    }

    inline constexpr std::optional<DimensionId> DimensionFromRegistryName(std::string_view name) {
        for (const DimensionId d : kAllDimensions) {
            if (name == DimensionRegistryName(d)) return d;
        }
        return std::nullopt;
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
            case DimensionId::Hush:      return "hush";
            case DimensionId::TwilightForest: return "twilight_forest";
            case DimensionId::Aether:    return "aether";
            case DimensionId::Overworld: return "overworld";
        }
        return "overworld";
    }

    // Sub-directory of the world folder holding this dimension's regions.
    // Empty for the overworld, which lives at the world root — MC's layout.
    // The Hush uses MC's custom-dimension layout (`dimensions/<ns>/<name>`),
    // which SaveRoot joins as a path, so the nested form is fine.
    inline constexpr std::string_view DimensionSaveSubdir(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:    return "DIM-1";
            case DimensionId::End:       return "DIM1";
            case DimensionId::Hush:      return "dimensions/obeycraft/the_hush";
            case DimensionId::TwilightForest: return "dimensions/twilightforest/twilight_forest";
            case DimensionId::Aether:    return "dimensions/aether/the_aether";
            case DimensionId::Overworld: return "";
        }
        return "";
    }

    // MC DimensionType.getTeleportationScale — the horizontal coordinate ratio
    // applied when moving between dimensions. Only the overworld/nether pair
    // is scaled (8:1); everything else is 1:1.
    //
    // MC computes it as `from.coordinateScale() / to.coordinateScale()` with
    // the nether at 8.0 and the other two at 1.0, so overworld→nether divides
    // by 8 and nether→overworld multiplies by 8.
    inline constexpr double DimensionCoordinateScale(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:         return 8.0;
            case DimensionId::TwilightForest: return 0.125;   // TF: eight of its blocks per overworld block
            default:                          return 1.0;
        }
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
        switch (d) {
            case DimensionId::Overworld:
            case DimensionId::Hush:      return -64;
            case DimensionId::TwilightForest: return -32;
            case DimensionId::Nether:
            case DimensionId::End:
            case DimensionId::Aether:    return 0;
        }
        return 0;
    }
    inline constexpr int DimensionLogicalHeight(DimensionId d) {
        switch (d) {
            case DimensionId::Nether:    return 128;
            case DimensionId::End:       return 256;
            case DimensionId::Hush:      return 384;
            case DimensionId::TwilightForest: return 288;
            case DimensionId::Aether:    return 256;
            case DimensionId::Overworld: return 384;
        }
        return 384;
    }

    // MC DimensionType.hasSkyLight / hasCeiling. The nether is the only one
    // with a bedrock roof, and the only one lit entirely by block light. The
    // Hush is an open-sky world under a fixed night. The End HAS sky light in
    // 26.x (the_end.json has_skylight true): its sky_light_factor is 0, so it
    // is invisible until an End flash raises it, but the light engine stores
    // it and the monster spawn rules read it (MC-exact).
    inline constexpr bool DimensionHasSkyLight(DimensionId d) {
        return d != DimensionId::Nether;
    }
    inline constexpr bool DimensionHasCeiling(DimensionId d) {
        return d == DimensionId::Nether;
    }

    // MC DimensionType.ambientLight — 0.1 in the Nether, 0 everywhere else.
    inline constexpr float DimensionAmbientLight(DimensionId d) {
        return d == DimensionId::Nether ? 0.1f : 0.0f;
    }

    // MC DimensionType.fixedTime — the day time a dimension is frozen at, or
    // nullopt when its clock runs. The End and the Nether have fixed time in
    // vanilla too, but nothing in this engine reads their clocks for the sky
    // (the Nether has no sky, the End draws its own), so only the Hush, whose
    // open sky and surface spawning depend on it, is pinned: 18000 is MC's
    // midnight.
    inline constexpr std::optional<int64_t> DimensionFixedTime(DimensionId d) {
        if (d == DimensionId::Hush) return int64_t{18000};
        // Twilight Forest: has_fixed_time, a perpetual dusk (the mod's
        // traditional 13000).
        if (d == DimensionId::TwilightForest) return int64_t{13000};
        return std::nullopt;
    }

    // MC BaseFireBlock.inPortalDimension (BaseFireBlock.java:157) — a nether
    // portal can only be LIT in the overworld or the nether. Lighting a fire
    // inside an obsidian frame in the End (or the Hush) does nothing but burn.
    inline constexpr bool DimensionAllowsNetherPortal(DimensionId d) {
        return d == DimensionId::Overworld || d == DimensionId::Nether;
    }

    // The Hush's counterpart: a reinforced-deepslate frame resonates with an
    // echo shard only in the overworld (the ancient city) or the Hush itself
    // (the return trip).
    inline constexpr bool DimensionAllowsHushPortal(DimensionId d) {
        return d == DimensionId::Overworld || d == DimensionId::Hush;
    }

    // The Aether's counterpart (DimensionHooks.createPortal): a glowstone
    // frame takes water only in the return dimension (the Overworld) or the
    // Aether itself — LevelUtil.returnDimension / destinationDimension.
    inline constexpr bool DimensionAllowsAetherPortal(DimensionId d) {
        return d == DimensionId::Overworld || d == DimensionId::Aether;
    }

} // namespace Game
