// File: src/common/network/packets/LoginStartC2S.cpp
#include "LoginStartC2S.hpp"
#include "common/network/IPacketListener.hpp"

namespace Network {

    void LoginStartC2SPacket::apply(IPacketListener& listener) {
        // Direct virtual call - no dynamic_cast needed!
        // This fixes the Windows RTTI issue
        listener.onLoginStart(*this);
    }

} // namespace Network