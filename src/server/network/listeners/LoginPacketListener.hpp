// File: src/server/network/listeners/LoginPacketListener.hpp
#pragma once

#include "ILoginPacketListener.hpp"
#include "common/network/packets/LoginStartC2S.hpp"

namespace Server {

    class ServerConnection;
    class NetworkServer;

    class LoginPacketListener : public ILoginPacketListener {
    private:
        ServerConnection& m_connection;
        NetworkServer* m_server;
        int m_compressionThreshold = -1;  // -1 = disabled
        
        // Finalize login after authentication
        void finalizeLogin(const std::string& username);
        
    public:
        LoginPacketListener(ServerConnection& connection, NetworkServer* server);
        ~LoginPacketListener() override = default;
        
        // Handle login start packet
        void onLoginStart(const Network::LoginStartC2SPacket& packet) override;
        
        // Handle encryption response (for online mode)
        void onEncryptionResponse(const Network::EncryptionResponseC2SPacket& packet) override;
        
        // Handle disconnect
        void onDisconnect(const std::string& reason) override;
        
        // Set compression threshold
        void setCompressionThreshold(int threshold) { m_compressionThreshold = threshold; }
    };

} // namespace Server