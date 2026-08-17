// File: src/common/world/chunk/ChunkSection.hpp
#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include "PalettedContainer.hpp"
#include "../math/WorldMath.hpp"
#include "../block/Blocks.hpp"
#include "../block/BlockStateIds.hpp"
#include "../biome/Biomes.hpp"

namespace Game {

    // Which BlockIDs want random ticks. Filled once by BlockRegistry::Init from
    // the `randomTick` callbacks in BlockGrowth.cpp; all false until then, so a
    // section built before initialisation simply counts nothing (nothing is
    // ticking that early either).
    //
    // A flat array rather than a BlockRegistry call because ChunkSection::Set is
    // the hottest write in the program — terrain generation runs it ~98k times
    // per chunk — and it lives in a header that deliberately knows nothing about
    // the model/mining/behaviour half of the registry. One L1-resident byte read
    // is a price worth paying here; a registry lookup would not be.
    extern std::array<bool, static_cast<size_t>(BlockID::Count)> g_blockRandomlyTicks;

    inline bool BlockRandomlyTicks(uint16_t rawId) {
        return rawId < g_blockRandomlyTicks.size() && g_blockRandomlyTicks[rawId];
    }

    // A single 16×16×16 sub‐chunk (also called a “section”).
    // Internally we store exactly 4096 block indices (one uint16_t per local voxel),
    // plus a palette (to be used later for PalettedStorage if desired).
    //
    // Memory layout: 
    //   - blocks[y][z][x] flattened via LocalIndex(x,y,z), where each runs 0..15.
    //
    // Get/Set methods do no bounds-checking in release; you may add asserts if you like.
    class ChunkSection {
    public:
        static constexpr int SIZE = 16;        // extent along each axis
        static constexpr int TOTAL = SIZE * SIZE * SIZE; // 4096

        // ── Storage (MC LevelChunkSection.states) ───────────────────────────
        //
        // One PalettedContainer of FLAT BLOCK-STATE IDS, exactly as MC stores a
        // section. The (BlockID, stateIndex) pair this class still speaks is
        // packed into that id on write and unpacked on read
        // (Game::BlockStateIds) — so every caller keeps its existing vocabulary
        // while the storage becomes MC's.
        //
        // This replaced a flat 4096-entry uint16_t array plus a lazily
        // allocated 4096-byte state plane: 8 KB per section unconditionally,
        // 12 KB for any section carrying state. Ordinary terrain now sits at
        // 4 bits per voxel, and a section of one block type — most of a
        // 384-block column — costs a one-entry palette and no backing array.
        //
        // It is also the wire format. MC's LevelChunkSection.write is
        // writeShort(count) followed by this container verbatim, so sending a
        // chunk stops being a re-encode pass over 4096 voxels.
    private:
        PalettedContainer m_states{
            PaletteStrategy::ForBlockStates(BlockStateIds::Bits()),
            BlockStateIds::Pack(BlockID::Air, 0)
        };

        PalettedContainer m_biomes{ PaletteStrategy::ForBiomes(16), kFallbackBiomeId };

        // Cached "does any voxel here carry a non-default state". Recomputed
        // when a non-default state is written, never scanned per voxel — the
        // chunk encoder asks this once per section.
        bool m_hasStates = false;

    public:
        ChunkSection() = default;

        // Read-only access to the container, for the paths that speak the
        // packed representation directly: the chunk encoder, the mesh section
        // copy, and the terrain-library conversion.
        const PalettedContainer& States() const { return m_states; }
        PalettedContainer&       States()       { return m_states; }

        // ── Biomes (MC LevelChunkSection.biomes) ────────────────────────────
        //
        // One id per 4x4x4 cell, 64 per section, in its own container — exactly
        // where MC keeps them, and written to the wire right after the states
        // (LevelChunkSection.write). This replaced a flat 1536-entry array on
        // the CHUNK: 3 KB per chunk that went out raw as one WriteShort per
        // cell, where a section spanning one biome now costs a single-entry
        // palette and no backing array.
        //
        // Note the tiers differ from block states: biomes use 1, 2 and 3 bits
        // where block states jump straight to 4 (MC Strategy.createForBiomes vs
        // createForBlockStates).
        static constexpr int BIOME_AXIS  = 4;
        static constexpr int BIOME_COUNT = BIOME_AXIS * BIOME_AXIS * BIOME_AXIS;

        static constexpr size_t BiomeIndex(int qx, int qy, int qz) {
            return (static_cast<size_t>(qy) * BIOME_AXIS + static_cast<size_t>(qz)) * BIOME_AXIS
                   + static_cast<size_t>(qx);
        }

        inline uint16_t GetBiome(int qx, int qy, int qz) const {
            return static_cast<uint16_t>(m_biomes.Get(BiomeIndex(qx, qy, qz)));
        }
        inline void SetBiome(int qx, int qy, int qz, uint16_t biomeId) {
            m_biomes.Set(BiomeIndex(qx, qy, qz), biomeId);
        }

        const PalettedContainer& Biomes() const { return m_biomes; }
        void AdoptBiomes(PalettedContainer&& biomes) { m_biomes = std::move(biomes); }

        // A biome container matching this section's configuration — used by
        // the wire decoder, which builds one before adopting it.
        static PalettedContainer MakeBiomeContainer() {
            return PalettedContainer(PaletteStrategy::ForBiomes(16), kFallbackBiomeId);
        }

        static PaletteStrategy BiomeStrategy() {
            // Biome ids are a small dense space; 16 bits covers any registry
            // this engine will carry and keeps the global fallback exact.
            return PaletteStrategy::ForBiomes(16);
        }

        // Replace the whole container (terrain-library conversion, wire decode).
        // Recomputes both censuses, since neither can be derived from the old
        // contents.
        void AdoptStates(PalettedContainer&& states) {
            m_states = std::move(states);
            RecountRandomTicking();
        }

        // State index at local (x,y,z). Returns 0 (the block's default state)
        // when nothing non-default has been written here.
        inline uint8_t GetState(int x, int y, int z) const {
            return BlockStateIds::Unpack(
                       m_states.Get(static_cast<size_t>(Math::LocalIndex(x, y, z)))).state;
        }

        inline void SetState(int x, int y, int z, uint8_t stateIndex) {
            const size_t i = static_cast<size_t>(Math::LocalIndex(x, y, z));
            const BlockStateRef cur = BlockStateIds::Unpack(m_states.Get(i));
            if (cur.state == stateIndex) return;
            m_states.Set(i, BlockStateIds::Pack(cur.id, stateIndex));
            if (stateIndex != 0) m_hasStates = true;
        }

        // True when this section carries any per-voxel state at all. Lets the
        // chunk serialiser skip work for ordinary terrain.
        //
        // Conservative by design: it can stay true after the last stateful
        // block is removed, because clearing it would need a full scan on every
        // write. A false positive costs a little extra encoding, never
        // correctness.
        inline bool HasStates() const { return m_hasStates; }

        // MC LevelChunkSection.hasOnlyAir — the single-value fast path the
        // encoder, the mesh copy and the conversion all branch on.
        inline bool IsAllAir() const {
            return m_states.IsSingleValue() &&
                   m_states.SingleValue() == BlockStateIds::Pack(BlockID::Air, 0);
        }

        // Retrieve the BlockID at local (x,y,z) in [0..15].
        // Returns a raw uint16_t; static_cast<BlockID>(...) when needed.
        inline uint16_t Get(int x, int y, int z) const {
            return static_cast<uint16_t>(
                BlockStateIds::Unpack(
                    m_states.Get(static_cast<size_t>(Math::LocalIndex(x, y, z)))).id);
        }

        inline BlockID GetBlockID(int x, int y, int z) const {
            return static_cast<BlockID>(Get(x, y, z));
        }

        // Set the block at local (x,y,z), PRESERVING the existing state index —
        // which is what the old split blocks[]/states[] arrays did, since a
        // write to one never touched the other. Pack clamps the index against
        // the new block's state count, so a block with fewer states cannot
        // inherit an index it does not have.
        inline void Set(int x, int y, int z, uint16_t rawID) {
            const size_t i = static_cast<size_t>(Math::LocalIndex(x, y, z));
            const BlockStateRef cur = BlockStateIds::Unpack(m_states.Get(i));
            if (static_cast<uint16_t>(cur.id) == rawID) return;

            const uint32_t previous =
                m_states.GetAndSet(i, BlockStateIds::Pack(static_cast<BlockID>(rawID), cur.state));

            // Keep the random-tick census current — MC does exactly this in
            // LevelChunkSection.setBlockState, which likewise reads the old
            // value back from the container to adjust its counts.
            const uint16_t prevRaw = static_cast<uint16_t>(BlockStateIds::Unpack(previous).id);
            if (BlockRandomlyTicks(prevRaw)) --randomTickingCount;
            if (BlockRandomlyTicks(rawID))   ++randomTickingCount;
        }

        inline void Set(int x, int y, int z, BlockID id) {
            Set(x, y, z, static_cast<uint16_t>(id));
        }

        // Set block and state together — MC's actual shape
        // (setBlockState takes one BlockState), and one container write instead
        // of two.
        inline void SetBlockState(int x, int y, int z, BlockID id, uint8_t stateIndex) {
            const size_t i = static_cast<size_t>(Math::LocalIndex(x, y, z));
            const uint32_t previous = m_states.GetAndSet(i, BlockStateIds::Pack(id, stateIndex));
            const uint16_t prevRaw = static_cast<uint16_t>(BlockStateIds::Unpack(previous).id);
            const uint16_t rawID   = static_cast<uint16_t>(id);
            if (prevRaw != rawID) {
                if (BlockRandomlyTicks(prevRaw)) --randomTickingCount;
                if (BlockRandomlyTicks(rawID))   ++randomTickingCount;
            }
            if (stateIndex != 0) m_hasStates = true;
        }

        // ── Random ticking (MC LevelChunkSection.isRandomlyTicking) ─────────
        //
        // How many blocks in this section want random ticks. Maintained on
        // every write above so the server's tick loop can skip an entire
        // section with one comparison — which is nearly all of them, since
        // ordinary terrain (stone, dirt, air) never random-ticks.
        //
        // Without this the loop would sample three positions in all 24 sections
        // of every simulated chunk, which is tens of thousands of scattered
        // reads per tick spent almost entirely on stone.
        uint16_t randomTickingCount = 0;

        inline bool IsRandomlyTicking() const { return randomTickingCount > 0; }

        // Rebuild the census from scratch. Needed by any path that fills
        // `blocks` in bulk instead of going through Set() — today just the
        // chunk loader's memcpy in AsyncChunkSaver. Cheap (one linear pass over
        // 4096 uint16_t) and idempotent, so call it after any such fill rather
        // than trying to reason about whether it was needed.
        inline void RecountRandomTicking() {
            uint32_t n = 0;
            bool anyState = false;
            // One pass over the DISTINCT values, not 4096 voxels — the palette
            // already knows how many of each there are (MC recalcBlockCounts
            // uses its container's count() the same way).
            m_states.ForEachValue([&](uint32_t stateId, int count) {
                const BlockStateRef ref = BlockStateIds::Unpack(stateId);
                if (BlockRandomlyTicks(static_cast<uint16_t>(ref.id))) {
                    n += static_cast<uint32_t>(count);
                }
                if (ref.state != 0) anyState = true;
            });
            randomTickingCount = static_cast<uint16_t>(n > 0xFFFFu ? 0xFFFFu : n);
            m_hasStates = anyState;
        }
    };

} // namespace Game
