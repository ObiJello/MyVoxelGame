// File: src/common/network/IPacketListener.hpp
#pragma once

#include "common/core/Features.hpp"

#include <string>
#include <vector>
#include <cstdint>

namespace Network {

    // Forward declarations for packet types
    struct ChunkDataS2CPacket;
    struct UnloadChunkS2CPacket;
    struct BlockChangeS2CPacket;
    struct ClientboundBlockUpdateS2CPacket;
    struct ClientboundSectionBlocksUpdateS2CPacket;
    struct MultiBlockChangeS2CPacket;
    struct PlayerUpdateS2CPacket;
    struct RemoveEntitiesS2CPacket;
    struct ItemEntitySpawnS2CPacket;
    struct ItemEntityMoveS2CPacket;
    struct TakeItemEntityS2CPacket;
    struct AddEntityS2CPacket;
    struct MoveEntityS2CPacket;
    struct EntityPositionSyncS2CPacket;
    struct SetEntityMotionS2CPacket;
    struct SetEntityDataS2CPacket;
    struct EntityEventS2CPacket;
    struct HurtAnimationS2CPacket;
    struct TickingStateS2CPacket;
    struct TickingStepS2CPacket;
    struct InteractC2SPacket;
    struct HotbarSyncS2CPacket;
    struct InventoryFullS2CPacket;
    struct InventorySetSlotS2CPacket;
    struct InventorySetCarriedS2CPacket;
    struct OpenScreenS2CPacket;
    struct ContainerSetDataS2CPacket;
    struct SetHealthS2CPacket;
    struct BlockChangedAckS2CPacket;
    struct PlayerAbilitiesS2CPacket;
#if ENABLE_PORTAL_GUN
    struct PortalSetS2CPacket;
    struct PortalRemoveS2CPacket;
    struct PortalTeleportFlashS2CPacket;
    struct PortalFizzleS2CPacket;
#endif

    // C2S packet types
    struct UseItemOnC2SPacket;
    struct UseItemC2SPacket;
    struct PlayerActionC2SPacket;
    struct PlayerAbilitiesC2SPacket;
    struct BlockActionC2SPacket;
    struct PlayerMoveC2SPacket;
    struct ChatMessageC2SPacket;
    struct HeldItemChangeC2SPacket;
    struct InventoryClickC2SPacket;
    struct InventoryCloseC2SPacket;
    class LoginStartC2SPacket;
    class HandshakeC2SPacket;
    class KeepAliveC2SPacket;

    // Base interface for all packet listeners
    // This follows Minecraft's visitor pattern for type-safe packet handling
    class IPacketListener {
    public:
        virtual ~IPacketListener() = default;
        
        // Get the name of this listener for debugging
        virtual const char* getName() const = 0;
        
        // ========================================================================
        // SERVER → CLIENT PACKET HANDLERS
        // ========================================================================
        
        // Chunk management
        virtual void onChunkDataS2C(const ChunkDataS2CPacket& packet) {}
        virtual void onUnloadChunkS2C(const UnloadChunkS2CPacket& packet) {}
        virtual void onChunkBatchStart() {}
        virtual void onChunkBatchFinished(int batchSize) {}
        
        // Block updates
        virtual void onBlockChangeS2C(const BlockChangeS2CPacket& packet) {}
        virtual void onClientboundBlockUpdate(const ClientboundBlockUpdateS2CPacket& packet) {}
        virtual void onClientboundSectionBlocksUpdate(const ClientboundSectionBlocksUpdateS2CPacket& packet) {}
        virtual void onMultiBlockChangeS2C(const MultiBlockChangeS2CPacket& packet) {}
        
        // Player updates
        virtual void onPlayerUpdateS2C(const PlayerUpdateS2CPacket& packet) {}

        // Entity removal (Minecraft's ClientboundRemoveEntitiesPacket).
        // Carries BOTH players and dropped items — the handler splits them by
        // id range.
        virtual void onRemoveEntitiesS2C(const RemoveEntitiesS2CPacket& packet) {}

        // Dropped items
        virtual void onItemEntitySpawnS2C(const ItemEntitySpawnS2CPacket& packet) {}
        virtual void onItemEntityMoveS2C(const ItemEntityMoveS2CPacket& packet) {}
        virtual void onTakeItemEntityS2C(const TakeItemEntityS2CPacket& packet) {}

        // ── Mob entities ───────────────────────────────────────────────────
        virtual void onAddEntityS2C(const AddEntityS2CPacket& packet) {}
        virtual void onMoveEntityS2C(const MoveEntityS2CPacket& packet) {}
        virtual void onEntityPositionSyncS2C(const EntityPositionSyncS2CPacket& packet) {}
        virtual void onSetEntityMotionS2C(const SetEntityMotionS2CPacket& packet) {}
        virtual void onSetEntityDataS2C(const SetEntityDataS2CPacket& packet) {}
        virtual void onEntityEventS2C(const EntityEventS2CPacket& packet) {}
        virtual void onHurtAnimationS2C(const HurtAnimationS2CPacket& packet) {}
        virtual void onTickingStateS2C(const TickingStateS2CPacket& packet) {}
        virtual void onTickingStepS2C(const TickingStepS2CPacket& packet) {}

        // Inventory sync
        virtual void onHotbarSyncS2C(const HotbarSyncS2CPacket& packet) {}
        virtual void onInventoryFullS2C(const InventoryFullS2CPacket& packet) {}
        virtual void onInventorySetSlotS2C(const InventorySetSlotS2CPacket& packet) {}
        virtual void onInventorySetCarriedS2C(const InventorySetCarriedS2CPacket& packet) {}

        // Block container opened (MC ClientboundOpenScreenPacket)
        virtual void onOpenScreenS2C(const OpenScreenS2CPacket& packet) {}
        virtual void onContainerSetDataS2C(const ContainerSetDataS2CPacket& packet) {}

        // Player stats (MC ClientboundSetHealthPacket)
        virtual void onSetHealthS2C(const SetHealthS2CPacket& packet) {}

        // Block-prediction ack (MC ClientboundBlockChangedAckPacket)
        virtual void onBlockChangedAckS2C(const BlockChangedAckS2CPacket& packet) {}

        // Abilities + game mode (MC ClientboundPlayerAbilitiesPacket + CHANGE_GAME_MODE)
        virtual void onPlayerAbilitiesS2C(const PlayerAbilitiesS2CPacket& packet) {}

#if ENABLE_PORTAL_GUN
        // Portal gun (server-authoritative pair state). Default no-op handlers
        // — Phase 4 will override these on the client side to push portal
        // state into the renderer.
        virtual void onPortalSetS2C(const PortalSetS2CPacket& packet) {}
        virtual void onPortalRemoveS2C(const PortalRemoveS2CPacket& packet) {}
        virtual void onPortalTeleportFlashS2C(const PortalTeleportFlashS2CPacket& packet) {}
        virtual void onPortalFizzleS2C(const PortalFizzleS2CPacket& packet) {}
#endif

        // View distance
        virtual void onSetChunkCacheRadiusS2C(int viewDistance) {}
        // Command names this server accepts — drives chat tab-completion.
        virtual void onCommandsS2C(const std::vector<std::string>& commandNames) {}
        
        // Connection management
        virtual void onDisconnect(const std::string& reason) {}
        virtual void onKeepAlive(uint64_t id) {}
        
        // ========================================================================
        // CLIENT → SERVER PACKET HANDLERS
        // ========================================================================
        
        // Login phase
        virtual void onHandshake(const HandshakeC2SPacket& packet) {}
        virtual void onLoginStart(const LoginStartC2SPacket& packet) {}
        
        // Play phase - Block interactions
        virtual void onUseItemOnC2S(const UseItemOnC2SPacket& packet) {}
        virtual void onBlockActionC2S(const BlockActionC2SPacket& packet) {}

        // Play phase - Item use in air (MC ServerboundUseItemPacket)
        virtual void onUseItemC2S(const UseItemC2SPacket& packet) {}

        // Play phase - Player actions (MC ServerboundPlayerActionPacket:
        // release-use, drop, swap-offhand, dig stages)
        virtual void onPlayerActionC2S(const PlayerActionC2SPacket& packet) {}
        virtual void onPlayerAbilitiesC2S(const PlayerAbilitiesC2SPacket& packet) {}
        virtual void onInteractC2S(const InteractC2SPacket& packet) {}
        
        // Play phase - Player updates
        virtual void onPlayerMoveC2S(const PlayerMoveC2SPacket& packet) {}
        
        // Play phase - Chat
        virtual void onChatMessageC2S(const ChatMessageC2SPacket& packet) {}

        // Play phase - Held item change
        virtual void onHeldItemChangeC2S(const HeldItemChangeC2SPacket& packet) {}

        // Play phase - Inventory clicks
        virtual void onInventoryClickC2S(const InventoryClickC2SPacket& packet) {}
        virtual void onInventoryCloseC2S(const InventoryCloseC2SPacket& packet) {}
        
        // Play phase - Keep alive
        virtual void onKeepAliveResponse(const KeepAliveC2SPacket& packet) {}

        // Play phase - Chunk batch acknowledgment
        virtual void onChunkBatchAck(float desiredChunksPerTick) {}
    };

} // namespace Network