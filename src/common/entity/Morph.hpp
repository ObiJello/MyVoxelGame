// File: src/common/entity/Morph.hpp
//
// /morph: what a player has become, packed into one 32-bit code that rides
// the wire (PlayerUpdateS2C for the others, PlayerAbilitiesS2C for the
// player's own body): a kind in the top byte and an id in the low 24 bits —
// a mob's EntityTypeId, an item's ItemID, a block's BlockID, or nothing for
// an experience orb. kNone is no morph.
//
// The body that goes with it (box and eye height) lives here too, so the
// server's entity view, the client's physics and the renderers all agree:
// a mob's from its type table, and MC's for the three entity kinds (item
// 0.25², orb 0.5², falling block 0.98², eyes at the default 0.85 × height).
#pragma once
#include "common/world/block/BlockState.hpp"

#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Item.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Game::Morph {

    constexpr uint32_t kNone = 0xFFFFFFFFu;

    // Player: a Minecraft player model with a skin; id 0 is Herobrine (the
    // Steve skin with white eyes, assets/textures/entity/player/herobrine.png).
    enum class Kind : uint8_t { Mob = 0, Item = 1, Block = 2, Xp = 3, Player = 4 };
    constexpr uint32_t kHerobrine = 0;

    constexpr uint32_t Encode(Kind kind, uint32_t id) {
        return (static_cast<uint32_t>(kind) << 24) | (id & 0xFFFFFFu);
    }
    constexpr Kind     KindOf(uint32_t code) { return static_cast<Kind>((code >> 24) & 0xFFu); }
    constexpr uint32_t IdOf(uint32_t code)   { return code & 0xFFFFFFu; }
    constexpr bool     IsNone(uint32_t code) { return code == kNone; }

    // A mob morph's id is the EntityTypeId in the low 16 bits; bit 16 is
    // "sheared" (a sheep morph after shears, until it eats grass); bit 17
    // is "baby" (`/morph baby <entity>`).
    constexpr uint32_t   MobTypeOf(uint32_t code)   { return IdOf(code) & 0xFFFFu; }
    constexpr uint32_t   kMobShearedBit = 0x10000u;
    constexpr uint32_t   kMobBabyBit    = 0x20000u;
    // Bit 16 of a mob morph, one flag per kind: a sheep is sheared, a snow
    // golem has lost its pumpkin (shears too), an armadillo is rolled up
    // (Left Alt). IsSheared reads it whatever it means for the mob.
    constexpr bool       IsSheared(uint32_t code)   { return (IdOf(code) & kMobShearedBit) != 0; }
    constexpr uint32_t   WithSheared(uint32_t code, bool on) {
        return Encode(Kind::Mob, (IdOf(code) & ~kMobShearedBit) | (on ? kMobShearedBit : 0u));
    }
    // Bit 17: the mob's baby form — the baby box and eye height (DimsOf),
    // the baby mesh and sheet on screen, a baby zombie's speed.
    constexpr bool       IsBaby(uint32_t code)      { return (IdOf(code) & kMobBabyBit) != 0; }
    constexpr uint32_t   WithBaby(uint32_t code, bool on) {
        return Encode(Kind::Mob, (IdOf(code) & ~kMobBabyBit) | (on ? kMobBabyBit : 0u));
    }

    // A block morph's id is the BlockID in the low 16 bits and its yaw
    // (quarter turns, 0..3, Shift+Alt in game) in the two above: the same
    // code carries both to every client.
    constexpr uint32_t BlockOf(uint32_t code)         { return IdOf(code) & 0xFFFFu; }
    constexpr uint32_t BlockRotationOf(uint32_t code) { return (IdOf(code) >> 16) & 3u; }
    // Bit 18: the block is open (a door morph, swung by a right-click).
    constexpr uint32_t kBlockOpenBit = 0x40000u;
    constexpr bool     IsBlockOpen(uint32_t code)     { return (IdOf(code) & kBlockOpenBit) != 0; }
    constexpr uint32_t WithBlockRotation(uint32_t code, uint32_t quarterTurns) {
        return Encode(Kind::Block, BlockOf(code) | ((quarterTurns & 3u) << 16) | (IdOf(code) & kBlockOpenBit));
    }
    constexpr uint32_t WithBlockOpen(uint32_t code, bool open) {
        return Encode(Kind::Block, (IdOf(code) & ~kBlockOpenBit) | (open ? kBlockOpenBit : 0u));
    }

    // A known kind with an id in range.
    bool IsValid(uint32_t code);

    // A mob that lives in the air: a morph into one flies always — the
    // body is put in flight and kept there.
    bool IsFlier(uint32_t code);
    bool IsMob(uint32_t code, EntityTypeId type);

    // MC Creeper.maxSwell: ticks from calm to the bang. The morph's swell
    // (0..kCreeperSwellTicks) rides the move packet as one byte.
    constexpr int kCreeperSwellTicks = 30;
    // MC EatBlockGoal.EAT_ANIMATION_TICKS: the sheep's graze, head down.
    constexpr int kSheepEatTicks = 40;
    // Ticks a skeleton morph holds the drawn bow after a shot.
    constexpr int kSkeletonAimTicks = 10;

    // MC Spider.onClimbable: horizontal collision is a wall to climb.
    bool ClimbsWalls(uint32_t code);

    struct Dims {
        float width;
        float height;
        float eyeHeight;
    };
    // The player's own body for kNone or an invalid code.
    Dims DimsOf(uint32_t code);

    // The block state a Block morph shows and, locked to the grid, places:
    // the block's default state, turned by the morph's rotation (Shift+Alt)
    // through its facing property when it has one — a ladder goes round
    // the four walls, a furnace turns its front — so the drawn model and
    // the placed block agree. `turnedByState` says the turn is in the
    // state (the renderer then does not also rotate the mesh); blocks
    // without a facing turn as a mesh about the cell's centre.
    // `upper`: the top half of a two-block-high block (a door, a tall
    // plant — DOUBLE_BLOCK_HALF); IsDoubleBlock says whether there is one.
    BlockState BlockStateOf(uint32_t code, bool* turnedByState = nullptr, bool upper = false);
    bool IsDoubleBlock(uint32_t code);

    // "[minecraft:]slug" → an ItemID: the pure items by their table slug,
    // then the block items by the block's registry slug.
    bool ParseItemSlug(const std::string& text, ItemID& out);
    // Every item slug, pure and block, for tab completion.
    std::vector<std::string> ItemSlugs();

} // namespace Game::Morph
