// File: src/common/network/packets/KeepAliveC2S.cpp
#include "KeepAliveC2S.hpp"
#include "common/network/IPacketListener.hpp"

namespace Network {

    void KeepAliveC2SPacket::apply(IPacketListener& listener) {
        // Direct virtual call - no dynamic_cast needed!
        // This fixes the Windows RTTI issue
        listener.onKeepAliveResponse(*this);
    }

} // namespace Network