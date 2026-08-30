// File: src/server/world/storage/anvil/PaletteCodec.cpp
#include "server/world/storage/anvil/PaletteCodec.hpp"

#include <algorithm>

namespace Game::Anvil {

    namespace {

        // Mth.ceillog2 — smallest e with (1 << e) >= n.
        int CeilLog2(size_t n) {
            int e = 0;
            while ((size_t{1} << e) < n) ++e;
            return e;
        }

        // First-seen palette building, without hashing.
        //
        // A chunk is 24 sections x 4096 voxels: ~98k lookups, where an
        // unordered_map costs milliseconds per chunk on the save path. A flat
        // value-indexed table costs one L1-resident load instead. It is reset
        // in O(distinct) through the touched list rather than by clearing
        // 32768 entries.
        struct SeenTable {
            std::vector<int32_t>  slot;      // value -> local id, -1 = unseen
            std::vector<uint32_t> touched;

            void Ensure(size_t valueSpace) {
                if (slot.size() < valueSpace) slot.resize(valueSpace, -1);
            }
            void Clear() {
                for (uint32_t v : touched) slot[v] = -1;
                touched.clear();
            }
        };

        SeenTable& Seen() {
            thread_local SeenTable table;
            return table;
        }

    } // namespace

    int DiskBitsFor(size_t paletteSize, bool blockStateTiers) {
        const int e = CeilLog2(paletteSize);
        if (e <= 0) return 0;                       // one entry: no data array
        if (blockStateTiers) return e <= 4 ? 4 : e; // 1..4 all round up to 4
        return e;                                   // biomes use every tier
    }

    void PackIndices(const std::vector<uint32_t>& indices, int bits,
                     std::vector<uint64_t>& out) {
        if (bits <= 0) { out.clear(); return; }

        const size_t perLong = static_cast<size_t>(64 / bits);
        out.assign(DiskWordCount(indices.size(), bits), 0);

        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t word = i / perLong;
            const int    shift = static_cast<int>((i % perLong) * static_cast<size_t>(bits));
            out[word] |= static_cast<uint64_t>(indices[i]) << shift;
        }
        // Nothing is written above bits*perLong in any word, so the padding
        // bits stay zero — which is what makes this layout non-straddling.
    }

    bool UnpackIndices(const std::vector<uint64_t>& data, size_t entryCount, int bits,
                       std::vector<uint32_t>& out) {
        if (bits <= 0) {
            if (!data.empty()) return false;
            out.assign(entryCount, 0);
            return true;
        }
        if (data.size() != DiskWordCount(entryCount, bits)) return false;

        const size_t   perLong = static_cast<size_t>(64 / bits);
        const uint64_t mask    = (bits >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);

        out.assign(entryCount, 0);
        for (size_t i = 0; i < entryCount; ++i) {
            const size_t word  = i / perLong;
            const int    shift = static_cast<int>((i % perLong) * static_cast<size_t>(bits));
            out[i] = static_cast<uint32_t>((data[word] >> shift) & mask);
        }
        return true;
    }

    DiskContainer PackForDisk(const PalettedContainer& container) {
        const PaletteStrategy& strategy = container.Strategy();
        const size_t entryCount = static_cast<size_t>(strategy.entryCount);

        // The value space the container promises to stay inside. Used to size
        // the lookup table AND to bound-check, because Get() on a global
        // container returns the raw storage word: a section decoded from a
        // corrupt or foreign file can yield something out of range, and that
        // would otherwise be an out-of-bounds WRITE into the table.
        const size_t valueSpace = (strategy.globalBits >= 32)
                                ? (size_t{1} << 31)
                                : (size_t{1} << strategy.globalBits);

        DiskContainer out;

        // Single-value fast path: vanilla writes a one-entry palette and omits
        // the data key entirely, which is most of a 384-block column.
        if (container.IsSingleValue()) {
            out.palette.assign(1, container.SingleValue());
            out.bits = 0;
            return out;
        }

        SeenTable& seen = Seen();
        seen.Ensure(valueSpace);

        std::vector<uint32_t> indices(entryCount);
        for (size_t i = 0; i < entryCount; ++i) {
            uint32_t v = container.Get(i);
            if (v >= valueSpace) v = 0;             // corrupt input -> the default value
            int32_t& id = seen.slot[v];
            if (id < 0) {
                id = static_cast<int32_t>(out.palette.size());
                out.palette.push_back(v);
                seen.touched.push_back(v);
            }
            indices[i] = static_cast<uint32_t>(id);
        }
        seen.Clear();

        out.bits = DiskBitsFor(out.palette.size(), strategy.blockStateTiers);
        PackIndices(indices, out.bits, out.data);
        return out;
    }

    bool UnpackFromDisk(const std::vector<uint32_t>& palette,
                        const std::vector<uint64_t>& data,
                        const PaletteStrategy& strategy,
                        PalettedContainer& out,
                        std::string& error) {
        if (palette.empty()) {
            error = "palette is empty";               // vanilla: SingleValuePalette needs >= 1
            return false;
        }

        const size_t entryCount = static_cast<size_t>(strategy.entryCount);
        const int    bits       = DiskBitsFor(palette.size(), strategy.blockStateTiers);

        if (bits > 0 && data.empty()) {
            error = "missing values for non-zero storage";
            return false;
        }
        if (bits == 0 && !data.empty()) {
            // Not fatal in vanilla, but it means the writer and the palette
            // disagree, so the indices cannot be trusted.
            error = "single-entry palette carries a data array";
            return false;
        }

        std::vector<uint32_t> indices;
        if (!UnpackIndices(data, entryCount, bits, indices)) {
            error = "packed data is " + std::to_string(data.size()) + " longs, expected "
                  + std::to_string(DiskWordCount(entryCount, bits))
                  + " for " + std::to_string(palette.size()) + " palette entries at "
                  + std::to_string(bits) + " bits";
            return false;
        }

        // BuildFrom re-derives the ENGINE's configuration from the value count
        // and validates every index, so a palette index past the end is
        // refused here rather than becoming an unchecked read later.
        out = PalettedContainer(strategy, palette[0]);
        if (!out.BuildFrom(palette, indices)) {
            error = "palette index out of range";
            return false;
        }
        return true;
    }

} // namespace Game::Anvil
