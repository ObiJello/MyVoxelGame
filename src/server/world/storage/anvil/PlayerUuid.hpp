// File: src/server/world/storage/anvil/PlayerUuid.hpp
//
// Minecraft's offline-mode player UUID.
//
// Java computes it as UUID.nameUUIDFromBytes(("OfflinePlayer:" + name)
// .getBytes(UTF_8)) — a type-3 (MD5) UUID. Deriving it identically is what
// makes playerdata/<uuid>.dat written here load in an offline-mode Minecraft
// under the same username.
//
// MD5 is not a security choice and the platform deprecation warning does not
// apply: anything stronger computes a DIFFERENT id, and the file would then
// load nowhere but in this engine.
//
// Header-only and free of engine types so it can be checked against a known
// value without linking the server.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Game::Anvil {

    // 16 raw bytes, big-endian, as vanilla stores them (an int array of 4).
    using PlayerUuid = std::array<uint8_t, 16>;

    // Defined in PlayerUuid.cpp, deliberately NOT inline here.
    //
    // The MD5 shim is a function-like macro that maps to CommonCrypto on
    // macOS, which makes it sensitive to include order — an inline definition
    // pulled into thirty translation units resolved differently in some of
    // them and produced an undefined _MD5 at link time. One definition in one
    // TU removes the whole class of problem, and keeps CommonCrypto out of
    // every header that merely wants a player UUID.
    PlayerUuid OfflinePlayerUuid(const std::string& name);

    // Canonical 8-4-4-4-12 form, which is what names the file.
    inline std::string UuidToString(const PlayerUuid& uuid) {
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(36);
        for (size_t i = 0; i < uuid.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
            out.push_back(hex[uuid[i] >> 4]);
            out.push_back(hex[uuid[i] & 0x0F]);
        }
        return out;
    }

} // namespace Game::Anvil
