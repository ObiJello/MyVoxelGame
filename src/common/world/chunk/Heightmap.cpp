// File: src/common/world/chunk/Heightmap.cpp
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"

#include <string_view>

namespace Game {

    namespace {

        // Precomputed predicate results, one bitmask per BlockID.
        //
        // Same reasoning as the pathfinder's PathTypeTable: MC asks a BlockState
        // a chain of tag questions per test, and this engine's equivalent is a
        // registry-slug comparison. Doing that string work inside Update — which
        // runs on every block write — and inside priming, which runs it 256
        // times per chunk with a downward scan each, would make the heightmap
        // cost more than the scans it replaces.
        //
        // Built at startup; see InitHeightmapTable.
        std::array<uint8_t, static_cast<size_t>(BlockID::Count)> g_table{};
        bool g_initialised = false;

        constexpr uint8_t kBitMotionBlockingNoLeaves = 1 << 0;
        constexpr uint8_t kBitWorldSurface           = 1 << 1;

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        uint8_t Classify(BlockID id, std::string_view slug) {
            uint8_t bits = 0;

            // MC WORLD_SURFACE: NOT_AIR. Everything else counts, including
            // flowers and torches — this is the "top of the world" map the sky
            // test wants, not a walkability map.
            if (id != BlockID::Air) bits |= kBitWorldSurface;

            // MC MOTION_BLOCKING_NO_LEAVES:
            //   (blocksMotion() || !fluidState.isEmpty()) && !(block is Leaves)
            //
            // Fluids are included even though you can walk into them — that is
            // the point of this map for spawning: it puts the surface at the
            // water's top, so nothing tries to spawn on the seabed.
            //
            // `!fluidState.isEmpty()` also catches the blocks that hold water
            // without being water — kelp, seagrass and bubble columns, whose
            // getFluidState is unconditionally WATER. None of them has
            // collision, so without this a kelp forest puts the surface back
            // down on the seabed and mobs try to spawn under the water.
            //
            // The per-state `waterlogged` blocks cannot be expressed here (this
            // table is keyed on BlockID alone) and do not need to be: every
            // waterloggable block that could change the answer — stairs, slabs,
            // fences, walls, trapdoors — already blocks motion, so the fluid
            // clause adds nothing for them either way.
            const bool isFluid = slug == "water" || slug == "flowing_water" ||
                                 slug == "lava"  || slug == "flowing_lava" ||
                                 BlockRegistry::IsAlwaysWaterlogged(id);
            const bool isLeaves = EndsWith(slug, "_leaves");

            if ((BlockRegistry::HasCollision(id) || isFluid) && !isLeaves) {
                bits |= kBitMotionBlockingNoLeaves;
            }

            return bits;
        }

    } // namespace

    void InitHeightmapTable() {
        for (size_t i = 0; i < g_table.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            g_table[i] = Classify(id, BlockRegistry::Get(id).registrySlug);
        }
        g_initialised = true;
        Log::Info("[Heightmap] Classified %zu block ids", g_table.size());
    }

    bool HeightmapIsOpaque(HeightmapType type, BlockID block) {
        const size_t idx = static_cast<size_t>(block);
        if (!g_initialised || idx >= g_table.size()) {
            // Before init, answer the way an empty world would. Failing the
            // other way (everything is surface) would put every column's
            // height at the top of the world, which reads as "mobs spawn in
            // the sky" rather than as an obvious missing-init bug.
            return false;
        }

        const uint8_t bits = g_table[idx];
        switch (type) {
            case HeightmapType::MotionBlockingNoLeaves:
                return (bits & kBitMotionBlockingNoLeaves) != 0;
            case HeightmapType::WorldSurface:
                return (bits & kBitWorldSurface) != 0;
            default:
                return false;
        }
    }

    // ── Anvil serialisation ────────────────────────────────────────────────
    //
    // MC SimpleBitStorage: 9 bits per entry, 7 entries per 64-bit long, top bit
    // of each long unused. Entries never straddle a long boundary.

    std::vector<int64_t> Heightmap::PackToLongs() const {
        std::vector<int64_t> longs(kLongCount, 0);

        for (int i = 0; i < kColumns; ++i) {
            const int longIndex = i / kEntriesPerLong;
            const int bitOffset = (i % kEntriesPerLong) * kBitsPerEntry;
            const uint64_t value = m_data[i] & ((1u << kBitsPerEntry) - 1u);

            longs[longIndex] |= static_cast<int64_t>(value << bitOffset);
        }

        return longs;
    }

    bool Heightmap::UnpackFromLongs(const std::vector<int64_t>& longs) {
        if (static_cast<int>(longs.size()) != kLongCount) return false;

        constexpr uint64_t kMask = (1u << kBitsPerEntry) - 1u;

        for (int i = 0; i < kColumns; ++i) {
            const int longIndex = i / kEntriesPerLong;
            const int bitOffset = (i % kEntriesPerLong) * kBitsPerEntry;
            const uint64_t raw = static_cast<uint64_t>(longs[longIndex]);

            uint16_t value = static_cast<uint16_t>((raw >> bitOffset) & kMask);

            // A stored value taller than the world means a file from a
            // different world height (or a corrupt one). Clamping keeps the
            // read total rather than letting it index past the column.
            const uint16_t maxValue = static_cast<uint16_t>(MaxY() - MinY() + 1);
            if (value > maxValue) value = maxValue;

            m_data[i] = value;
        }

        return true;
    }

} // namespace Game
