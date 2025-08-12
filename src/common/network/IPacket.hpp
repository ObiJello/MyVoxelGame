// File: src/common/network/IPacket.hpp
#pragma once

#include "PacketRegistry.hpp"
#include <memory>
#include <chrono>

namespace Network {
    
    // Forward declaration
    class IPacketListener;

    // Base interface for all packets - visitor pattern for type-safe handling
    class IPacket {
    public:
        virtual ~IPacket() = default;
        
        // Get the packet ID
        virtual PacketId getId() const = 0;
        
        // Get timestamp when packet was received
        virtual std::chrono::steady_clock::time_point getTimestamp() const = 0;
    };

    // Server-to-Client packet interface
    class IS2CPacket : public IPacket {
    public:
        // Apply this packet to the listener (visitor pattern)
        virtual void apply(IPacketListener& listener) = 0;
    };

    // Client-to-Server packet interface  
    class IC2SPacket : public IPacket {
    public:
        // Apply this packet to the listener (visitor pattern)
        virtual void apply(IPacketListener& listener) = 0;
    };

    // Type aliases
    using PacketPtr = std::unique_ptr<IPacket>;
    using S2CPacketPtr = std::unique_ptr<IS2CPacket>;
    using C2SPacketPtr = std::unique_ptr<IC2SPacket>;

    // Incoming packet wrapper for queueing
    struct IncomingPacket {
        PacketPtr packet;
        std::chrono::steady_clock::time_point timestamp;
        
        IncomingPacket() = default;
        IncomingPacket(PacketPtr p) 
            : packet(std::move(p))
            , timestamp(std::chrono::steady_clock::now()) {}
    };

    // Outgoing packet wrapper for queueing
    struct OutgoingPacket {
        uint8_t packetId;
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point timestamp;
        
        OutgoingPacket() = default;
        OutgoingPacket(uint8_t id, std::vector<uint8_t> d)
            : packetId(id)
            , data(std::move(d))
            , timestamp(std::chrono::steady_clock::now()) {}
    };

} // namespace Network