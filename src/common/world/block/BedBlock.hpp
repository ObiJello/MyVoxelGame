// File: src/common/world/block/BedBlock.hpp
//
// MC AbstractBedBlock / BedBlock — the two-cell bed and everything both
// sides of the wire need to know about it: which blocks are beds, where the
// other half of a pair sits, the placement pair, the stand-up search a
// sleeper (or a respawn) uses, and the right-click behaviour.
//
// The sleeping itself — the checks in ServerPlayer.startSleepInBed, the
// sleep counter, the night skip — is server-side (PlayerSession /
// IntegratedServer). What lives here is the block: identical on the client,
// which runs BedUse to predict that the click was swallowed and, in the
// Nether and End, that both halves vanished.
//
// 26.3 turned the bed's dimension rules into the BedRule environment
// attribute. This engine's dimensions have fixed rules, so the attribute is
// folded to its per-dimension values (DimensionBedRule): the Overworld is
// CAN_SLEEP_WHEN_DARK (sleep at night, always sets spawn), the Nether and
// the End are DESTROY_ON_USE (the bed explodes), and the Hush is its own
// rule — never sleep, never set spawn, never explode: the bed refuses you
// with a line from HushBedRefusal.
#pragma once

#include "Blocks.hpp"
#include "BlockState.hpp"
#include "BlockInteraction.hpp"
#include "BlockRegistry.hpp"
#include "Direction.hpp"
#include "common/world/level/DimensionId.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Game {

    struct IBlockAccess;
    class ILevelWrite;
    class IUsePlayer;

    // Every "*_bed" block: the sixteen dyed beds and the straw bed.
    bool IsBedBlock(BlockID id);

    // MC BedBlock.SHAPES → getSleepHeight: the mattress top, 9/16. A sleeper
    // lies at bedY + this + 0.125 (LivingEntity.setPosToBed).
    constexpr float kBedSleepHeight = 9.0f / 16.0f;
    // MC AbstractBedBlock.getSleepHeight: the shape's top — 9/16 for a dyed
    // bed, 4/16 for the straw bed (StrawBedBlock reads the foot's slab).
    constexpr float BedSleepHeight(BlockID id) {
        return id == BlockID::StrawBed ? 4.0f / 16.0f : kBedSleepHeight;
    }

    Direction BedFacing(BlockState state);

    // MC BedBlock.SHAPES: the mattress slab Block.column(16, 3, 9) plus two
    // 3×3×3 legs at the corners of the half's OUTER end — the side
    // getConnectedDirection(state).getOpposite() names (the head's legs at
    // the head end, the foot's at the foot end). Outline and collision
    // alike; the bounds are the full footprint, 9/16 tall. Dyed beds only —
    // the straw bed's shape comes from its block model.
    BlockRegistry::BlockShapeSet BedShapeBoxes(BlockState state);
    bool      IsBedHead(BlockState state);
    bool      IsBedOccupied(BlockState state);

    // MC AbstractBedBlock.getNeighbourDirection: the foot's partner is in the
    // facing direction, the head's is behind it.
    glm::ivec3 BedOtherHalfPos(const glm::ivec3& pos, BlockState state);
    // The head cell of the pair that `pos` (either half) belongs to.
    glm::ivec3 BedHeadPos(const glm::ivec3& pos, BlockState state);

    // True when the cell in the partner direction holds the same bed block
    // with the opposite part — the pair the use / break paths require.
    bool BedPairIntact(const IBlockAccess& level, const glm::ivec3& pos, BlockState state);

    // MC AbstractBedBlock.getStateForPlacement + setPlacedBy: the clicked
    // cell takes the FOOT (facing already set by the horizontal rule), the
    // head goes in the facing direction.
    BlockState BedFootPlacementState(BlockState state);
    BlockState BedHeadState(BlockState foot);
    BlockState BedWithOccupied(BlockState state, bool occupied);

    // MC AbstractBedBlock.findStandUpPosition(type, level, pos, forward, yaw)
    // for a player: the 12-offset search around the bed (safe cells first,
    // then any standable cell), bunk beds included. `headPos` is the head
    // cell, `facing` the bed's FACING, `yaw` the sleeper's yaw. Empty when no
    // cell around the bed can take a player; callers fall back to the cell
    // above the bed the way LivingEntity.stopSleeping does.
    std::optional<glm::dvec3> FindBedStandUpPosition(const IBlockAccess& level,
                                                     const glm::ivec3& headPos,
                                                     Direction facing, float yaw);

    // MC BedRule (world/attribute/BedRule.java): whether a bed lets you
    // sleep, whether it sets your respawn point, and whether it is destroyed
    // on use (the explosion) or when you get up, and the action-bar line a
    // refused sleep shows (BedRule.errorMessage → asProblem).
    struct BedRule {
        // MC BedRule.Rule: ALWAYS, WHEN_DARK (Level.isDarkOutside), NEVER.
        enum class Check : uint8_t { Always, WhenDark, Never };
        // MC errorMessage: empty (nothing shown), block.minecraft.bed
        // .no_sleep, or the Hush's HushBedRefusal lines.
        enum class Message : uint8_t { None, NoSleep, Hush };
        Check   canSleep;
        Check   canSetSpawn;
        bool    destroyOnUse;
        bool    destroyOnLeave;
        Message errorMessage;
    };

    constexpr bool BedRuleTest(BedRule::Check check, bool darkOutside) {
        return check == BedRule::Check::Always ||
               (check == BedRule::Check::WhenDark && darkOutside);
    }

    // The BedRule a bed of block `bed` follows in `dimension` — MC's
    // BED_RULE / STRAW_BED_RULE attribute (StrawBedBlock
    // .getBedEnvironmentAttribute) folded per dimension:
    //   Overworld  dyed bed  CAN_SLEEP_WHEN_DARK, straw bed DESTROY_ON_LEAVE;
    //   Nether, End          DESTROY_ON_USE (a dyed bed explodes, a straw
    //                        bed just breaks);
    //   Hush                 never sleep, never set spawn, never destroyed —
    //                        the refusal is HushBedRefusal's;
    //   Twilight Forest  dyed bed  TF's BED_RULE (TFDimensionGenerator:
    //                        can_sleep NEVER, can_set_spawn ALWAYS, not
    //                        destroyed, no error message — the click sets
    //                        spawn and shows nothing else); straw bed the
    //                        attribute default, DESTROY_ON_LEAVE;
    //   Aether     the_aether.json bed_works true → the attribute defaults,
    //                        as the Overworld (CAN_SLEEP_WHEN_DARK, straw
    //                        bed DESTROY_ON_LEAVE). The mod's eternal-day
    //                        refusal (DimensionHooks.isEternalDay →
    //                        NOT_POSSIBLE_NOW) is not modelled: the server
    //                        runs no Aether time.
    BedRule DimensionBedRule(DimensionId dimension, BlockID bed);

    // The action-bar lines a Hush bed answers with (its BedRule error
    // message); `pick` is any random number, reduced modulo the pool.
    std::string_view HushBedRefusal(uint32_t pick);

    // MC AbstractBedBlock.useWithoutItem. Normalises to the head, applies the
    // dimension's bed rule, and hands the sleep request to the player
    // (IUsePlayer::UseBed) — the request is the server's to grant.
    UseResult BedUse(ILevelWrite* world, const glm::ivec3& pos,
                     IUsePlayer* player, const BlockHitResult& hit);

    // MC StrawBedBlock.destroyBed: the break-on-leave sound, then the head
    // cell set to air (updateShape takes the foot with it — explicit here).
    // Used by BedUse in the Nether and End (STRAW_BED_RULE DESTROY_ON_USE,
    // no explosion for straw) and by the server when a sleeper gets up in
    // the Overworld (DESTROY_ON_LEAVE).
    void DestroyStrawBed(ILevelWrite* world, const glm::ivec3& headPos);

} // namespace Game
