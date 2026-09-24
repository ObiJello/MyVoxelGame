// File: src/common/world/lighting/DataLayer.hpp
//
// MC `net.minecraft.world.level.chunk.DataLayer` — one light layer (sky OR
// block) of one 16x16x16 section: 4096 nibbles in 2048 bytes, index
// y << 8 | z << 4 | x, low nibble first. Byte-for-byte the Anvil
// "BlockLight"/"SkyLight" array and MC's wire format.
//
// Like MC's, a layer can be HOMOGENEOUS: no array at all, every nibble reads
// `defaultValue`. The sky above the terrain (15) and the block light of
// almost every section (0) never allocate, which is what keeps light storage
// for a whole loaded world in the tens of megabytes rather than the hundreds.
//
// Storage is shared, copy-on-write: copying a DataLayer shares the byte array
// and a write clones it first when anyone else still holds it. The client
// relies on that — a mesh job's region snapshot takes cheap copies of the
// live layers on the main thread and reads them on a worker while the next
// light packet replaces the live ones. The server never shares a layer across
// threads (the light engine and every reader run on the server thread; the
// saver reads under the chunk's content lock).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace Game::Lighting {

    class DataLayer {
    public:
        static constexpr int kSize = 2048;          // bytes
        static constexpr int kLayerSize = 128;      // bytes per y slice (16x16 nibbles)
        using Bytes = std::array<uint8_t, kSize>;

        DataLayer() = default;
        explicit DataLayer(int defaultValue) : m_default(static_cast<uint8_t>(defaultValue & 15)) {}

        // Takes 2048 bytes (the Anvil / wire array).
        static DataLayer FromBytes(const uint8_t* src) {
            DataLayer d;
            d.m_data = std::make_shared<Bytes>();
            std::memcpy(d.m_data->data(), src, kSize);
            return d;
        }

        static constexpr int Index(int x, int y, int z) { return (y << 8) | (z << 4) | x; }

        int Get(int x, int y, int z) const { return GetIndex(Index(x, y, z)); }
        int GetIndex(int index) const {
            if (!m_data) return m_default;
            return ((*m_data)[static_cast<size_t>(index >> 1)] >> ((index & 1) << 2)) & 15;
        }

        void Set(int x, int y, int z, int value) { SetIndex(Index(x, y, z), value); }
        void SetIndex(int index, int value) {
            // A homogeneous layer written with its own value stays
            // homogeneous — MC materialises the array here, which is harmless
            // there and a 2 KB allocation per untouched section here.
            if (!m_data && value == m_default) return;
            Bytes& data = Writable();
            const int shift = (index & 1) << 2;
            uint8_t& b = data[static_cast<size_t>(index >> 1)];
            b = static_cast<uint8_t>((b & ~(15 << shift)) | ((value & 15) << shift));
        }

        // MC DataLayer.fill: back to homogeneous.
        void Fill(int value) {
            m_default = static_cast<uint8_t>(value & 15);
            m_data.reset();
        }

        // MC isEmpty: homogeneous zero.
        bool IsEmpty() const { return !m_data && m_default == 0; }
        // MC isDefinitelyHomogenous.
        bool IsDefinitelyHomogeneous() const { return !m_data; }
        bool IsDefinitelyFilledWith(int value) const { return !m_data && m_default == value; }
        int  DefaultValue() const { return m_default; }

        // The 2048-byte array, or null when homogeneous.
        const uint8_t* RawData() const { return m_data ? m_data->data() : nullptr; }

        // MC getData: materialises a homogeneous layer into `out`.
        void CopyTo(uint8_t* out) const {
            if (m_data) {
                std::memcpy(out, m_data->data(), kSize);
            } else {
                std::memset(out, static_cast<int>((m_default << 4) | m_default), kSize);
            }
        }

        // Collapse an array whose nibbles are all equal back to homogeneous.
        // Returns true when it collapsed. O(2048).
        bool Compact() {
            if (!m_data) return false;
            const uint8_t first = (*m_data)[0];
            if ((first >> 4) != (first & 15)) return false;
            for (int i = 1; i < kSize; ++i) {
                if ((*m_data)[static_cast<size_t>(i)] != first) return false;
            }
            m_default = static_cast<uint8_t>(first & 15);
            m_data.reset();
            return true;
        }

        // True when both layers read the same value everywhere.
        bool ContentEquals(const DataLayer& o) const {
            if (!m_data && !o.m_data) return m_default == o.m_default;
            if (m_data && o.m_data) {
                return m_data == o.m_data ||
                       std::memcmp(m_data->data(), o.m_data->data(), kSize) == 0;
            }
            const DataLayer& arr = m_data ? *this : o;
            const int v = m_data ? o.m_default : m_default;
            const uint8_t packed = static_cast<uint8_t>((v << 4) | v);
            for (int i = 0; i < kSize; ++i) {
                if ((*arr.m_data)[static_cast<size_t>(i)] != packed) return false;
            }
            return true;
        }

        // An independent copy (never shares the array).
        DataLayer DeepCopy() const {
            DataLayer d;
            d.m_default = m_default;
            if (m_data) d.m_data = std::make_shared<Bytes>(*m_data);
            return d;
        }

        size_t HeapBytes() const { return m_data ? sizeof(Bytes) : 0; }

    private:
        Bytes& Writable() {
            if (!m_data) {
                m_data = std::make_shared<Bytes>();
                m_data->fill(static_cast<uint8_t>((m_default << 4) | m_default));
            } else if (m_data.use_count() > 1) {
                // Copy-on-write: someone (a mesh snapshot) still reads the old
                // bytes. A concurrent release by that holder can only make this
                // a harmless extra copy.
                m_data = std::make_shared<Bytes>(*m_data);
            }
            return *m_data;
        }

        std::shared_ptr<Bytes> m_data;
        uint8_t m_default = 0;
    };

} // namespace Game::Lighting
