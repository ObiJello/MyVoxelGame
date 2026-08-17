// File: src/common/world/chunk/IBlockAccess.hpp
#pragma once

#include "../block/Blocks.hpp"
#include "common/world/biome/Biomes.hpp"

namespace Game {

    // Minimal interface for read-only block access
    // This allows physics, meshing, and other systems to access block data
    // without depending on the entire World class
    struct IBlockAccess {
        virtual ~IBlockAccess() = default;

        // Core block access methods
        virtual BlockID GetBlock(int worldX, int worldY, int worldZ) const = 0;

        // State index within the block's own state list (MC BlockState.getId());
        // 0 is always the block's default state. NOT pure virtual on purpose:
        // "I don't track state" and "everything is at its default state" are the
        // same answer, so accessors that only carry block ids (mesher snapshots,
        // physics views) stay correct without implementing anything.
        virtual uint8_t GetBlockState(int /*worldX*/, int /*worldY*/, int /*worldZ*/) const { return 0; }

        // Biome id at a world position, quantised to MC's 4x4x4 noise-biome
        // grid by the chunk. Accessors with no biome data answer 0, which is
        // the fallback biome — so a colour query degrades to plains rather
        // than failing, exactly as it did before biomes existed.
        virtual uint16_t GetBiome(int /*worldX*/, int /*worldY*/, int /*worldZ*/) const {
            return kFallbackBiomeId;
        }

        // Chunk loading state queries
        virtual bool IsChunkLoaded(int chunkX, int chunkZ) const = 0;
        virtual bool IsPositionLoaded(int worldX, int worldY, int worldZ) const = 0;

        // Convenience methods for physics and other systems
        virtual bool IsBlockSolid(int worldX, int worldY, int worldZ) const = 0;
        virtual bool IsBlockFluid(int worldX, int worldY, int worldZ) const = 0;
        virtual bool IsValidPosition(int worldX, int worldY, int worldZ) const = 0;

        // ── Light (MC LevelReader.getRawBrightness(pos, amount)) ────────────
        //
        // STAND-IN. This engine has no light engine at all — no sky light, no
        // block light, no propagation — so there is nothing to read. Every MC
        // farming rule is nevertheless gated on light (crops need >= 9 to grow
        // and >= 8 to survive), and dropping those gates outright would let
        // wheat grow in a sealed cave, which reads as a bug.
        //
        // So: 15 when this column is open to the sky, 0 when it is roofed.
        // That reproduces the behaviour players actually notice — crops grow
        // outdoors, refuse indoors — and gets the comparison operators right,
        // so porting a real light engine later means replacing THIS function
        // and nothing else. Call sites keep MC's literal `>= 9` / `>= 8`.
        //
        // Deliberately NOT modelled: torches (no block light to read), the
        // per-block attenuation that lets crops grow under a leaf canopy at
        // reduced light, and night. MC's crops keep growing at night because
        // `getRawBrightness(pos, 0)` reads raw sky light, not the time-of-day
        // -dimmed value — so returning a constant 15 for open sky is correct
        // rather than a simplification.
        //
        // The default walks the column with GetBlock, which is honest but
        // O(height). It is affordable because it is only reached for blocks
        // that random-tick at all — a few per tick, never ordinary terrain —
        // but an implementation with a heightmap should override it.
        virtual int GetRawBrightness(int worldX, int worldY, int worldZ) const;
    };

} // namespace Game