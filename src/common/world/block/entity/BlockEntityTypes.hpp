// File: src/common/world/block/entity/BlockEntityTypes.hpp
//
// Static registry of all BlockEntityType instances. Mirrors MC
// `BlockEntityTypes.java`. Type ids are STABLE: append-only ordering matches
// the enum below so wire-format + future save-format ids never shift.
//
// Lookup paths used elsewhere:
//   - World::SetBlock      → ForBlock(blockId)   → BE create/destroy
//   - PacketHandler        → ForId(typeId)       → reconstruct from wire
//   - Future Anvil loader  → ByStringId("chest") → for vanilla compat
#pragma once

#include "BlockEntityType.hpp"
#include "../Blocks.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace Game {

    namespace BlockEntityTypeIds {
        // Append-only. Don't reorder. The values are wire-stable.
        // For Stage 1 we only need the chest family; the rest will be added
        // as their renderers ship. The numeric gap is fine — type id is
        // sparse, not contiguous.
        constexpr uint16_t CHEST         = 1;
        constexpr uint16_t TRAPPED_CHEST = 2;
        constexpr uint16_t ENDER_CHEST   = 3;
        // Container block entities (they all store items, so they all need a BE).
        constexpr uint16_t BARREL        = 4;
        constexpr uint16_t SHULKER_BOX   = 5;
        constexpr uint16_t DISPENSER     = 6;
        constexpr uint16_t DROPPER       = 7;
        constexpr uint16_t HOPPER        = 8;
        constexpr uint16_t FURNACE       = 9;
        constexpr uint16_t BLAST_FURNACE = 10;
        constexpr uint16_t SMOKER        = 11;
        constexpr uint16_t BREWING_STAND = 12;
        constexpr uint16_t BEACON        = 13;
        constexpr uint16_t CRAFTER       = 14;
        constexpr uint16_t CAMPFIRE      = 15;
        constexpr uint16_t SOUL_CAMPFIRE = 16;
        // ... 17..29 reserved for the remaining MC BE types (Sign, Banner, Bed,
        // Bell, Conduit, EndPortal, EndGateway, EnchantingTable, Lectern,
        // MobSpawner, TrialSpawner, Vault, StructureBlock, TestInstanceBlock,
        // Piston, BrushableBlock, DecoratedPot, Skull,
        // CopperGolemStatue, Shelf, HangingSign). Added in later stages.
        constexpr uint16_t MAX_ID        = 64;
    }

    class BlockEntityTypes {
    public:
        // Initialise the registry. Idempotent (safe to call from both client +
        // server startup). Called from BlockRegistry::Init right after blocks
        // are registered (BE types reference BlockIDs).
        static void Initialize();

        // True iff the given block has an associated BE type.
        static bool HasBlockEntity(BlockID id);

        // → BlockEntityType for a given block, or nullptr.
        static const BlockEntityType* ForBlock(BlockID id);

        // → BlockEntityType by wire type id, or nullptr.
        static const BlockEntityType* ForId(uint16_t typeId);

        // → BlockEntityType by save/debug id ("chest"), or nullptr.
        static const BlockEntityType* ByStringId(const std::string& stringId);

    private:
        BlockEntityTypes() = delete;

        // Per-type storage (sparse — indexed by typeId).
        static std::array<const BlockEntityType*, BlockEntityTypeIds::MAX_ID> s_byId;
        // Per-BlockID quick lookup; null entry means "no BE".
        static std::vector<const BlockEntityType*> s_byBlockId;
        static bool s_initialised;
    };

} // namespace Game
