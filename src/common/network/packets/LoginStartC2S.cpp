// File: src/common/network/packets/LoginStartC2S.cpp
#include "LoginStartC2S.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/core/Log.hpp"

namespace Network {

    void LoginStartC2SPacket::apply(IPacketListener& listener) {
        // DIAGNOSTIC: Log apply call
        Log::Info("[LoginStartC2SPacket] apply() called with username='%s', listener=%s", 
                  username.c_str(), listener.getName());
        
        // Direct virtual call - no dynamic_cast needed!
        // This fixes the Windows RTTI issue
        listener.onLoginStart(*this);
        
        Log::Debug("[LoginStartC2SPacket] apply() completed, onLoginStart() was called");
    }

} // namespace Network