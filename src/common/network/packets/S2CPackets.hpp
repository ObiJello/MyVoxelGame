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

} // namespace Packets
} // namespace Network