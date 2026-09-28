// File: src/client/renderer/particle/BlockParticleTint.cpp
#include "BlockParticleTint.hpp"

#include "../mesh/Mesher.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneWire.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>

namespace Render {

    namespace {

        enum class Channel { Grass, Foliage, DryFoliage, Water };

        uint32_t ChannelColor(Channel c, Game::BiomeId biome, int x, int z) {
            switch (c) {
                case Channel::Grass:      return Game::BiomeRegistry::GrassColor(biome, x, z);
                case Channel::Foliage:    return Game::BiomeRegistry::FoliageColor(biome);
                case Channel::DryFoliage: return Game::BiomeRegistry::DryFoliageColor(biome);
                case Channel::Water:      return Game::BiomeRegistry::WaterColor(biome);
            }
            return 0xFFFFFF;
        }

        // MC ClientLevel.calculateBlockTint.
        uint32_t BlendedColor(const Game::IBlockAccess& blocks, const glm::ivec3& pos, Channel c) {
            const int r = Mesher::GetMeshOptions().biomeBlendRadius;
            if (r == 0) return ChannelColor(c, blocks.GetBiome(pos.x, pos.y, pos.z), pos.x, pos.z);
            int sr = 0, sg = 0, sb = 0, count = 0;
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    const int x = pos.x + dx, z = pos.z + dz;
                    const uint32_t col = ChannelColor(c, blocks.GetBiome(x, pos.y, z), x, z);
                    sr += static_cast<int>((col >> 16) & 255);
                    sg += static_cast<int>((col >> 8) & 255);
                    sb += static_cast<int>(col & 255);
                    ++count;
                }
            }
            return (static_cast<uint32_t>(sr / count & 255) << 16) |
                   (static_cast<uint32_t>(sg / count & 255) << 8) |
                   static_cast<uint32_t>(sb / count & 255);
        }

    } // namespace

    int64_t BlockTintColor(Game::BlockState state, const Game::IBlockAccess* blocks, const glm::ivec3& pos,
                           bool asTerrainParticle) {
        using B = Game::BlockID;
        const B id = state.Block();
        const auto biome = [&](Channel c) -> int64_t {
            if (!blocks) return -1;
            return BlendedColor(*blocks, pos, c);
        };
        switch (id) {
            // BlockTintSources.grassBlock: colorAsTerrainParticle -1.
            case B::Grass:
                return asTerrainParticle ? -1 : biome(Channel::Grass);
            // doubleTallGrass / grass / sugarCane: the average grass colour.
            case B::LargeFern: case B::TallGrass: case B::Fern: case B::ShortGrass:
            case B::PottedFern: case B::Bush: case B::SugarCane:
                return biome(Channel::Grass);
            case B::SpruceLeaves: return 0x619961;   // constant(-10380959)
            case B::BirchLeaves:  return 0x80A755;   // constant(-8345771)
            case B::OakLeaves: case B::JungleLeaves: case B::AcaciaLeaves: case B::DarkOakLeaves:
            case B::Vine: case B::MangroveLeaves:
                return biome(Channel::Foliage);
            case B::LeafLitter:
                return biome(Channel::DryFoliage);
            case B::WaterCauldron:
                return biome(Channel::Water);
            // waterParticles: only its terrain particles are tinted.
            case B::Water: case B::BubbleColumn:
                return asTerrainParticle ? biome(Channel::Water) : -1;
            case B::RedstoneWire:
                return Game::RedstoneWireColorForPower(state.GetIndex(Game::PropertyId::POWER)) & 0xFFFFFF;
            case B::AttachedMelonStem: case B::AttachedPumpkinStem:
                return 0xE0C71C;   // constant(-2046180)
            case B::MelonStem: case B::PumpkinStem: {
                // BlockTintSources.stem: ARGB.color(age * 32, 255 - age * 8, age * 4).
                const std::string_view ageName = state.GetValueByName("age");
                const int age = ageName.empty() ? 0 : std::clamp(ageName.front() - '0', 0, 7);
                return (static_cast<int64_t>(age * 32) << 16) | (static_cast<int64_t>(255 - age * 8) << 8) |
                       static_cast<int64_t>(age * 4);
            }
            case B::LilyPad:
                return 0x208030;   // constant(-9321636, -14647248): the in-world colour
            default:
                return -1;
        }
    }

} // namespace Render
