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

} // namespace Packets
} // namespace Network