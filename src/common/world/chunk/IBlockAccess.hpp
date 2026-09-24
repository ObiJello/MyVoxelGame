// File: src/common/world/chunk/IBlockAccess.hpp
#pragma once

#include <glm/vec3.hpp>
#include <cstdint>

#include "../block/Blocks.hpp"
#include "../block/BlockState.hpp"
#include "common/world/biome/Biomes.hpp"

namespace Game::Lighting {
    enum class LightLayer : uint8_t;
}

namespace Game {

    // Minimal interface for read-only block access
    // This allows physics, meshing, and other systems to access block data
    // without depending on the entire World class
    struct IBlockAccess {
        virtual ~IBlockAccess() = default;

        // Core block access methods
        virtual BlockID GetBlock(int worldX, int worldY, int worldZ) const = 0;

        // State index within the block's own state list (MC BlockState.getId()).
        //
        // PURE VIRTUAL, deliberately. This used to default to `return 0` on the
        // reasoning that "I don't track state" and "everything is at its
        // default state" are the same answer. That reasoning holds only while
        // state index 0 IS the default for every block — and it is about to
        // stop being true. In MC, `StateDefinition.any()` takes the first value
        // of every property and BooleanProperty lists `true` first, so the
        // default is somewhere else entirely for most blocks; once states are
        // global ids, 0 means "air's default state" and nothing else.
        //
        // An accessor that inherited the default would then report the whole
        // world as air-shaped while GetBlock kept returning stone — no crash,
        // no warning, wrong collision and wrong meshing. Every implementor
        // already overrides this, so requiring it costs nothing today and
        // removes the trap before it can be sprung.
        virtual BlockState GetBlockState(int worldX, int worldY, int worldZ) const = 0;

        // Fill a packed bitset with "does this cell have a collision shape",
        // i.e. BlockRegistry::HasCollision(GetBlockState(c).Block()), over the
        // box [origin, origin+size). Bit index is
        //     (dy << shiftY) | (dz << shiftZ) | dx
        // with 1<<shiftZ >= size.x and 1<<(shiftY-shiftZ) >= size.z, so callers
        // can address cells with shifts instead of multiplies. `out` must hold
        // at least (size.y << shiftY + 63)/64 words and be zeroed by the caller.
        //
        // CONTRACT for overrides: a cell whose contents are UNKNOWN — a column
        // or section that is not resident — must be reported as SET, not clear.
        // Consumers treat a clear bit as authoritative and skip the live read,
        // so reporting an absent column as clear freezes "there is nothing
        // there" into a snapshot that a chunk arriving from a worker thread can
        // falsify. A set bit only means "go and look", which is always safe.
        //
        // The default walks GetBlockState cell by cell, so every accessor is
        // correct with no code — but note it necessarily reports an absent
        // column as CLEAR, because GetBlockState cannot distinguish absent from
        // air. That is safe only for callers that never outlive a chunk
        // install; an accessor used for longer-lived snapshots should override
        // this and honour the rule above. Defined in the .cpp, following
        // ContainsWater, so this header does not pull in BlockRegistry.
        virtual void FillCollisionMask(const glm::ivec3& origin, const glm::ivec3& size,
                                       int shiftZ, int shiftY, uint64_t* out) const;

        // True when every cell in [min, max] (inclusive, world coords) is
        // provably air — answered from section emptiness, so it costs one
        // flag test per (column, section) tile rather than one read per cell.
        // A column that is not resident answers FALSE (unknown is not air).
        // The default is the conservative answer; accessors with section
        // access override. Callers use it to skip cell-by-cell sweeps over
        // open sky: an entity's inside-block scan, an explosion's ray march.
        //
        // `absentIsAir` chooses what a non-resident column means: FALSE for a
        // reader that must not guess (the explosion scan — unknown is not
        // air), TRUE for collision (MC collides against nothing in an
        // unloaded chunk: Level.getChunkForCollisions returns null and the
        // iterator skips it), so an entity thrown past the loaded area flies
        // free instead of paying a cell walk over chunks that are not there.
        virtual bool IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                                    bool absentIsAir = false) const {
            (void)min; (void)max; (void)absentIsAir;
            return false;
        }

        // Every state in [min, max] (inclusive), written x-major:
        //     out[((x - min.x) * ny + (y - min.y)) * nz + (z - min.z)]
        // The default reads cell by cell; World answers per (column, section)
        // tile so a 27-cell collision gather is one chunk lookup and one
        // section lookup rather than 27 of each. Cells in non-resident
        // columns and outside the build range are air.
        // Sum of the per-column write counters over the columns [min, max]
        // spans (y ignored). Counters only grow, so an equal sum means no
        // block in those columns changed. 0 for accessors without counters —
        // their parked entities then rely on the impulse/motion unpark paths.
        virtual uint64_t RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const {
            (void)min; (void)max;
            return 0;
        }

        virtual void GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                         BlockState* out) const {
            const int ny = max.y - min.y + 1, nz = max.z - min.z + 1;
            for (int x = min.x; x <= max.x; ++x)
                for (int y = min.y; y <= max.y; ++y)
                    for (int z = min.z; z <= max.z; ++z)
                        out[((x - min.x) * ny + (y - min.y)) * nz + (z - min.z)] =
                            GetBlockState(x, y, z);
        }

        // Biome id at a world position. The level accessors (World on the
        // server, ClientBlockAccess on the client) answer MC
        // LevelReader.getBiome — the fuzzy-zoomed biome of the block
        // (BiomeZoom.hpp); a lower-level store such as ChunkProvider answers
        // its plain 4x4x4 cell. Accessors with no biome data answer 0, which
        // is the fallback biome — so a colour query degrades to plains rather
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

        // MC `level.getFluidState(pos).is(FluidTags.WATER)`.
        //
        // NOT the same question as "is the block here water": a waterlogged
        // fence, a kelp stalk and a coral fan all hold water while being
        // something else entirely. Every caller that used to compare
        // GetBlock() against BlockID::Water wants THIS instead — meshing,
        // entity fluid tests, bucket fill, block placement.
        //
        // The default composes GetBlock + GetBlockState, so an accessor only
        // has to override GetBlockState to answer correctly; one that tracks
        // no state still gets the always-water blocks and plain water right,
        // which is strictly better than the block-id comparison it replaces.
        //
        // Defined in the .cpp so this header — which half the engine includes —
        // does not have to pull in BlockRegistry.
        virtual bool ContainsWater(int worldX, int worldY, int worldZ) const;

        // ── Light (MC LevelReader light queries) ────────────────────────────
        //
        // GetBrightness is MC getBrightness(LightLayer, pos): the stored sky
        // or block light, 0..15. World answers from the level light engine
        // (Lighting::LevelLightManager), the client from the chunk layers the
        // server sent. The DEFAULT here is for accessors that carry no light
        // (snapshots, tests): sky 15 for a column no opaque block roofs, 0
        // otherwise; block 0 — the stand-in the engine used before it had a
        // light engine, kept so such an accessor still grows crops outdoors.
        virtual int GetBrightness(Lighting::LightLayer layer, int worldX, int worldY, int worldZ) const;

        // MC getRawBrightness(pos, 0) = max(block light, sky light). Crops
        // (>= 9 to grow), stems, bamboo and berries read this: raw sky light
        // is not dimmed at night, which is why vanilla crops grow in the dark
        // outdoors — and why a torch lets them grow underground.
        virtual int GetRawBrightness(int worldX, int worldY, int worldZ) const;

        // MC getMaxLocalRawBrightness(pos, amount) = max(block, sky - amount);
        // pass the level's sky darkening for the time-dimmed value grass
        // spread and saplings read.
        int GetMaxLocalRawBrightness(int worldX, int worldY, int worldZ, int amount) const;
    };

} // namespace Game