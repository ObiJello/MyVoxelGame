// File: src/common/world/block/piston/PistonBlockEntities.hpp
//
// The pistons_move_block_entities engine rule: pistons push and pull blocks
// that carry a block entity — chests, barrels, furnaces, hoppers, droppers,
// dispensers, signs, banners, lecterns, campfires, beehives … — taking the
// entity (inventory, custom name, lock, text, patterns, bees) along and
// putting it back down where the block lands. Off = vanilla, exactly: MC
// PistonBaseBlock.isPushable ends in `!state.hasBlockEntity()`.
//
// HOW A MOVE CARRIES ITS ENTITY
//   PistonBaseBlock's moveBlocks DETACHES the entity of every pushed cell
//   before the first write (ILevelWrite::TakeBlockEntity — no side effects,
//   so a container does not spill) and hands it to the PistonMovingBlock-
//   Entity of the cell the block moves into. When that entity lands the
//   block (its tick, finalTick, or a write over the moving cell), the carried
//   entity is installed at the landing cell (PlaceCarried) — or, when the
//   block did not survive the landing, its contents are spilled there
//   (MC Containers.dropContents), so nothing is ever lost or doubled: the
//   entity has exactly one owner at every moment (chunk → moving cell →
//   chunk). A move in flight saves its carried entity with the moving cell
//   (BlockEntityNbt, `obeycraft_carried`).
//
//   The client mirrors the move from the block event as vanilla does, so it
//   carries its own copy for drawing (PistonRenderer draws the carried
//   entity's model with the moving block); the server's landing broadcast is
//   what both sides end on.
//
// WHAT MOVES
//   Only the entity types whose state is position-free (the whitelist in
//   PistonBlockEntities.cpp). Kept immovable whatever the rule says:
//   spawners, trial spawners and vaults (structure progression), the
//   jukebox (its playing song is registered by position), the sculk sensor,
//   calibrated sensor, shrieker and catalyst (their vibration listener is
//   pinned to the position), the end gateway, the moving piston itself and
//   the Hush quest fixtures. PushReaction still rules first: a block that
//   is BLOCK or unbreakable stays put, a DESTROY one (shulker box, bell,
//   decorated pot, heads) still pops.
//
// The flag is server-wide, kept per world in level.dat's `obeycraft`
// compound and mirrored to clients (WorldRulesS2C) so both sides resolve the
// same structure. Default ON.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <atomic>
#include <glm/glm.hpp>
#include <memory>

namespace Game {

    class BlockEntity;
    class BlockEntityType;
    class ILevelWrite;

    namespace PistonBlockEntities {

        inline std::atomic<bool> g_enabled{true};

        inline bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
        inline void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

        // Whether an entity of this type may ride a piston (the whitelist),
        // regardless of the rule.
        bool IsMovableType(const BlockEntityType* type);

        // PistonBaseBlock.isPushable's `!state.hasBlockEntity()` under the
        // rule: a block with an automatically-created entity is pushable
        // only while the rule is on and its type is movable.
        bool CanPushBlockWithEntity(BlockID block);

        // The rule's call on an entity found at a pushed cell: carry it
        // along (true), or leave the vanilla behaviour (false). A lazily
        // attached entity (the shared crafting table's grid) is found here
        // too — its block is pushable in vanilla.
        bool ShouldCarry(const BlockEntity& entity);

        // Puts a carried entity back into the world at `pos`. `landed` says
        // the moving cell there has just been written with the carried block
        // by its own landing. When it has, and the block there is one the
        // entity belongs to, the entity is re-homed and installed (replacing
        // the empty one the write created) and the comparators reading the
        // cell are told. Otherwise — the block broke on landing, or the cell
        // was taken by something else — on the server the entity's contents
        // are spilled at `pos` (its PreRemoveSideEffects, MC
        // Containers.dropContents) before it is discarded; a client copy
        // just goes. Null `carried` is a no-op.
        void PlaceCarried(ILevelWrite& level, const glm::ivec3& pos,
                          std::unique_ptr<BlockEntity> carried, bool landed);

    } // namespace PistonBlockEntities

} // namespace Game
