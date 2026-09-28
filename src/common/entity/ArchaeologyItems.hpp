// File: src/common/entity/ArchaeologyItems.hpp
//
// The brush (MC BrushItem) and the goat horn (MC InstrumentItem), reachable
// from common. Same bridge shape as HushItems.hpp: ItemBehaviors.cpp wires
// the items, the server half (server/items/ArchaeologyItems.cpp) does what
// needs the ServerPlayer — the hold-to-use lifecycle, the cooldown, the
// server level's block entity — and each call answers "nothing happened"
// with no server behind it.
//
//   brush     — useOn a block starts a 200-tick hold (BRUSH pose). Every
//               10 ticks of it (on the 5th, the stroke before the
//               backswing) the view ray's block gets a brush sound and, when
//               it is a suspicious sand / gravel block, one stroke of its
//               BrushableBlockEntity; the stroke that finishes the dig costs
//               the brush a point of durability. Looking off the block ends
//               the hold.
//   goat horn — use plays the stack's INSTRUMENT (sound, range / 16 volume,
//               the INSTRUMENT_PLAY game event), holds for the
//               instrument's use_duration in the TOOT_HORN pose and puts the
//               horn on that long a cooldown.
#pragma once

#include "common/world/block/BlockInteraction.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Game {

    class ILevelWrite;
    class IUsePlayer;
    struct ItemStack;

    namespace Archaeology {

        // MC BrushItem.USE_DURATION / ANIMATION_DURATION.
        inline constexpr int kBrushUseDuration       = 200;
        inline constexpr int kBrushAnimationDuration = 10;

        // The block the player's view ray meets first (the POV clip the
        // bucket uses: outline shapes, fluids ignored), within `reach`
        // blocks, and the face it entered through (Direction 3D data value).
        // Implemented in ItemBehaviors.cpp.
        struct ViewBlockHit {
            glm::ivec3 pos{0};
            int        face = 1;
            glm::dvec3 location{0.0};   // where the ray meets that face
            glm::dvec3 viewVector{0.0}; // the ray's direction (unit)
        };
        std::optional<ViewBlockHit> ClipPlayerView(ILevelWrite& world, const IUsePlayer& player,
                                                   float reach);

        // BrushItem.useOn: starts the hold when the view ray is on a block.
        // CONSUME either way. Server half in server/items/ArchaeologyItems.cpp;
        // the client's pose comes from its own predicted use.
        UseResult BrushUseOn(const UseOnContext& ctx, ItemStack& stack);
        // BrushItem.onUseTick (server; ServerPlayer::updateUsingItem).
        void BrushUseTick(IUsePlayer& player, ItemStack& stack, int remainingTicks);

        // InstrumentItem.use, server half: the hold and the cooldown. The
        // sound and game event run in the common behaviour on both sides.
        UseResult GoatHornBegin(IUsePlayer& player, uint32_t hand, const ItemStack& stack);

    } // namespace Archaeology
} // namespace Game
