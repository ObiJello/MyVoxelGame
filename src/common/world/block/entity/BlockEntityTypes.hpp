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
        // Skulls / mob heads. One BE type across every skull kind and both the
        // floor and wall placements, exactly as MC registers
        // BlockEntityType.SKULL against all 14 blocks. The BE exists so the
        // SkullBlockRenderer has something to render — the skull BLOCK model is
        // empty in vanilla, all visuals are the block-entity renderer's.
        constexpr uint16_t SKULL         = 17;
        // 18 stays reserved for EndPortal (the End portal renders off a
        // per-chunk block index today, not a BE — see EndPortalRenderer).
        // MC TheEndGatewayBlockEntity: age / cooldown / cached exit position.
        constexpr uint16_t END_GATEWAY   = 19;
        // MC BlockEntityType.SIGN / HANGING_SIGN — the text on the board
        // (SignBlockEntity). The board itself is chunk-mesh geometry from
        // the 26.3 block models; the renderer draws only the text.
        constexpr uint16_t SIGN          = 20;
        constexpr uint16_t HANGING_SIGN  = 21;
        // MC BlockEntityTypes.COMPARATOR — the comparator's output signal.
        constexpr uint16_t COMPARATOR    = 22;
        // MC BlockEntityTypes.DAYLIGHT_DETECTOR — a data-less ticker.
        constexpr uint16_t DAYLIGHT_DETECTOR = 23;
        // MC BlockEntityTypes.PISTON — the moving-block cell's carried state.
        constexpr uint16_t PISTON        = 24;
        // Engine block entity (The Hush): the lighthouse lamp. Data-less —
        // it exists so HushLighthouseRenderer has something to draw the
        // sweeping beams from, as MC's BeaconRenderer hangs off its BE.
        constexpr uint16_t HUSH_LIGHTHOUSE_LAMP = 25;
        // MC BlockEntityTypes.LECTERN — the book on a lectern and its open
        // page (LecternBlockEntity).
        constexpr uint16_t LECTERN       = 26;
        // MC BlockEntityTypes.MOB_SPAWNER — the monster spawner's BaseSpawner
        // (SpawnerBlockEntity): what it spawns, its delay and its limits.
        constexpr uint16_t MOB_SPAWNER   = 27;
        // MC BlockEntityTypes.POTENT_SULFUR — the geyser's countdown and
        // eruption clock, and its tickers (PotentSulfurBlockEntity).
        constexpr uint16_t POTENT_SULFUR = 28;
        // Engine block entities (Aurelith, the Lantern City). Data-less, like
        // the lamp: the Heart's hanging rings (AurelithHeartRenderer) and a
        // gate tower's sky beam (VoiceBeaconRenderer) are functions of game
        // time, position and (the beacon) facing. Taken from 40 up so the
        // 20..29 band stays free for the remaining vanilla types.
        constexpr uint16_t RESONANCE_ENGINE = 40;
        constexpr uint16_t VOICE_BEACON     = 41;
        // Aurelith's quest (AurelithBlockEntities.hpp): the Podium's chord
        // sockets (the seated key), the pedestals (the item on show) and the
        // Hall of Instruments' tuned cabinet. The engine's own entity
        // (RESONANCE_ENGINE) carries the city's rotation since this drop.
        constexpr uint16_t CHORD_SOCKET     = 42;
        constexpr uint16_t VOICE_PEDESTAL   = 43;
        constexpr uint16_t CHOIR_CABINET    = 44;
        // ... 20..29 reserved for the remaining MC BE types (Sign, Banner,
        // Bed, Bell, Conduit, EnchantingTable, Lectern, MobSpawner,
        // TrialSpawner, Vault, StructureBlock, TestInstanceBlock, Piston,
        // BrushableBlock, DecoratedPot, CopperGolemStatue, Shelf,
        // HangingSign). Added in later stages.
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
