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
        // Stick-figure colour (Game::PlayerColorId raw byte). Tail-appended to
        // the LoginStart payload by the client; old clients that don't send it
        // get colorId=0 (Default neon green).
        uint8_t colorId = 0;

        // Constructor for creating from network data
        explicit LoginStartC2SPacket(PacketReader& reader) {
            username = reader.ReadString();
            if (reader.Remaining() >= 1) {
                colorId = reader.ReadByte();
            }
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