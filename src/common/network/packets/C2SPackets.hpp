// File: src/common/network/packets/C2SPackets.hpp
#pragma once

#include "../IPacket.hpp"
#include "../PacketTypes.hpp"
#include "../IPacketListener.hpp"
#include "common/core/Log.hpp"

// This file contains packet implementation classes for C2S packets that don't have
// their own dedicated files. For packets like HandshakeC2SPacket, LoginStartC2SPacket,
// and KeepAliveC2SPacket, see their respective header files.

namespace Network {
namespace Packets {

    // ========================================================================
    // USE ITEM ON PACKET
    // ========================================================================
    
    class UseItemOnC2SPacketImpl : public IC2SPacket {
    private:
        UseItemOnC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit UseItemOnC2SPacketImpl(UseItemOnC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            // Just forward to the listener - it will handle validation
            listener.onUseItemOnC2S(m_data);
        }
        
        const UseItemOnC2SPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::UseItemOnC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // USE ITEM PACKET (use in air — no block target)
    // ========================================================================

    class UseItemC2SPacketImpl : public IC2SPacket {
    private:
        UseItemC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit UseItemC2SPacketImpl(UseItemC2SPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onUseItemC2S(m_data);
        }

        const UseItemC2SPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::UseItem; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // PLAYER ACTION PACKET (release use / drop / swap offhand / dig stages)
    // ========================================================================

    class PlayerActionC2SPacketImpl : public IC2SPacket {
    private:
        PlayerActionC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit PlayerActionC2SPacketImpl(PlayerActionC2SPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onPlayerActionC2S(m_data);
        }

        const PlayerActionC2SPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::PlayerAction; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // PLAYER ABILITIES PACKET (client fly-state toggle)
    // ========================================================================

    class PlayerAbilitiesC2SPacketImpl : public IC2SPacket {
    private:
        PlayerAbilitiesC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit PlayerAbilitiesC2SPacketImpl(PlayerAbilitiesC2SPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onPlayerAbilitiesC2S(m_data);
        }

        const PlayerAbilitiesC2SPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::PlayerAbilitiesC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // BLOCK ACTION PACKET (existing)
    // ========================================================================
    
    class BlockActionC2SPacketImpl : public IC2SPacket {
    private:
        BlockActionC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit BlockActionC2SPacketImpl(BlockActionC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onBlockActionC2S(m_data);
        }
        
        const BlockActionC2SPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::BlockActionC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // PLAYER MOVE PACKET
    // ========================================================================
    
    class PlayerMoveC2SPacketImpl : public IC2SPacket {
    private:
        PlayerMoveC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit PlayerMoveC2SPacketImpl(PlayerMoveC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onPlayerMoveC2S(m_data);
        }
        
        const PlayerMoveC2SPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::PlayerMoveC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // ========================================================================
    // CHAT MESSAGE PACKET
    // ========================================================================
    
    class ChatMessageC2SPacketImpl : public IC2SPacket {
    private:
        ChatMessageC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
        
    public:
        explicit ChatMessageC2SPacketImpl(ChatMessageC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        
        void apply(IPacketListener& listener) override {
            listener.onChatMessageC2S(m_data);
        }
        
        const ChatMessageC2SPacket& getData() const { return m_data; }
        
        PacketId getId() const override { return PacketId::ChatMessageC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };


    // ========================================================================
    // CHUNK BATCH ACK PACKET
    // ========================================================================

    class ChunkBatchAckC2SPacketImpl : public IC2SPacket {
    private:
        float m_desiredRate;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit ChunkBatchAckC2SPacketImpl(float desiredRate)
            : m_desiredRate(desiredRate)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onChunkBatchAck(m_desiredRate);
        }
        float getDesiredRate() const { return m_desiredRate; }
        PacketId getId() const override { return PacketId::ChunkBatchAckC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

} // namespace Packets
} // namespace Network