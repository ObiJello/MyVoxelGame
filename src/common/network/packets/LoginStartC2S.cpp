// File: src/common/network/packets/LoginStartC2S.cpp
#include "LoginStartC2S.hpp"
#include "server/network/listeners/ILoginPacketListener.hpp"

namespace Network {

    void LoginStartC2SPacket::apply(IPacketListener& listener) {
        if (auto* loginListener = dynamic_cast<Server::ILoginPacketListener*>(&listener)) {
            loginListener->onLoginStart(*this);
        }
    }

} // namespace Network