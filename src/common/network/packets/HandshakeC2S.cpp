// File: src/common/network/packets/HandshakeC2S.cpp
#include "HandshakeC2S.hpp"
#include "server/network/listeners/IHandshakePacketListener.hpp"

namespace Network {

    void HandshakeC2SPacket::apply(IPacketListener& listener) {
        if (auto* handshakeListener = dynamic_cast<Server::IHandshakePacketListener*>(&listener)) {
            handshakeListener->onHandshake(*this);
        }
    }

} // namespace Network