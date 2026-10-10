// File: src/common/world/lighting/ChunkLight.hpp
//
// A chunk's light: MC keeps DataLayers in the level-wide light engine maps;
// here they live on the chunk itself (Game::Chunk::light), on the server and
// on the client alike, so every chunk carries exactly what the Anvil section
// list and ClientboundLightUpdatePacketData carry.
//
// LIGHT SECTIONS. MC stores light one section past each end of the world
// (LevelLightEngine.getMinLightSection = minSection - 1, count = sections + 2)
// so sky light has a layer above the build limit and light from the lowest
// blocks has somewhere to go. With 24 block sections that is 26 light
// sections, light index = block section index + 1:
//     light 0      -> section y -5 (world y -80..-65)
//     light 1..24  -> the world's sections (-4..19)
//     light 25     -> section y 20 (world y 320..335)
//
// Every light section of a lit chunk has a layer for both lights (MC stores
// sky data only up to the column's top non-empty section and derives the
// rest; homogeneous layers make storing all of them free, and the values are
// the same everywhere a surface can be).
//
// COLOURED LIGHT (engine). Three more layers per light section — red, green
// and blue block light — each propagated exactly as block light is, from the
// state's channel emission (BlockLightProperties::ChannelEmission). Block
// light stays the gameplay value (spawning, crops, MC parity) and is always
// the maximum of the three. They are for drawing only: the mesher tints a
// face by them. Where a channel reads the same as block light — everywhere
// no coloured emitter reaches — it SHARES the block layer's storage
// (ShareWhiteChannels; DataLayer copy-on-write keeps the two apart the
// moment either is written), so a world without coloured light carries no
// extra bytes, on disk (ChunkSerializer) or on the wire (NetCodec) either.
#pragma once

#include "common/core/Config.hpp"
#include "common/world/lighting/ChunkSkyLightSources.hpp"
#include "common/world/lighting/DataLayer.hpp"
#include "common/world/math/WorldMath.hpp"

#include <array>

namespace Game::Lighting {

    // Red / Green / Blue are the coloured block-light channels (see the
    // header note); LightChannel order.
    enum class LightLayer : uint8_t { Sky = 0, Block = 1, Red = 2, Green = 3, Blue = 4 };
    inline constexpr LightLayer ChannelLayer(int channel) {
        return static_cast<LightLayer>(static_cast<int>(LightLayer::Red) + channel);
    }

    inline constexpr int kLightSectionCount = Math::SECTIONS_PER_CHUNK + 2;     // 26
    // World section coordinate (y >> 4) of light index 0.
    inline constexpr int kMinLightSectionY = (Config::MinY >> 4) - 1;         // -5
    inline constexpr int kMaxLightSectionY = kMinLightSectionY + kLightSectionCount - 1;   // 20

    // Light index for world Y, or -1 / kLightSectionCount when outside.
    inline constexpr int LightIndexForY(int worldY) {
        return (worldY >> 4) - kMinLightSectionY;
    }

    struct ChunkLight {
        std::array<DataLayer, kLightSectionCount> sky{};
        std::array<DataLayer, kLightSectionCount> block{};
        // Red, green, blue (LightChannel order) — see the header note.
        std::array<std::array<DataLayer, kLightSectionCount>, 3> channel{};
        ChunkSkyLightSources skySources;

        // MC ChunkAccess.isLightCorrect / the Anvil "isLightOn" flag: the
        // layers above are a finished lighting of this chunk (its own sources;
        // the light engine reconciles borders with neighbours when the chunk
        // is registered). False = no usable light: a chunk the old engine
        // saved, a Minecraft world without isLightOn, a client chunk whose
        // packet carried none.
        bool lightCorrect = false;

        // The blocks in this chunk whose light is coloured (server; set by
        // LightChunk / LevelLightManager::Register, kept by OnBlockChanged;
        // not saved). Where none is near, the channels are block light and
        // the level engine does not propagate them (LevelLightManager).
        int coloredEmitters = 0;

        DataLayer& Layer(LightLayer l, int index) {
            const size_t i = static_cast<size_t>(index);
            switch (l) {
                case LightLayer::Sky:   return sky[i];
                case LightLayer::Block: return block[i];
                default:                return channel[static_cast<size_t>(l) - static_cast<size_t>(LightLayer::Red)][i];
            }
        }
        const DataLayer& Layer(LightLayer l, int index) const {
            return const_cast<ChunkLight*>(this)->Layer(l, index);
        }

        // Light index `li` has light that is not white: a channel differs from
        // block light. Exact after ShareWhiteChannels.
        bool HasColor(int li) const {
            const size_t i = static_cast<size_t>(li);
            for (const auto& c : channel) {
                if (!c[i].SharesWith(block[i])) return true;
            }
            return false;
        }

        // Every channel layer that reads the same as block light takes the
        // block layer's storage (see the header note). O(2048) per differing
        // pair; the shared case is a pointer compare.
        void ShareWhiteChannels(int li) {
            const size_t i = static_cast<size_t>(li);
            for (auto& c : channel) {
                if (!c[i].SharesWith(block[i]) && c[i].ContentEquals(block[i])) c[i] = block[i];
            }
        }
        void ShareWhiteChannels() {
            for (int li = 0; li < kLightSectionCount; ++li) ShareWhiteChannels(li);
        }

        // Raw level at chunk-local (x, z) and world y. Sky above the light
        // range reads 15; below it, the bottom layer's lowest row (MC's
        // SkyLightSectionStorage walks up to the first stored layer). Block
        // light outside the range reads 0.
        int Get(LightLayer l, int localX, int worldY, int localZ) const {
            const int li = LightIndexForY(worldY);
            if (li >= kLightSectionCount) return l == LightLayer::Sky ? 15 : 0;   // block and channels: 0
            if (li < 0) return l == LightLayer::Sky ? sky[0].Get(localX, 0, localZ) : 0;
            return Layer(l, li).Get(localX, worldY & 15, localZ);
        }

        void Reset() {
            for (auto& d : sky) d = DataLayer();
            for (auto& d : block) d = DataLayer();
            for (auto& c : channel) for (auto& d : c) d = DataLayer();
            skySources = ChunkSkyLightSources();
            lightCorrect = false;
            coloredEmitters = 0;
        }

        // Drop arrays whose nibbles are all equal (after a full relight), and
        // share every white channel with block light.
        void Compact() {
            for (auto& d : sky) d.Compact();
            for (auto& d : block) d.Compact();
            for (auto& c : channel) for (auto& d : c) d.Compact();
            ShareWhiteChannels();
        }

        // A channel sharing block light's array is not counted twice.
        size_t HeapBytes() const {
            size_t n = 0;
            for (const auto& d : sky) n += d.HeapBytes();
            for (const auto& d : block) n += d.HeapBytes();
            for (const auto& c : channel) {
                for (size_t i = 0; i < c.size(); ++i) {
                    if (!c[i].SharesWith(block[i])) n += c[i].HeapBytes();
                }
            }
            return n;
        }
    };

} // namespace Game::Lighting
