// File: src/common/world/chunk/PalettedContainer.hpp
//
// Port of net.minecraft.world.level.chunk.PalettedContainer + Strategy.
//
// Stores `entryCount` values as indices into a palette, with the index width
// chosen from how many DISTINCT values are present. The palette grows itself:
// a section of plain stone costs a control block and no backing array at all,
// and typical terrain sits at 4 bits.
//
// This is the storage AND the wire format, which is the whole point of the
// port. MC's LevelChunkSection.write is `writeShort(count)` followed by the two
// containers verbatim, so sending a chunk is a memcpy of data that is already
// packed rather than a re-encode pass over 4096 voxels.
//
// Values here are plain uint32_t ids — flat block-state ids from
// Game::BlockStateIds, or biome ids. MC's container is templated over the value
// type and pairs with an IdMap; ours is not, because both of our value spaces
// are already dense integers. That removes the indirection MC needs without
// changing the format.
//
// CONTRACT: every value stored must be < (1 << strategy.globalBits). Once the
// palette overflows its tiers the container switches to a global palette where
// the VALUE IS THE INDEX, so a value wider than globalBits is silently
// truncated to it. MC cannot hit this — its global palette is an IdMap and ids
// are dense by construction — so the guard below is ours, and it is a debug
// assert rather than a branch on the write path.
#pragma once

#include "BitStorage.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Game {

    // MC Strategy: how many bits to use for a given number of distinct values,
    // and which palette implementation backs it.
    //
    // The two configurations are verbatim from Strategy.createForBlockStates
    // (Strategy.java:33) and createForBiomes (:68). Note block states skip
    // 1-3 bits entirely — anything from 1 to 4 distinct values is stored at 4
    // bits — while biomes use every tier. That asymmetry is MC's, and it is
    // load-bearing for wire compatibility, not an optimisation to tidy up.
    struct PaletteStrategy {
        int  entryCount    = 4096;   // 1 << (bitsPerAxis * 3)
        int  bitsPerAxis   = 4;
        int  globalBits    = 15;     // ceillog2(total distinct values)
        bool blockStateTiers = true; // true: 1-4 -> 4. false: 1,2,3 distinct.

        // MC getConfigurationForBitCount. Returns the storage width to use for
        // a palette that must hold `entryBits` worth of distinct values, and
        // whether that width means "global" (value is its own index).
        int WidthFor(int entryBits) const {
            if (entryBits <= 0) return 0;
            if (blockStateTiers) {
                if (entryBits <= 4) return 4;
                if (entryBits <= 8) return entryBits;
                return globalBits;
            }
            if (entryBits <= 3) return entryBits;
            return globalBits;
        }

        bool IsGlobal(int width) const { return width >= globalBits && width > 8; }

        static PaletteStrategy ForBlockStates(int globalBits) {
            PaletteStrategy s;
            s.entryCount = 4096; s.bitsPerAxis = 4;
            s.globalBits = globalBits; s.blockStateTiers = true;
            return s;
        }
        static PaletteStrategy ForBiomes(int globalBits) {
            PaletteStrategy s;
            s.entryCount = 64; s.bitsPerAxis = 2;
            s.globalBits = globalBits; s.blockStateTiers = false;
            return s;
        }
    };

    class PalettedContainer {
    public:
        PalettedContainer() = default;

        // A container holding `fill` everywhere: MC's constructor seeds the
        // palette with the default value at zero bits, so an untouched section
        // has no backing array.
        PalettedContainer(const PaletteStrategy& strategy, uint32_t fill)
            : m_strategy(strategy) {
            m_palette.assign(1, fill);
            m_storage = BitStorage(0, static_cast<size_t>(strategy.entryCount));
        }

        uint32_t Get(size_t index) const {
            if (m_global) return m_storage.Get(index);
            const uint32_t id = m_storage.Get(index);
            return id < m_palette.size() ? m_palette[id] : m_palette[0];
        }

        // Returns the previous value, like MC getAndSet — the caller needs it
        // to keep per-section censuses current.
        uint32_t GetAndSet(size_t index, uint32_t value) {
            assert(m_strategy.globalBits >= 32 ||
                   value < (uint32_t{1} << m_strategy.globalBits));
            const uint32_t previous = Get(index);
            if (previous == value) return previous;
            m_storage.Set(index, IdFor(value));
            return previous;
        }

        void Set(size_t index, uint32_t value) { (void)GetAndSet(index, value); }

        // MC PalettedContainer.count — distinct values with their occurrence
        // counts, without walking 4096 entries when the palette is small.
        template <class Fn>
        void ForEachValue(Fn fn) const {
            if (m_global) {
                std::unordered_map<uint32_t, int> counts;
                for (size_t i = 0; i < m_storage.Size(); ++i) ++counts[m_storage.Get(i)];
                for (const auto& [v, n] : counts) fn(v, n);
                return;
            }
            if (m_palette.size() == 1) {
                fn(m_palette[0], m_strategy.entryCount);
                return;
            }
            std::vector<int> counts(m_palette.size(), 0);
            for (size_t i = 0; i < m_storage.Size(); ++i) ++counts[m_storage.Get(i)];
            for (size_t i = 0; i < m_palette.size(); ++i) {
                if (counts[i] > 0) fn(m_palette[i], counts[i]);
            }
        }

        bool IsSingleValue() const { return !m_global && m_palette.size() == 1; }
        uint32_t SingleValue() const { return m_palette[0]; }

        // ── Bulk construction (no MC equivalent) ────────────────────────────
        //
        // MC never needs this: its generator writes into the container the
        // world keeps, so there is nothing to import. Ours receives finished
        // sections from the vendored terrain library, which has its own
        // palette — see MyTerrainGenerator::ConvertLibChunk. Building the
        // palette in one shot and adopting the indices avoids re-deriving it
        // one voxel at a time.
        //
        // `values` are the distinct values in index order; `indices` maps each
        // entry to a position in that list. Fails (returns false) if any index
        // is out of range, so a bad remap is caught rather than stored.
        bool BuildFrom(const std::vector<uint32_t>& values,
                       const std::vector<uint32_t>& indices) {
            if (values.empty() || indices.size() != static_cast<size_t>(m_strategy.entryCount)) {
                return false;
            }
            for (uint32_t idx : indices) {
                if (idx >= values.size()) return false;
            }

            int entryBits = 0;
            while ((1u << entryBits) < values.size()) ++entryBits;
            const int width = m_strategy.WidthFor(entryBits);

            m_global  = m_strategy.IsGlobal(width);
            m_storage = BitStorage(width, static_cast<size_t>(m_strategy.entryCount));

            if (m_global) {
                m_palette.clear();
                for (size_t i = 0; i < indices.size(); ++i) {
                    m_storage.Set(i, values[indices[i]]);
                }
            } else {
                m_palette = values;
                m_lookup.clear();
                if (width > 4) {
                    for (uint32_t i = 0; i < m_palette.size(); ++i) m_lookup[m_palette[i]] = i;
                }
                for (size_t i = 0; i < indices.size(); ++i) {
                    m_storage.Set(i, indices[i]);
                }
            }
            return true;
        }

        // ── Wire ────────────────────────────────────────────────────────────
        // MC PalettedContainer.write: bits byte, then palette (absent when
        // global), then the raw words with no length prefix.
        int  StorageBits() const { return m_storage.Bits(); }
        bool IsGlobalPalette() const { return m_global; }
        const std::vector<uint32_t>& Palette() const { return m_palette; }
        const std::vector<uint64_t>& RawWords() const { return m_storage.Raw(); }

        // Decode counterpart. `bits == 0` means a single-value palette.
        bool ReadFrom(int bits, std::vector<uint32_t>&& palette,
                      std::vector<uint64_t>&& words) {
            m_global = m_strategy.IsGlobal(bits);
            m_storage = BitStorage(bits, static_cast<size_t>(m_strategy.entryCount));
            if (m_global) {
                m_palette.clear();
            } else {
                if (palette.empty()) return false;
                m_palette = std::move(palette);
                m_lookup.clear();
                if (bits > 4) {
                    for (uint32_t i = 0; i < m_palette.size(); ++i) m_lookup[m_palette[i]] = i;
                }
            }
            if (bits == 0) return words.empty();
            return m_storage.Adopt(std::move(words));
        }

        const PaletteStrategy& Strategy() const { return m_strategy; }

    private:
        // MC Palette.idFor + PalettedContainer.onResize: find the value, or add
        // it, growing to the next configuration when the current width is full.
        uint32_t IdFor(uint32_t value) {
            if (m_global) return value;

            if (m_storage.Bits() > 4) {
                auto it = m_lookup.find(value);
                if (it != m_lookup.end()) return it->second;
            } else {
                // Linear scan, as MC's LinearPalette does — at 4 bits there are
                // at most 16 entries and a hash lookup would cost more.
                for (uint32_t i = 0; i < m_palette.size(); ++i) {
                    if (m_palette[i] == value) return i;
                }
            }

            const size_t capacity =
                (m_storage.Bits() <= 0) ? 1u : (size_t{1} << m_storage.Bits());
            if (m_palette.size() < capacity) {
                const uint32_t id = static_cast<uint32_t>(m_palette.size());
                m_palette.push_back(value);
                if (m_storage.Bits() > 4) m_lookup[value] = id;
                return id;
            }
            return Grow(value);
        }

        // MC onResize: build the next configuration up and copy every entry
        // through the old palette into the new one.
        uint32_t Grow(uint32_t value) {
            int entryBits = 0;
            while ((1u << entryBits) < (m_palette.size() + 1)) ++entryBits;
            const int width = m_strategy.WidthFor(entryBits);

            const BitStorage old = m_storage;
            const std::vector<uint32_t> oldPalette = m_palette;
            const bool wasGlobal = m_global;

            m_global  = m_strategy.IsGlobal(width);
            m_storage = BitStorage(width, static_cast<size_t>(m_strategy.entryCount));

            if (m_global) {
                m_palette.clear();
                m_lookup.clear();
                for (size_t i = 0; i < m_storage.Size(); ++i) {
                    const uint32_t oldId = old.Get(i);
                    m_storage.Set(i, wasGlobal ? oldId
                                              : (oldId < oldPalette.size() ? oldPalette[oldId]
                                                                           : oldPalette[0]));
                }
                return value;
            }

            // Same palette, wider indices — the ids do not move, so the entries
            // only need re-packing, not re-resolving.
            m_lookup.clear();
            if (width > 4) {
                for (uint32_t i = 0; i < m_palette.size(); ++i) m_lookup[m_palette[i]] = i;
            }
            for (size_t i = 0; i < m_storage.Size(); ++i) {
                m_storage.Set(i, old.Get(i));
            }

            const uint32_t id = static_cast<uint32_t>(m_palette.size());
            m_palette.push_back(value);
            if (width > 4) m_lookup[value] = id;
            return id;
        }

        PaletteStrategy       m_strategy = PaletteStrategy::ForBlockStates(15);
        std::vector<uint32_t> m_palette;
        // Only maintained above 4 bits — MC switches from LinearPalette to
        // HashMapPalette at exactly the same point.
        std::unordered_map<uint32_t, uint32_t> m_lookup;
        BitStorage            m_storage;
        bool                  m_global = false;
    };

} // namespace Game
