// File: src/common/world/biome/Biomes.hpp
//
// Vanilla biome colour data, and the exact colour maths MC applies on top of it.
//
// Port of, in order:
//   net/minecraft/world/level/biome/Biome            (getGrassColor / getFoliageColor / …)
//   net/minecraft/world/level/biome/BiomeSpecialEffects.GrassColorModifier
//   net/minecraft/world/level/ColorMapColorUtil      (the colormap index)
//   net/minecraft/world/level/{Grass,Foliage,DryFoliage}Color
//
// The per-biome numbers come from the vendored vanilla JSON via
// tools/gen_biomes.py, so temperature/downfall and the handful of hard-coded
// overrides are vanilla's own values rather than transcriptions.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Game {

    // MC BiomeSpecialEffects.GrassColorModifier — applied to the grass colour
    // only, AFTER the override/colormap lookup.
    enum class GrassColorModifier : uint8_t { None, DarkForest, Swamp };

    // Index into the generated table. Travels in ChunkDataS2C, so the ordering
    // is append-only (see tools/gen_biomes.py).
    using BiomeId = uint16_t;

    // The biome every "no data" path resolves to: an unloaded chunk, a blend
    // sample past a border, an accessor that tracks no biomes.
    //
    // This is MC's behaviour, not a convenience — ClientLevel.getUncachedNoiseBiome
    // returns Biomes.PLAINS outright, and LevelReader.getNoiseBiome calls it for
    // any chunk that isn't loaded to BIOMES status.
    //
    // Pinned to 0 so the low-level accessors can answer with it WITHOUT pulling
    // in the biome table (Chunk, IBlockAccess and the mesh snapshot are all
    // hot, widely-included headers). Biomes.cpp static_asserts that slot 0 is
    // actually plains, so a regenerated table that loses the pin fails the
    // build rather than silently tinting the world badlands-tan.
    inline constexpr BiomeId kFallbackBiomeId = 0;

    struct BiomeInfo {
        std::string_view   name;          // "plains" (no namespace)
        float              temperature;
        float              downfall;
        uint32_t           waterColor;    // RGB
        bool               hasGrassOverride;
        uint32_t           grassColor;
        bool               hasFoliageOverride;
        uint32_t           foliageColor;
        bool               hasDryFoliageOverride;
        uint32_t           dryFoliageColor;
        GrassColorModifier grassModifier;
    };

    namespace BiomeRegistry {
        // Number of biomes in the generated table.
        BiomeId Count();

        // Out-of-range ids resolve to the fallback (plains) rather than
        // reading past the table — a chunk from a newer peer can name a biome
        // this build doesn't know.
        const BiomeInfo& Get(BiomeId id);

        // Slug lookup ("minecraft:" prefix optional). Returns Fallback() when
        // unknown, so terrain conversion never has to special-case a miss.
        BiomeId FromName(std::string_view name);

        BiomeId Fallback();          // plains

        // Loads textures/colormap/{grass,foliage,dry_foliage}.png into CPU-side
        // 256x256 tables and precomputes each biome's base colours.
        //
        // Must run before any colour query. Colours resolve to vanilla's
        // documented no-colormap defaults until it does, so a missed call
        // degrades rather than crashes.
        bool LoadColormaps(const std::string& texturesRoot);

        // ── MC Biome colour accessors ───────────────────────────────────────
        //
        // Grass takes (x, z) because GrassColorModifier::Swamp samples a noise
        // field per position (Biome.java:190-193 → BiomeSpecialEffects:66-69).
        // Every other channel is constant per biome.
        uint32_t GrassColor(BiomeId id, int worldX, int worldZ);
        uint32_t FoliageColor(BiomeId id);

        // Sample the colormaps directly at an explicit (temperature, downfall)
        // rather than via a biome. This is what MC's `minecraft:grass` /
        // `minecraft:foliage` ITEM tint sources do: an item has no biome, so
        // its model JSON carries a fixed climate to look up
        // (e.g. bush's {"type":"minecraft:grass","temperature":0.5,
        // "downfall":1.0}). Returns vanilla's no-colormap constant until
        // LoadColormaps has run.
        uint32_t GrassColorAt(float temperature, float downfall);
        uint32_t FoliageColorAt(float temperature, float downfall);
        uint32_t DryFoliageColor(BiomeId id);
        uint32_t WaterColor(BiomeId id);
    }

    // MC ColorMapColorUtil.get — exposed for tests and for anything that wants
    // to sample a colormap directly.
    //
    //   rain *= temp;
    //   idx = ((int)((1 - rain) * 255) << 8) | (int)((1 - temp) * 255);
    //
    // `pixels` is a 256*256 RGB table; out-of-range indices return `fallback`.
    uint32_t ColorMapLookup(double temperature, double rainfall,
                            const uint32_t* pixels, size_t pixelCount,
                            uint32_t fallback);

} // namespace Game
