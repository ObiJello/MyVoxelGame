// File: src/common/network/packets/S2CPackets.hpp
#pragma once

#include "../IPacket.hpp"
#include "../PacketTypes.hpp"
#include "../IPacketListener.hpp"

namespace Network {
namespace Packets {

    // ========================================================================
    // CHUNK DATA PACKET
    // ========================================================================
    
    class ChunkDataS2CPacketImpl : public IS2CPacket {
    private:
        ChunkDataS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit ChunkDataS2CPacketImpl(ChunkDataS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onChunkDataS2C(m_data);
        }
        
        const ChunkDataS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::ChunkDataS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // CHUNK UNLOAD PACKET
    // ========================================================================
    
    class UnloadChunkS2CPacketImpl : public IS2CPacket {
    private:
        UnloadChunkS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit UnloadChunkS2CPacketImpl(UnloadChunkS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onUnloadChunkS2C(m_data);
        }
        
        const UnloadChunkS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::UnloadChunkS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // BLOCK CHANGE PACKET
    // ========================================================================
    
    class BlockChangeS2CPacketImpl : public IS2CPacket {
    private:
        BlockChangeS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit BlockChangeS2CPacketImpl(BlockChangeS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onBlockChangeS2C(m_data);
        }
        
        const BlockChangeS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::BlockChangeS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // CLIENTBOUND BLOCK UPDATE PACKET
    // ========================================================================
    
    class ClientboundBlockUpdateS2CPacketImpl : public IS2CPacket {
    private:
        ClientboundBlockUpdateS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit ClientboundBlockUpdateS2CPacketImpl(ClientboundBlockUpdateS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onClientboundBlockUpdate(m_data);
        }
        
        const ClientboundBlockUpdateS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::ClientboundBlockUpdate; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };
    
    // ========================================================================
    // CLIENTBOUND SECTION BLOCKS UPDATE PACKET
    // ========================================================================
    
    class ClientboundSectionBlocksUpdateS2CPacketImpl : public IS2CPacket {
    private:
        ClientboundSectionBlocksUpdateS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit ClientboundSectionBlocksUpdateS2CPacketImpl(ClientboundSectionBlocksUpdateS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onClientboundSectionBlocksUpdate(m_data);
        }
        
        const ClientboundSectionBlocksUpdateS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::ClientboundSectionBlocksUpdate; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // CHUNK BATCH START PACKET (empty marker)
    // ========================================================================

    class ChunkBatchStartS2CPacketImpl : public IS2CPacket {
    private:
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        ChunkBatchStartS2CPacketImpl()
            : m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onChunkBatchStart(); }
        PacketId getId() const override { return PacketId::ChunkBatchStartS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // CHUNK BATCH FINISHED PACKET
    // ========================================================================

    class ChunkBatchFinishedS2CPacketImpl : public IS2CPacket {
    private:
        int m_batchSize;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit ChunkBatchFinishedS2CPacketImpl(int batchSize)
            : m_batchSize(batchSize)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onChunkBatchFinished(m_batchSize); }
        int getBatchSize() const { return m_batchSize; }
        PacketId getId() const override { return PacketId::ChunkBatchFinishedS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // MULTI BLOCK CHANGE PACKET
    // ========================================================================
    
    class MultiBlockChangeS2CPacketImpl : public IS2CPacket {
    private:
        MultiBlockChangeS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit MultiBlockChangeS2CPacketImpl(MultiBlockChangeS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onMultiBlockChangeS2C(m_data);
        }
        
        const MultiBlockChangeS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::MultiBlockChangeS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // PLAYER UPDATE PACKET
    // ========================================================================
    
    class PlayerUpdateS2CPacketImpl : public IS2CPacket {
    private:
        PlayerUpdateS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit PlayerUpdateS2CPacketImpl(PlayerUpdateS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onPlayerUpdateS2C(m_data);
        }
        
        const PlayerUpdateS2CPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::PlayerUpdateS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // REMOVE ENTITIES PACKET (Minecraft's ClientboundRemoveEntitiesPacket)
    // ========================================================================

    class RemoveEntitiesS2CPacketImpl : public IS2CPacket {
    private:
        RemoveEntitiesS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit RemoveEntitiesS2CPacketImpl(RemoveEntitiesS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onRemoveEntitiesS2C(m_data);
        }

        const RemoveEntitiesS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::EntityDestroy; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // DROPPED ITEM ENTITIES
    // ========================================================================

    class ItemEntitySpawnS2CPacketImpl : public IS2CPacket {
    private:
        ItemEntitySpawnS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit ItemEntitySpawnS2CPacketImpl(ItemEntitySpawnS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onItemEntitySpawnS2C(m_data);
        }

        const ItemEntitySpawnS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::ItemEntitySpawnS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class ItemEntityMoveS2CPacketImpl : public IS2CPacket {
    private:
        ItemEntityMoveS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit ItemEntityMoveS2CPacketImpl(ItemEntityMoveS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onItemEntityMoveS2C(m_data);
        }

        const ItemEntityMoveS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::ItemEntityMoveS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class TakeItemEntityS2CPacketImpl : public IS2CPacket {
    private:
        TakeItemEntityS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit TakeItemEntityS2CPacketImpl(TakeItemEntityS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onTakeItemEntityS2C(m_data);
        }

        const TakeItemEntityS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::TakeItemEntityS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // MOB ENTITIES (MC clientbound entity packet family)
    // ========================================================================

    class AddEntityS2CPacketImpl : public IS2CPacket {
    private:
        AddEntityS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit AddEntityS2CPacketImpl(AddEntityS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onAddEntityS2C(m_data);
        }

        const AddEntityS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::AddEntityS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class MoveEntityS2CPacketImpl : public IS2CPacket {
    private:
        MoveEntityS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit MoveEntityS2CPacketImpl(MoveEntityS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onMoveEntityS2C(m_data);
        }

        const MoveEntityS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::MoveEntityS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class EntityPositionSyncS2CPacketImpl : public IS2CPacket {
    private:
        EntityPositionSyncS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit EntityPositionSyncS2CPacketImpl(EntityPositionSyncS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onEntityPositionSyncS2C(m_data);
        }

        const EntityPositionSyncS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::EntityPositionSyncS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class SetEntityMotionS2CPacketImpl : public IS2CPacket {
    private:
        SetEntityMotionS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit SetEntityMotionS2CPacketImpl(SetEntityMotionS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onSetEntityMotionS2C(m_data);
        }

        const SetEntityMotionS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::SetEntityMotionS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class SetEntityDataS2CPacketImpl : public IS2CPacket {
    private:
        SetEntityDataS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit SetEntityDataS2CPacketImpl(SetEntityDataS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onSetEntityDataS2C(m_data);
        }

        const SetEntityDataS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::SetEntityDataS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // MC ClientboundHurtAnimationPacket — the victim's own camera tilt.
    class HurtAnimationS2CPacketImpl : public IS2CPacket {
    private:
        HurtAnimationS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit HurtAnimationS2CPacketImpl(HurtAnimationS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onHurtAnimationS2C(m_data);
        }

        const HurtAnimationS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::HurtAnimationS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class EntityEventS2CPacketImpl : public IS2CPacket {
    private:
        EntityEventS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit EntityEventS2CPacketImpl(EntityEventS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onEntityEventS2C(m_data);
        }

        const EntityEventS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::EntityEventS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // TICKING STATE / STEP (MC's two /tick packets)
    // ========================================================================

    class TickingStateS2CPacketImpl : public IS2CPacket {
    private:
        TickingStateS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit TickingStateS2CPacketImpl(TickingStateS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onTickingStateS2C(m_data);
        }

        const TickingStateS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::TickingStateS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class TickingStepS2CPacketImpl : public IS2CPacket {
    private:
        TickingStepS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit TickingStepS2CPacketImpl(TickingStepS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onTickingStepS2C(m_data);
        }

        const TickingStepS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::TickingStepS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // SIMPLE PACKETS (no data payload)
    // ========================================================================
    
    class DisconnectPacketImpl : public IS2CPacket {
    private:
        std::string m_reason;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit DisconnectPacketImpl(std::string reason)
            : m_reason(std::move(reason))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onDisconnect(m_reason);
        }
        
        const std::string& getReason() const { return m_reason; }
        
        PacketId getId() const override { return PacketId::Disconnect; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class KeepAliveS2CPacketImpl : public IS2CPacket {
    private:
        uint64_t m_id;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit KeepAliveS2CPacketImpl(uint64_t id)
            : m_id(id)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            // Call the listener's keep-alive method (virtual dispatch)
            listener.onKeepAlive(m_id);
        }
        
        uint64_t getKeepAliveId() const { return m_id; }
        
        PacketId getId() const override { return PacketId::KeepAliveS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // HOTBAR SYNC PACKET
    // ========================================================================

    class HotbarSyncS2CPacketImpl : public IS2CPacket {
    private:
        HotbarSyncS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit HotbarSyncS2CPacketImpl(HotbarSyncS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onHotbarSyncS2C(m_data);
        }

        const HotbarSyncS2CPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::HotbarSyncS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // SET CHUNK CACHE RADIUS PACKET (server tells client effective view distance)
    // ========================================================================

    class SetChunkCacheRadiusS2CPacketImpl : public IS2CPacket {
    private:
        int m_viewDistance;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit SetChunkCacheRadiusS2CPacketImpl(int viewDistance)
            : m_viewDistance(viewDistance)
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onSetChunkCacheRadiusS2C(m_viewDistance);
        }

        int getViewDistance() const { return m_viewDistance; }

        PacketId getId() const override { return PacketId::SetChunkCacheRadiusS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // COMMANDS PACKET (server tells client which commands it accepts)
    // ========================================================================

    class CommandsS2CPacketImpl : public IS2CPacket {
    private:
        std::vector<std::string> m_commandNames;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit CommandsS2CPacketImpl(std::vector<std::string> names)
            : m_commandNames(std::move(names))
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onCommandsS2C(m_commandNames);
        }

        const std::vector<std::string>& getCommandNames() const { return m_commandNames; }

        PacketId getId() const override { return PacketId::CommandsS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // INVENTORY SYNC PACKETS
    // ========================================================================

    class InventoryFullS2CPacketImpl : public IS2CPacket {
    private:
        InventoryFullS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit InventoryFullS2CPacketImpl(InventoryFullS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onInventoryFullS2C(m_data); }
        const InventoryFullS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::InventoryFullS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class InventorySetSlotS2CPacketImpl : public IS2CPacket {
    private:
        InventorySetSlotS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit InventorySetSlotS2CPacketImpl(InventorySetSlotS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onInventorySetSlotS2C(m_data); }
        const InventorySetSlotS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::InventorySetSlotS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class InventorySetCarriedS2CPacketImpl : public IS2CPacket {
    private:
        InventorySetCarriedS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit InventorySetCarriedS2CPacketImpl(InventorySetCarriedS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onInventorySetCarriedS2C(m_data); }
        const InventorySetCarriedS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::InventorySetCarriedS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class OpenScreenS2CPacketImpl : public IS2CPacket {
    private:
        OpenScreenS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit OpenScreenS2CPacketImpl(OpenScreenS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onOpenScreenS2C(m_data); }
        const OpenScreenS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::OpenScreenS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // One ContainerData index of the open menu changed (furnace burn/cook).
    class ContainerSetDataS2CPacketImpl : public IS2CPacket {
    private:
        ContainerSetDataS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit ContainerSetDataS2CPacketImpl(ContainerSetDataS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onContainerSetDataS2C(m_data); }
        const ContainerSetDataS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::ContainerSetDataS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // PLAYER STATS
    // ========================================================================

    class SetHealthS2CPacketImpl : public IS2CPacket {
    private:
        SetHealthS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit SetHealthS2CPacketImpl(SetHealthS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onSetHealthS2C(m_data); }
        const SetHealthS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::SetHealthS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // Retires client-side block predictions up to `sequence` (MC
    // ClientboundBlockChangedAckPacket).
    class BlockChangedAckS2CPacketImpl : public IS2CPacket {
    private:
        BlockChangedAckS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit BlockChangedAckS2CPacketImpl(BlockChangedAckS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onBlockChangedAckS2C(m_data); }
        const BlockChangedAckS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::BlockChangedAckS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class PlayerAbilitiesS2CPacketImpl : public IS2CPacket {
    private:
        PlayerAbilitiesS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit PlayerAbilitiesS2CPacketImpl(PlayerAbilitiesS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onPlayerAbilitiesS2C(m_data); }
        const PlayerAbilitiesS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::PlayerAbilities; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

#if ENABLE_PORTAL_GUN
    class PortalSetS2CPacketImpl : public IS2CPacket {
    private:
        PortalSetS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit PortalSetS2CPacketImpl(PortalSetS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onPortalSetS2C(m_data); }
        const PortalSetS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::PortalSetS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class PortalRemoveS2CPacketImpl : public IS2CPacket {
    private:
        PortalRemoveS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit PortalRemoveS2CPacketImpl(PortalRemoveS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onPortalRemoveS2C(m_data); }
        const PortalRemoveS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::PortalRemoveS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class PortalTeleportFlashS2CPacketImpl : public IS2CPacket {
    private:
        PortalTeleportFlashS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit PortalTeleportFlashS2CPacketImpl(PortalTeleportFlashS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onPortalTeleportFlashS2C(m_data); }
        const PortalTeleportFlashS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::PortalTeleportFlashS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class PortalFizzleS2CPacketImpl : public IS2CPacket {
    private:
        PortalFizzleS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit PortalFizzleS2CPacketImpl(PortalFizzleS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onPortalFizzleS2C(m_data); }
        const PortalFizzleS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::PortalFizzleS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };
#endif

    // ========================================================================
    // THE LAST OF THE LEGACY-REGISTRY PACKETS
    // ========================================================================
    //
    // These eight were the final S2C packets without a typed representation,
    // which meant they were dispatched by raw payload id. MC has no such path —
    // every packet is a type and every PLAY-phase client handler runs on the
    // main thread via PacketUtils.ensureRunningOnSameThread (verified for all
    // six equivalents in ClientPacketListener). Giving them types is what
    // retires the second dispatch mechanism entirely.
    //
    // Their bodies stay on ClientConnection because they read its state
    // (world age, spawn position, player id, the chat/time/teleport callbacks).
    // That mirrors MC, whose ClientPacketListener holds `this.connection` and
    // reaches through it for exactly the same reason.

    class ChatMessageS2CPacketImpl : public IS2CPacket {
    private:
        ChatMessageS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit ChatMessageS2CPacketImpl(ChatMessageS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onChatMessageS2C(m_data); }
        const ChatMessageS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::ChatMessageS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class TimeUpdateS2CPacketImpl : public IS2CPacket {
    private:
        TimeUpdateS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit TimeUpdateS2CPacketImpl(TimeUpdateS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onTimeUpdateS2C(m_data); }
        const TimeUpdateS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::TimeUpdate; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class WorldSpawnS2CPacketImpl : public IS2CPacket {
    private:
        WorldSpawnS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit WorldSpawnS2CPacketImpl(WorldSpawnS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onWorldSpawnS2C(m_data); }
        const WorldSpawnS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::WorldSpawn; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class PlayerInfoS2CPacketImpl : public IS2CPacket {
    private:
        PlayerInfoS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit PlayerInfoS2CPacketImpl(PlayerInfoS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onPlayerInfoS2C(m_data); }
        const PlayerInfoS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::PlayerInfoS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class ClientboundPlayerPositionPacketImpl : public IS2CPacket {
    private:
        ClientboundPlayerPositionPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit ClientboundPlayerPositionPacketImpl(ClientboundPlayerPositionPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onClientboundPlayerPosition(m_data); }
        const ClientboundPlayerPositionPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::ClientboundPlayerPosition; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class BlockEntityDataS2CPacketImpl : public IS2CPacket {
    private:
        BlockEntityDataS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit BlockEntityDataS2CPacketImpl(BlockEntityDataS2CPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onBlockEntityDataS2C(m_data); }
        const BlockEntityDataS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::BlockEntityDataS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class BlockEntityRemoveS2CPacketImpl : public IS2CPacket {
    private:
        BlockEntityRemoveS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit BlockEntityRemoveS2CPacketImpl(BlockEntityRemoveS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onBlockEntityRemoveS2C(m_data); }
        const BlockEntityRemoveS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::BlockEntityRemoveS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // Block events (MC ClientboundBlockEventPacket). The listener default is a
    // no-op — the per-BE TriggerEvent hook lands with ChestLidController — but
    // the id is claimed here rather than left to the raw path, so the dispatch
    // is already in place when that arrives.
    class BlockEntityActionS2CPacketImpl : public IS2CPacket {
    private:
        BlockEntityActionS2CPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit BlockEntityActionS2CPacketImpl(BlockEntityActionS2CPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override { listener.onBlockEntityActionS2C(m_data); }
        const BlockEntityActionS2CPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::BlockEntityActionS2C; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

} // namespace Packets
} // namespace Network