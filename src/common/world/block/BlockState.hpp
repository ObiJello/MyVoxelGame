// File: src/common/world/block/BlockState.hpp
//
// The engine's BlockState — port of MC's `BlockState` / `StateHolder`.
//
// WHAT THIS REPLACES
// ------------------
// A voxel used to be the pair `(BlockID, BlockStateIndex stateIndex)`, where the index
// addressed that block's OWN property list. MC has no such number, and it is
// the reason 30 vanilla blocks were impossible here: walls need 324 states,
// redstone_wire 1296, note_block 1150.
//
// MC's BlockState is an interned object, with `Block.BLOCK_STATE_REGISTRY`
// mapping it to a flat global int for storage and the wire. In a voxel engine
// that flat int is the better primary representation and is semantically
// identical — same interning, same identity comparison — so here the id IS the
// state. `ChunkSection` already stores exactly this number.
//
// TWO THINGS THIS TYPE DELIBERATELY REFUSES TO DO
// -----------------------------------------------
// 1. It does not convert to or from an integer implicitly. Constructing one
//    from a raw id needs the named `FromRawId`. Half the engine still speaks
//    the old pair; an implicit conversion would let a within-block index be
//    read as a global id at any of those sites and compile silently.
//
// 2. It does not treat 0 as "some block's default". `BlockState{}` is air's
//    default state and nothing else. The old code could say "state 0" and mean
//    "whatever the default is" because index 0 WAS the default for every
//    block. In MC it is not: `StateDefinition.any()` takes the first value of
//    every property and BooleanProperty lists `true` first, so 627 of this
//    engine's blocks default somewhere else entirely. Ask for the default with
//    `BlockStates::Default(id)`.
#pragma once

#include "Blocks.hpp"
#include "GeneratedBlockStates.hpp"

#include <cstdint>
#include <string_view>

namespace Game {

    class BlockState {
    public:
        // Air's default state. Air has exactly one state and it is global id 0,
        // which is what makes a zero-initialised voxel and an untouched palette
        // both mean "air" for free.
        constexpr BlockState() = default;

        static constexpr BlockState FromRawId(uint32_t id) {
            BlockState s;
            s.m_id = id;
            return s;
        }
        constexpr uint32_t RawId() const { return m_id; }

        // The block this state belongs to — MC's `BlockState.getBlock()`.
        BlockID Block() const;
        bool    Is(BlockID id) const { return Block() == id; }

        // This state's position within its OWN block's state list. MC has no
        // such number — its ids are global — and neither does anything here any
        // more, EXCEPT the two boundaries that must keep speaking it: the NBT
        // reader (which builds a state from a property map) and the debug
        // overlay. Everything else passes the BlockState itself.
        BlockStateIndex Index() const;

        // MC `StateHolder.hasProperty`.
        bool HasProperty(PropertyId prop) const;

        // The value's index within the property's own value list — MC's
        // `Property.getInternalIndex(getValue(prop))`. Returns -1 when this
        // block does not declare the property, which is MC throwing.
        int GetIndex(PropertyId prop) const;

        // The value as the string MC serialises ("north", "true", "7"), or an
        // empty view when the block does not declare the property. This is the
        // form the blockstate JSONs, loot conditions and NBT all speak.
        std::string_view GetName(PropertyId prop) const;

        // Look a property up by NAME rather than by identity, answering with the
        // value's name. The SLOW path — a string compare per property the block
        // declares — and it exists only because MC reuses a name across
        // properties that are NOT the same property: `facing` is FACING,
        // HORIZONTAL_FACING or FACING_HOPPER depending on the block, so a
        // caller that only wants "which way does this face" cannot name one.
        //
        // Prefer GetName(PropertyId) anywhere the property is known.
        std::string_view GetValueByName(std::string_view propName) const;

        // MC `StateHolder.setValue`. Returns the interned sibling state.
        //
        // MC needs a precomputed `neighbours` table here because its states are
        // heap objects with no arithmetic relationship. Ours are dense integers
        // in odometer order, so the sibling is one multiply away and the table
        // — 442k entries in vanilla — buys nothing. Do not "restore" it.
        //
        // Returns *this unchanged when the block does not declare the property
        // (MC's `trySetValue`) or the index is out of range.
        BlockState SetIndex(PropertyId prop, int valueIndex) const;

        // Same, by the value's serialised name.
        BlockState SetName(PropertyId prop, std::string_view value) const;

        constexpr bool operator==(const BlockState& o) const { return m_id == o.m_id; }
        constexpr bool operator!=(const BlockState& o) const { return m_id != o.m_id; }

    private:
        uint32_t m_id = 0;
    };

    static_assert(sizeof(BlockState) == 4, "BlockState must stay palette-sized");

    namespace BlockStates {

        // Builds the global tables. Eager and explicit rather than a lazy magic
        // static: the old per-state caches were lazy, could be filled before
        // the data they needed existed, and grew a hardcoded slab special case
        // to paper over it. Call once from BlockRegistry::Init.
        void Init();

        // MC `Block.defaultBlockState()`. NOT the block's first state — see the
        // header note.
        BlockState Default(BlockID id);

        // MC `getStateDefinition().getPossibleStates().size()`.
        uint32_t Count(BlockID id);

        // The block's first global id. States of one block are contiguous.
        uint32_t Base(BlockID id);

        // How many distinct states exist across every block — MC's
        // `Block.BLOCK_STATE_REGISTRY.size()`.
        uint32_t Total();

        // (block, within-block index) -> state. The index is clamped to the
        // block's count, so a save or a peer describing a state this build does
        // not model degrades to that block's last state rather than minting an
        // id that lands in the NEXT block's slice.
        //
        // This is the boundary conversion. Prefer passing BlockState around; a
        // call to this in the middle of the engine is a sign something still
        // speaks the old pair.
        BlockState FromIndex(BlockID id, BlockStateIndex stateIndex);

        // Resolve a state from the form MC uses on disk: a registry slug plus
        // property name/value strings. Unknown property names and unparseable
        // values are SKIPPED rather than rejected, exactly as
        // `NbtUtils.readBlockState` does, so a world written by a different
        // version still loads as something sensible.
        BlockState FromSlug(std::string_view slug);

        // The property table, for callers that need to enumerate rather than
        // name a property (the NBT reader, the debug overlay).
        uint16_t         PropertyCount(BlockID id);
        PropertyId       PropertyAt(BlockID id, uint16_t slot);
        std::string_view PropertyName(PropertyId prop);
        uint16_t         PropertyValueCount(PropertyId prop);
        std::string_view PropertyValueName(PropertyId prop, uint16_t valueIndex);

    } // namespace BlockStates

    // "This block, in its default state" as a within-block index — the thing
    // almost every caller means when it passes a bare BlockID. Spelled out as
    // a helper because writing the literal 0 for it was correct for years and
    // is now wrong for 627 blocks, so the mistake is easy to make by habit.
    inline BlockStateIndex DefaultStateIndexOf(BlockID id) {
        return static_cast<BlockStateIndex>(
            BlockStates::Default(id).RawId() - BlockStates::Base(id));
    }

} // namespace Game
