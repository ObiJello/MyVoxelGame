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


} // namespace Packets
} // namespace Network