// File: src/client/renderer/mesh/BlockTint.hpp
//
// MC BlockColors.createDefault + BlockTintSource.colorInWorld + the biome
// ColorResolvers, written once so the places that tint a block MODEL in the
// world do it with the same table and the same arithmetic:
//
//   * the section mesher (Mesher::AddBlockFace), over its snapshot caches and
//     ClientLevel.calculateBlockTint's (2r+1)² blend;
//   * the moving-block pass (BlockCubeEntityRenderer — pistons' carried
//     blocks, falling blocks), over the live client level with MC
//     MovingBlockRenderState.getBlockTint's single, unblended biome;
//   * the block particles (TerrainParticle, FallingDustParticle) and the
//     tinted leaves' falling-leaf colour (ClientLevel.getClientLeafTintColor)
//     — Layer0Color, MC BlockColors.getTintSource(state, 0) asked
//     colorAsTerrainParticle / colorInWorld over the live level.
//
// Vanilla dispatches the tint on the BLOCK and treats tintIndex only as a
// filter inside that block's resolver — which is why grass_block (tintindex
// 0) takes the GRASS colormap while oak_leaves (also tintindex 0) takes
// FOLIAGE. The block → resolver table is matched on model name, so a
// snapshot bump (or a mod block reusing a vanilla model name) picks new
// members of each family up the way the mining/collision classifiers in
// BlockRegistry already do.
//
// Colours are 0xRRGGBB; kUntinted (white) is MC's -1.
#pragma once

#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/RedstoneWire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Render::BlockTint {

    inline constexpr uint32_t kUntinted = 0xFFFFFF;

    // MC BiomeColors' four ColorResolvers.
    enum class Channel : uint8_t { Grass, Foliage, DryFoliage, Water };

    // Which BlockTintSource a block registers (MC BlockColors.createDefault).
    enum class Source : uint8_t {
        None,          // no resolver registered -> untinted (MC returns -1)
        Biome,         // the biome colour of `channel`
        Constant,      // fixed colour (spruce / birch leaves, lily pad, attached stems)
        FlowerBed,     // tintIndex 0 untinted, otherwise grass
        StemAge,       // melon / pumpkin stem: colour computed from `age`
        RedstonePower, // redstone dust: RedstoneWireBlock.COLORS[power]
    };

    // Where MC BlockTintSource.colorAsTerrainParticle is not simply
    // colorInWorld (BlockTintSources' two overrides).
    enum class ParticleTint : uint8_t {
        InWorld,    // the default: colorInWorld
        Untinted,   // grassBlock: -1 — its debris is dirt
        BiomeOnly,  // waterParticles: the `channel` biome colour (colorInWorld is -1)
    };

    struct Profile {
        Source       source   = Source::None;
        Channel      channel  = Channel::Grass;
        uint32_t     constant = kUntinted;
        ParticleTint particle = ParticleTint::InWorld;
        // BlockTintSources.doubleTallGrass: the upper half reads the biome at
        // pos.below() (Layer0Color honours it; the section mesher reads the
        // plant's own cell, see Classify).
        bool         doubleTallGrass = false;
    };

    // MC BlockColors.createDefault, in its own registration order.
    inline Profile Classify(const std::string& n) {
        auto has = [&](std::string_view sub) { return n.find(sub) != std::string::npos; };
        auto is  = [&](std::string_view ex)  { return n == ex; };

        Profile p;
        // GRASS: grass_block, fern, short_grass, potted_fern, bush,
        // sugar_cane, and both halves of large_fern / tall_grass.
        //
        // The double plants are substring matches because this engine
        // splits each into two BlockIDs whose model names carry a
        // _bottom / _top suffix. MC samples the UPPER half's biome at
        // pos.below(); with biomes on a 4-block grid and a 5x5 blend,
        // one block of vertical offset changes the result only where a
        // biome border also happens to fall on a quart boundary, so the
        // plant's own position is used here.
        if (is("grass_block") || is("grass_block_snow") ||
            is("fern") || is("short_grass") || is("potted_fern") ||
            is("bush") || is("sugar_cane") ||
            has("large_fern") || has("tall_grass")) {
            p.source = Source::Biome;
            p.channel = Channel::Grass;
            // BlockTintSources.grassBlock.colorAsTerrainParticle: -1.
            if (is("grass_block") || is("grass_block_snow")) p.particle = ParticleTint::Untinted;
            p.doubleTallGrass = has("large_fern") || has("tall_grass");
        } else if (has("pink_petals") || has("wildflowers")) {
            // tintIndex 0 is the petals (already coloured, untinted);
            // anything else is the stem, which takes grass.
            p.source = Source::FlowerBed;
        } else if (is("spruce_leaves")) {
            p.source = Source::Constant;  // FoliageColor.FOLIAGE_EVERGREEN
            p.constant = 0x619961;
        } else if (is("birch_leaves")) {
            p.source = Source::Constant;  // FoliageColor.FOLIAGE_BIRCH
            p.constant = 0x80A755;
        } else if (has("leaf_litter")) {
            p.source = Source::Biome;
            p.channel = Channel::DryFoliage;
        } else if (is("oak_leaves") || is("jungle_leaves") ||
                   is("acacia_leaves") || is("dark_oak_leaves") ||
                   is("mangrove_leaves") || is("vine")) {
            // EXACTLY MC's foliage list (BlockColors.java:47), not a
            // "_leaves" substring. cherry_leaves and pale_oak_leaves
            // carry tintindex 0 in their models but are NOT registered
            // in vanilla, so they render from their own artwork —
            // a substring match turns cherry blossom green.
            p.source = Source::Biome;
            p.channel = Channel::Foliage;
        } else if (has("water_cauldron")) {
            p.source = Source::Biome;   // BlockTintSources.water
            p.channel = Channel::Water;
        } else if (is("water") || is("bubble_column")) {
            // BlockTintSources.waterParticles: only colorAsTerrainParticle
            // reads the water colour; colorInWorld is the default color(state),
            // -1. (Neither block has a model to tint: FluidRenderer colours
            // the water itself.)
            p.source = Source::Constant;
            p.constant = kUntinted;
            p.channel = Channel::Water;
            p.particle = ParticleTint::BiomeOnly;
        } else if (is("lily_pad")) {
            p.source = Source::Constant;  // BlockColors.LILY_PAD_IN_WORLD
            p.constant = 0x208030;
        } else if (is("attached_melon_stem") || is("attached_pumpkin_stem")) {
            p.source = Source::Constant;  // BlockColors.java: -2046180
            p.constant = 0xE0C71C;
        } else if (is("redstone_wire")) {
            // MC BlockColors registers RedstoneWireBlock.getColorForPower
            // with addColoringState(POWER): the dust texture is
            // greyscale and its whole colour — dark red at 0, bright
            // red at 15 — comes from the state's power.
            p.source = Source::RedstonePower;
        } else if (is("melon_stem") || is("pumpkin_stem")) {
            // BlockColors.java:54-57 — a growing stem fades from green
            // to the attached stem's yellow as it ages:
            //   ARGB.color(age * 32, 255 - age * 8, age * 4)
            // MC also calls addColoringState(StemBlock.AGE, …), which
            // is what tells it to re-bake per state.
            //
            // There is only ONE stem texture (melon_stem.png), greyscale
            // — every stage's colour comes from this tint, so without it
            // a whole field of stems renders identically grey-green.
            p.source = Source::StemAge;
        }
        return p;
    }

    // Classify, once per block for the life of the process (the registry's
    // model names are fixed once it has loaded). For callers outside the
    // mesher, whose thread-local props cache already holds the same answer.
    inline const Profile& ProfileOf(Game::BlockID id) {
        static const auto table = [] {
            std::array<Profile, static_cast<size_t>(Game::BlockID::Count)> t{};
            for (size_t i = 0; i < t.size(); ++i) {
                t[i] = Classify(Game::BlockRegistry::Get(static_cast<Game::BlockID>(i)).modelName);
            }
            return t;
        }();
        const size_t i = static_cast<size_t>(id);
        static const Profile kNone{};
        return i < table.size() ? table[i] : kNone;
    }

    // MC BiomeColors' resolvers: Biome.getGrassColor(x, z) (the swamp
    // modifier samples noise at x, z), getFoliageColor, getDryFoliageColor,
    // getWaterColor.
    inline uint32_t ChannelColor(Channel c, Game::BiomeId biome, int x, int z) {
        switch (c) {
            case Channel::Grass:      return Game::BiomeRegistry::GrassColor(biome, x, z);
            case Channel::Foliage:    return Game::BiomeRegistry::FoliageColor(biome);
            case Channel::DryFoliage: return Game::BiomeRegistry::DryFoliageColor(biome);
            case Channel::Water:      return Game::BiomeRegistry::WaterColor(biome);
        }
        return kUntinted;
    }

    // MC ClientLevel.calculateBlockTint — the biome blend.
    //
    //   int radius = options.biomeBlendRadius().get();          // default 2
    //   if (radius == 0) return resolver.getColor(biome(pos), x, z);
    //   int size = (radius*2+1)^2;
    //   ...accumulate r/g/b over the square at THIS y...
    //   return (r/size)<<16 | (g/size)<<8 | (b/size);
    //
    // The square is horizontal only — every sample uses the block's own Y. The
    // per-channel integer average (not a colour-space blend) is what gives
    // vanilla its characteristic 5-block-wide biome gradient. Radius 0 is the
    // single-lookup fast path, which the loop degenerates to.
    //
    // `biomeAt(x, y, z)` is MC getBiome — the fuzzy-zoomed biome id.
    template <class BiomeAt>
    uint32_t BlendedColor(Channel channel, int x, int y, int z, int radius, BiomeAt&& biomeAt) {
        const int size = (radius * 2 + 1) * (radius * 2 + 1);
        int r = 0, g = 0, b = 0;
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int sx = x + dx;
                const int sz = z + dz;
                const uint32_t c = ChannelColor(channel, static_cast<Game::BiomeId>(biomeAt(sx, y, sz)), sx, sz);
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
            }
        }
        return static_cast<uint32_t>(((r / size) << 16) | ((g / size) << 8) | (b / size));
    }

    // The `age` of a melon/pumpkin stem, for the StemAge tint. Reads the
    // state definition rather than assuming state index == age: that
    // happens to hold today (stems declare `age` as their only property)
    // but would silently produce wrong colours the moment one gained a
    // second property.
    inline int StemAgeOf(Game::BlockID id, Game::BlockStateIndex stateIndex) {
        const std::string_view v =
            Game::BlockRegistry::GetStateDefinition(id).ValueOf(stateIndex, "age");
        int n = 0;
        for (char c : v) {
            if (c < '0' || c > '9') return 0;
            n = n * 10 + (c - '0');
        }
        return n;
    }

    // MC ModelBlockRenderer.computeTintColor → BlockTintSource.colorInWorld
    // for one quad of `state` with `tintIndex`: the colour its vertices are
    // multiplied by, or kUntinted. `biomeColor(Channel)` answers the level's
    // biome colour for the block's position — the mesher's blended one, or a
    // MovingBlockRenderState's single biome — so this function is the whole
    // block → resolver dispatch and nothing else.
    template <class BiomeColor>
    uint32_t ColorInWorld(const Profile& profile, int tintIndex, Game::BlockState state,
                          BiomeColor&& biomeColor) {
        if (tintIndex < 0) return kUntinted;
        switch (profile.source) {
            case Source::None:
                return kUntinted;
            case Source::Constant:
                return profile.constant & 0xFFFFFF;
            case Source::Biome:
                return biomeColor(profile.channel) & 0xFFFFFF;
            case Source::FlowerBed:
                // BlockColors: List.of(BLANK_LAYER, grass()) — layer 0 is -1.
                return tintIndex != 0 ? (biomeColor(Channel::Grass) & 0xFFFFFF) : kUntinted;
            case Source::RedstonePower:
                return Game::RedstoneWireColorForPower(state.GetIndex(Game::PropertyId::POWER)) & 0xFFFFFF;
            case Source::StemAge: {
                // BlockTintSources.stem, verbatim:
                //   ARGB.color(age * 32, 255 - age * 8, age * 4)
                // age 0 = (0, 255, 0) bright green; age 7 = (224, 199, 28),
                // which is exactly the attached stem's constant, so a stem
                // that matures and attaches does not visibly change colour.
                const int age = StemAgeOf(state.Block(), state.Index());
                return (static_cast<uint32_t>(age * 32 & 0xFF) << 16) |
                       (static_cast<uint32_t>((255 - age * 8) & 0xFF) << 8) |
                       static_cast<uint32_t>(age * 4 & 0xFF);
            }
        }
        return kUntinted;
    }

    // MC BlockTintSource.colorAsTerrainParticle for tint layer 0.
    template <class BiomeColor>
    uint32_t ColorAsTerrainParticle(const Profile& profile, Game::BlockState state, BiomeColor&& biomeColor) {
        switch (profile.particle) {
            case ParticleTint::Untinted:  return kUntinted;
            case ParticleTint::BiomeOnly: return biomeColor(profile.channel) & 0xFFFFFF;
            case ParticleTint::InWorld:   break;
        }
        return ColorInWorld(profile, 0, state, biomeColor);
    }

    // The colour MC asks of tint layer 0 against a live level, with
    // ClientLevel.calculateBlockTint's blend (`blendRadius`, the Biome Blend
    // option) around (x, y, z):
    //
    //   asTerrainParticle   colorAsTerrainParticle — TerrainParticle (break,
    //                       crack, dust pillar) and FallingDustParticle;
    //   otherwise           colorInWorld — ClientLevel.getClientLeafTintColor.
    //
    // Empty when the block registers no tint source (MC getTintSource(state,
    // 0) is null, and each caller has its own answer for that); a source that
    // answers -1 (grass_block's particles, the flower beds' blank layer 0,
    // water in the world) is kUntinted. `biomeAt(x, y, z)` is MC getBiome.
    template <class BiomeAt>
    std::optional<uint32_t> Layer0Color(Game::BlockState state, int x, int y, int z, int blendRadius,
                                        bool asTerrainParticle, BiomeAt&& biomeAt) {
        const Profile& profile = ProfileOf(state.Block());
        if (profile.source == Source::None) return std::nullopt;
        // BlockTintSources.doubleTallGrass: HALF == UPPER ? pos.below() : pos.
        if (profile.doubleTallGrass && state.GetValueByName("half") == "upper") --y;
        const auto biomeColor = [&](Channel channel) {
            return BlendedColor(channel, x, y, z, blendRadius, biomeAt);
        };
        return asTerrainParticle ? ColorAsTerrainParticle(profile, state, biomeColor)
                                 : ColorInWorld(profile, 0, state, biomeColor);
    }

} // namespace Render::BlockTint
