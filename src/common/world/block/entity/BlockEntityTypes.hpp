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
        // ... 4..29 reserved for the other 23 MC BE types (Sign, Banner, Bed,
        // ShulkerBox, Bell, Beacon, Conduit, EndPortal, EndGateway, EnchantingTable,
        // Lectern, MobSpawner, TrialSpawner, Vault, StructureBlock, TestInstanceBlock,
        // Piston, BrushableBlock, DecoratedPot, Campfire, Skull, CopperGolemStatue,
        // Shelf, HangingSign). Added in later stages.
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
