// File: src/common/network/packets/HandshakeC2S.cpp
#include "HandshakeC2S.hpp"
#include "common/network/IPacketListener.hpp"

namespace Network {

    void HandshakeC2SPacket::apply(IPacketListener& listener) {
        // Direct virtual call - no dynamic_cast needed!
        // This fixes the Windows RTTI issue
        listener.onHandshake(*this);
    }

} // namespace Network