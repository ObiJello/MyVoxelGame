// File: src/common/world/chunk/BitStorage.hpp
//
// Port of net.minecraft.util.SimpleBitStorage.
//
// A fixed-size array of `size` values, each `bits` wide, packed into 64-bit
// words. The one property that matters and that MC changed in 1.16: a value
// NEVER spans a word boundary. `valuesPerLong = 64 / bits`, and the remaining
// 64 % bits high bits of each word are simply left unused.
//
// That wastage is deliberate — it makes get/set a single shift and mask with no
// carry handling, and it is what lets the wire format be a straight memcpy of
// the backing array on both sides. Our existing chunk decoder already assumes
// exactly this layout (ClientChunkManager's `blocksPerLong = 64 / bitsPerBlock`),
// so it is also what keeps the two halves compatible.
//
// MC uses a magic-number reciprocal table to turn `index / valuesPerLong` into
// a multiply. That is not reproduced: valuesPerLong is a runtime value but the
// division is not on any measured hot path here (the hot paths — meshing and
// chunk conversion — walk the storage sequentially or copy it wholesale).
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace Game {

    class BitStorage {
    public:
        BitStorage() = default;

        BitStorage(int bits, size_t size)
            : m_bits(bits), m_size(size) {
            if (bits <= 0) {
                // MC models this as a separate ZeroBitStorage: every value is
                // index 0, and there is no backing array at all.
                m_valuesPerLong = 0;
                m_mask = 0;
                return;
            }
            m_valuesPerLong = 64 / bits;
            m_mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
            m_data.assign((size + m_valuesPerLong - 1) / m_valuesPerLong, 0);
        }

        // Adopt an existing backing array (wire/decode path). Returns false if
        // the length does not match what `bits` and `size` require, so a
        // malformed packet is rejected rather than read out of bounds.
        bool Adopt(std::vector<uint64_t>&& data) {
            if (data.size() != ExpectedWords()) return false;
            m_data = std::move(data);
            return true;
        }

        uint32_t Get(size_t index) const {
            if (m_bits <= 0) return 0;
            const size_t cell = index / static_cast<size_t>(m_valuesPerLong);
            const int bitIndex =
                static_cast<int>(index - cell * static_cast<size_t>(m_valuesPerLong)) * m_bits;
            return static_cast<uint32_t>((m_data[cell] >> bitIndex) & m_mask);
        }

        void Set(size_t index, uint32_t value) {
            if (m_bits <= 0) return;   // single-value storage: nothing to write
            const size_t cell = index / static_cast<size_t>(m_valuesPerLong);
            const int bitIndex =
                static_cast<int>(index - cell * static_cast<size_t>(m_valuesPerLong)) * m_bits;
            m_data[cell] = (m_data[cell] & ~(m_mask << bitIndex)) |
                           ((static_cast<uint64_t>(value) & m_mask) << bitIndex);
        }

        int    Bits() const { return m_bits; }
        size_t Size() const { return m_size; }

        // Raw backing words — what goes on the wire verbatim (MC
        // writeFixedSizeLongArray, which carries no length prefix because the
        // reader derives the count from `bits`).
        const std::vector<uint64_t>& Raw() const { return m_data; }
        std::vector<uint64_t>&       Raw()       { return m_data; }

        size_t ExpectedWords() const {
            if (m_bits <= 0) return 0;
            return (m_size + static_cast<size_t>(m_valuesPerLong) - 1) /
                   static_cast<size_t>(m_valuesPerLong);
        }

    private:
        int      m_bits          = 0;
        size_t   m_size          = 0;
        int      m_valuesPerLong = 0;
        uint64_t m_mask          = 0;
        std::vector<uint64_t> m_data;
    };

} // namespace Game
