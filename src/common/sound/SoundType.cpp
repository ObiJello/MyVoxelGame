// File: src/common/sound/SoundType.cpp
#include "common/sound/SoundType.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    namespace SoundTypes {
#define SOUND_TYPE(NAME, V, P, B, S, PL, H, F) const SoundType NAME{V, P, B, S, PL, H, F};
#include "common/sound/GeneratedSoundTypes.inc"
#undef SOUND_TYPE
    }

    namespace BlockSetTypes {
#define BLOCK_SET_TYPE(NAME, ST, DC, DO, TC, TO, PF, PN, BF, BN) \
        const BlockSetType NAME{#NAME, &SoundTypes::ST, DC, DO, TC, TO, PF, PN, BF, BN};
#define WOOD_TYPE(NAME, SET, ST, HST, GC, GO)
#include "common/sound/GeneratedBlockSetTypes.inc"
#undef BLOCK_SET_TYPE
#undef WOOD_TYPE
    }

    namespace WoodTypes {
#define BLOCK_SET_TYPE(NAME, ST, DC, DO, TC, TO, PF, PN, BF, BN)
#define WOOD_TYPE(NAME, SET, ST, HST, GC, GO) \
        const WoodType NAME{#NAME, &BlockSetTypes::SET, &SoundTypes::ST, &SoundTypes::HST, GC, GO};
#include "common/sound/GeneratedBlockSetTypes.inc"
#undef BLOCK_SET_TYPE
#undef WOOD_TYPE
    }

    namespace {

        struct BlockSoundRow {
            const SoundType*    soundType;
            const BlockSetType* setType;
            const WoodType*     woodType;
        };

        // The generated rows by slug.
        const std::unordered_map<std::string_view, BlockSoundRow>& RowsBySlug() {
            static const std::unordered_map<std::string_view, BlockSoundRow> rows = [] {
                std::unordered_map<std::string_view, BlockSoundRow> m;
                m.reserve(2048);
#define SET_TYPE(NAME)     (&BlockSetTypes::NAME)
#define WOOD_TYPE_OF(NAME) (&WoodTypes::NAME)
#define NO_SET_TYPE        nullptr
#define NO_WOOD_TYPE       nullptr
#define BLOCK_SOUND_TYPE(SLUG, ST, SET, WOOD) \
                m.emplace(SLUG, BlockSoundRow{&SoundTypes::ST, SET, WOOD});
#include "common/sound/GeneratedBlockSoundTypes.inc"
#undef BLOCK_SOUND_TYPE
#undef NO_WOOD_TYPE
#undef NO_SET_TYPE
#undef WOOD_TYPE_OF
#undef SET_TYPE
                return m;
            }();
            return rows;
        }

        // Per BlockID, resolved once from the registry slugs.
        const std::vector<BlockSoundRow>& RowsById() {
            static const std::vector<BlockSoundRow> rows = [] {
                std::vector<BlockSoundRow> v(BlockRegistry::Size,
                                             BlockSoundRow{&SoundTypes::STONE, nullptr, nullptr});
                const auto& bySlug = RowsBySlug();
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    const std::string& slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    const auto it = bySlug.find(slug);
                    if (it != bySlug.end()) v[i] = it->second;
                }
                // Air: MC Blocks.AIR has .air() and no sound override, i.e. STONE —
                // but nothing ever plays an air block's sound, and a caller asking
                // is looking at the wrong cell. EMPTY makes that harmless.
                v[static_cast<size_t>(BlockID::Air)].soundType = &SoundTypes::EMPTY;
                return v;
            }();
            return rows;
        }

        const BlockSoundRow& Row(BlockID block) {
            const auto& rows = RowsById();
            const size_t i = static_cast<size_t>(block);
            return i < rows.size() ? rows[i] : rows[0];
        }

    } // namespace

    const SoundType& SoundTypeOf(BlockID block) {
        return *Row(block).soundType;
    }

    const SoundType& SoundTypeOf(BlockState state) {
        const BlockID block = state.Block();
        // MC DecoratedPotBlock.getSoundType: the cracked pot shatters.
        if (block == BlockID::DecoratedPot) {
            return state.GetName(PropertyId::CRACKED) == "true"
                ? SoundTypes::DECORATED_POT_CRACKED : SoundTypes::DECORATED_POT;
        }
        return SoundTypeOf(block);
    }

    const BlockSetType* BlockSetTypeOf(BlockID block) { return Row(block).setType; }
    const WoodType*     WoodTypeOf(BlockID block)     { return Row(block).woodType; }

} // namespace Game
