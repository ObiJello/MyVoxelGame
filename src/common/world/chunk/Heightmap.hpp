// File: src/common/world/chunk/Heightmap.hpp
//
// MC net.minecraft.world.level.levelgen.Heightmap.
//
// One height per column of a chunk — 16x16 = 256 entries — under a per-type
// predicate. What makes it worth having is not the lookup being fast; it is
// that MC never rebuilds it. A chunk is primed ONCE, and every block change
// after that costs O(1) in the common case (see Update). Anything that answers
// "what is the surface height here" by scanning the column is doing per-query
// what this does per-write.
//
// Consumers here: the natural spawner (MOTION_BLOCKING_NO_LEAVES, to bound
// where it samples Y) and the sky test that mob spawning and undead burning
// both read (WORLD_SURFACE).
//
// ── Divergence from MC, deliberate ────────────────────────────────────────
//
// MC packs entries into a SimpleBitStorage at ceillog2(height+1) = 9 bits,
// 288 bytes per map. This stores a plain uint16 per column, 512 bytes. The
// reason is not laziness: the faithful SimpleBitStorage lives in
// src/my_terrain_library, and CLAUDE.md forbids putting `src/` on that
// library's include path (both trees have a top-level `server/`, so headers
// would resolve to the wrong one). 224 bytes per map per chunk is a price
// worth paying to keep that boundary intact.
//
// The ANVIL SERIALISATION still uses MC's exact bit layout — see
// PackToLongs/UnpackFromLongs — because that crosses into vanilla's format
// and has to be byte-compatible.
#pragma once

#include "../block/Blocks.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Game {

    // MC Heightmap.Types, reduced to the two this engine consumes. Kept as an
    // enum rather than two separate members so the storage, the update loop and
    // the NBT round-trip can all iterate.
    //
    // The serialization keys are vanilla's, so worlds stay readable in MC.
    enum class HeightmapType : uint8_t {
        // MC MOTION_BLOCKING_NO_LEAVES: blocksMotion() || hasFluid(), minus
        // leaves. What the mob spawner samples against.
        MotionBlockingNoLeaves = 0,
        // MC WORLD_SURFACE: any non-air block. What the sky test reads.
        WorldSurface,
        Count
    };

    inline constexpr const char* HeightmapSerializationKey(HeightmapType type) {
        switch (type) {
            case HeightmapType::MotionBlockingNoLeaves: return "MOTION_BLOCKING_NO_LEAVES";
            case HeightmapType::WorldSurface:           return "WORLD_SURFACE";
            default: return "";
        }
    }

    // Build the per-BlockID predicate table. Must run after
    // BlockRegistry::Init(), and is re-runnable — BlockRegistry re-registers
    // blocks on world reload.
    void InitHeightmapTable();

    // MC's per-type predicate, answered from that table.
    bool HeightmapIsOpaque(HeightmapType type, BlockID block);

    class Heightmap {
    public:
        static constexpr int kColumns = 256;   // 16 x 16

        // MC stores "first available Y", i.e. one ABOVE the topmost matching
        // block, and getHeight() returns that minus one. Storing MC's value
        // rather than the surface itself keeps the empty-column case honest:
        // an all-air column is minY, not "minY - 1".
        Heightmap() { Reset(MinY()); }

        static constexpr int MinY() { return -64; }
        static constexpr int MaxY() { return 319; }

        // First Y with nothing matching at or above it.
        int GetFirstAvailable(int localX, int localZ) const {
            return static_cast<int>(m_data[Index(localX, localZ)]) + MinY();
        }

        // The topmost matching block itself — MC ChunkAccess.getHeight().
        int GetHeight(int localX, int localZ) const {
            return GetFirstAvailable(localX, localZ) - 1;
        }

        void SetHeight(int localX, int localZ, int firstAvailable) {
            m_data[Index(localX, localZ)] =
                static_cast<uint16_t>(firstAvailable - MinY());
        }

        void Reset(int firstAvailable) {
            m_data.fill(static_cast<uint16_t>(firstAvailable - MinY()));
        }

        // MC Heightmap.update — the whole reason a heightmap is cheap.
        //
        // `blockAt` reads the CHUNK's blocks at chunk-local x/z and world y; it
        // is only called in the one case that needs a scan (the top block was
        // removed), and then only downward from there.
        //
        // Returns true when the stored height changed.
        template <typename BlockAt>
        bool Update(int localX, int worldY, int localZ, BlockID newBlock,
                    HeightmapType type, BlockAt&& blockAt) {
            const int firstAvailable = GetFirstAvailable(localX, localZ);

            // Two or more blocks below the surface: cannot possibly matter.
            // This is the early-out that makes the common case free — most
            // block edits in a world are underground or inside a build.
            if (worldY <= firstAvailable - 2) return false;

            if (HeightmapIsOpaque(type, newBlock)) {
                // Placed at or above the surface: the surface is now here.
                if (worldY >= firstAvailable) {
                    SetHeight(localX, localZ, worldY + 1);
                    return true;
                }
                return false;
            }

            // Removed the block that WAS the surface — the only branch that
            // scans, and it starts at the old surface rather than the top of
            // the world.
            if (firstAvailable - 1 == worldY) {
                for (int y = worldY - 1; y >= MinY(); --y) {
                    if (HeightmapIsOpaque(type, blockAt(localX, y, localZ))) {
                        SetHeight(localX, localZ, y + 1);
                        return true;
                    }
                }
                SetHeight(localX, localZ, MinY());
                return true;
            }

            return false;
        }

        // ── Anvil serialisation (MC SimpleBitStorage layout) ───────────────
        //
        // 9 bits per entry, and entries do NOT straddle long boundaries: 7 per
        // 64-bit long with the top bit wasted, so 256 columns need 37 longs and
        // not the 36 a dense packing would give. Getting this wrong produces a
        // file MC reads as garbage heights.
        static constexpr int kBitsPerEntry = 9;
        static constexpr int kEntriesPerLong = 64 / kBitsPerEntry;              // 7
        static constexpr int kLongCount = (kColumns + kEntriesPerLong - 1) / kEntriesPerLong; // 37

        std::vector<int64_t> PackToLongs() const;
        // Returns false (and leaves the map untouched) when the input is not
        // the expected length — MC logs and re-primes in that case, and so
        // does the caller here.
        bool UnpackFromLongs(const std::vector<int64_t>& longs);

        const std::array<uint16_t, kColumns>& Raw() const { return m_data; }

    private:
        static int Index(int localX, int localZ) {
            return (localX & 15) + (localZ & 15) * 16;
        }

        std::array<uint16_t, kColumns> m_data{};
    };

} // namespace Game
