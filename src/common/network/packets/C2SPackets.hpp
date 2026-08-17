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

    // MC ServerboundInteractPacket — the client's "I attacked that entity".
    // The server re-checks reach and liveness; this packet is a request, not a
    // command (see IntegratedServer::HandleInteract).
    class InteractC2SPacketImpl : public IC2SPacket {
    private:
        InteractC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;

    public:
        explicit InteractC2SPacketImpl(InteractC2SPacket data)
            : m_data(data)
            , m_timestamp(std::chrono::steady_clock::now()) {}

        void apply(IPacketListener& listener) override {
            listener.onInteractC2S(m_data);
        }

        const InteractC2SPacket& getData() const { return m_data; }

        PacketId getId() const override { return PacketId::InteractC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

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


    // ========================================================================
    // HELD ITEM CHANGE / INVENTORY CLICK / INVENTORY CLOSE / CLIENT SETTINGS
    // ========================================================================
    //
    // These four were the last C2S packets handled by the legacy raw-payload
    // registry, i.e. inline on the network I/O thread while the server thread
    // was ticking the very ServerPlayer and inventory they mutate. Giving them
    // typed representations puts them on the one path MC has: decode on the
    // network thread, apply on the server thread.

    class HeldItemChangeC2SPacketImpl : public IC2SPacket {
    private:
        HeldItemChangeC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit HeldItemChangeC2SPacketImpl(HeldItemChangeC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onHeldItemChangeC2S(m_data);
        }
        const HeldItemChangeC2SPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::HeldItemChange; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class InventoryClickC2SPacketImpl : public IC2SPacket {
    private:
        InventoryClickC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit InventoryClickC2SPacketImpl(InventoryClickC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onInventoryClickC2S(m_data);
        }
        const InventoryClickC2SPacket& getData() const { return m_data; }
        PacketId getId() const override { return PacketId::InventoryClickC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    class InventoryCloseC2SPacketImpl : public IC2SPacket {
    private:
        InventoryCloseC2SPacket m_data;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit InventoryCloseC2SPacketImpl(InventoryCloseC2SPacket data)
            : m_data(std::move(data))
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onInventoryCloseC2S(m_data);
        }
        PacketId getId() const override { return PacketId::InventoryCloseC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

    // MC ServerboundClientInformationPacket. Unlike the three above this is a
    // "common" packet in MC and its handler carries no ensureRunningOnSameThread
    // call — but ours reaches IntegratedServer::OnClientSettingsReceived, which
    // touches session view distances, so it belongs on the server thread.
    class ClientConfigC2SPacketImpl : public IC2SPacket {
    private:
        int   m_renderDistance;
        bool  m_vsync;
        float m_mouseSensitivity;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        ClientConfigC2SPacketImpl(int renderDistance, bool vsync, float mouseSensitivity)
            : m_renderDistance(renderDistance)
            , m_vsync(vsync)
            , m_mouseSensitivity(mouseSensitivity)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onClientConfigC2S(m_renderDistance, m_vsync, m_mouseSensitivity);
        }
        PacketId getId() const override { return PacketId::ClientConfigC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };


    // ========================================================================
    // ACCEPT TELEPORTATION PACKET
    // ========================================================================

    // MC ServerboundAcceptTeleportationPacket. This one is decoded rather than
    // left to the legacy registry ON PURPOSE, and the distinction is
    // load-bearing: decoded packets are queued and applied on the SERVER
    // thread, while undecoded ones run inline on the network I/O thread. The
    // ack races the login handshake — it can arrive before the server thread
    // has finished finalizeLogin and flipped the connection to PLAY — so
    // handling it off-thread meant it got dropped by the phase check and the
    // teleport gate never cleared. Queuing it puts it back in FIFO order
    // behind LoginStart, which is what MC's
    // PacketUtils.ensureRunningOnSameThread guarantees for every game packet.
    class AcceptTeleportationC2SPacketImpl : public IC2SPacket {
    private:
        int32_t m_id;
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        explicit AcceptTeleportationC2SPacketImpl(int32_t id)
            : m_id(id)
            , m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onAcceptTeleportation(m_id);
        }
        int32_t getTeleportId() const { return m_id; }
        PacketId getId() const override { return PacketId::ServerboundAcceptTeleportation; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };


    // ========================================================================
    // PLAYER LOADED PACKET
    // ========================================================================

    // MC ServerboundPlayerLoadedPacket — a `record ...()` with a unit stream
    // codec, i.e. no payload at all. The client sends it once its own level is
    // ready; the server uses it to clear the client-load timeout early.
    class PlayerLoadedC2SPacketImpl : public IC2SPacket {
    private:
        std::chrono::steady_clock::time_point m_timestamp;
    public:
        PlayerLoadedC2SPacketImpl()
            : m_timestamp(std::chrono::steady_clock::now()) {}
        void apply(IPacketListener& listener) override {
            listener.onPlayerLoaded();
        }
        PacketId getId() const override { return PacketId::PlayerLoadedC2S; }
        std::chrono::steady_clock::time_point getTimestamp() const override { return m_timestamp; }
    };

} // namespace Packets
} // namespace Network