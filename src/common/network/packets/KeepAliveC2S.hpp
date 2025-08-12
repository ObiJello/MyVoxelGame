// File: src/common/network/packets/KeepAliveC2S.hpp
#pragma once

#include "common/network/IPacket.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/network/PacketRegistry.hpp"

namespace Server {
    class IServerPlayPacketListener;
}

namespace Network {

    class KeepAliveC2SPacket : public IC2SPacket {
    public:
        uint64_t keepAliveId;
        
        // Constructor for creating from network data
        explicit KeepAliveC2SPacket(PacketReader& reader) {
            keepAliveId = reader.ReadLong();
        }
        
        // Constructor for creating programmatically
        explicit KeepAliveC2SPacket(uint64_t id)
            : keepAliveId(id) {}
        
        PacketId getId() const override { return PacketId::KeepAliveC2S; }
        
        std::chrono::steady_clock::time_point getTimestamp() const override {
            return std::chrono::steady_clock::now();
        }
        
        // Visitor pattern - apply to listener
        void apply(IPacketListener& listener) override;
    };
    
    // Simple implementation class for legacy compatibility
    class KeepAliveC2SPacketImpl : public KeepAliveC2SPacket {
    public:
        explicit KeepAliveC2SPacketImpl(uint64_t id) 
            : KeepAliveC2SPacket(id) {}
        
        void apply(IPacketListener& listener) override {
            // Will be implemented to call listener.onKeepAlive(*this)
            // For now, no-op until we update the listener interface
        }
    };

} // namespace Network