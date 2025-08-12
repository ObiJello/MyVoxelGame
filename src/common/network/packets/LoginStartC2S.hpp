// File: src/common/network/packets/LoginStartC2S.hpp
#pragma once

#include "common/network/IPacket.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/network/PacketRegistry.hpp"

namespace Server {
    class ILoginPacketListener;
}

namespace Network {

    class LoginStartC2SPacket : public IC2SPacket {
    public:
        std::string username;
        
        // Constructor for creating from network data
        explicit LoginStartC2SPacket(PacketReader& reader) {
            username = reader.ReadString();
        }
        
        // Constructor for creating programmatically
        explicit LoginStartC2SPacket(const std::string& name)
            : username(name) {}
        
        PacketId getId() const override { return PacketId::LoginStart; }
        
        std::chrono::steady_clock::time_point getTimestamp() const override {
            return std::chrono::steady_clock::now();
        }
        
        // Visitor pattern - apply to listener
        void apply(IPacketListener& listener) override;
    };

} // namespace Network