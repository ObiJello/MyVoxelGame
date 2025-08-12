// File: src/common/network/packets/KeepAliveC2S.cpp
#include "KeepAliveC2S.hpp"
#include "server/network/listeners/IServerPlayPacketListener.hpp"

namespace Network {

    void KeepAliveC2SPacket::apply(IPacketListener& listener) {
        // Try to cast to server play packet listener
        if (auto* playListener = dynamic_cast<Server::IServerPlayPacketListener*>(&listener)) {
            playListener->onKeepAlive(*this);
        }
        // If not a play listener, this packet shouldn't have been received
    }

} // namespace Network