// File: src/server/world/storage/anvil/PlayerUuid.cpp
//
// The ONLY translation unit that touches MD5.
//
// The shim in ext/md5 is a function-like macro (CommonCrypto on macOS,
// a self-contained implementation elsewhere). Keeping it to one TU means the
// expansion cannot vary with include order, which is exactly what an inline
// version in the header did.
#include "server/world/storage/anvil/PlayerUuid.hpp"

#if defined(__APPLE__)
  // MD5 is not a security choice here and the deprecation does not apply:
  // Minecraft's offline UUID scheme IS a type-3 (MD5) UUID, so anything
  // stronger computes a different id and the player file would load nowhere
  // but in this engine.
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

// <md5/openssl/md5.h>, NOT <openssl/md5.h>.
//
// The vendored shim lives at ext/md5/openssl/md5.h and maps MD5 to
// CommonCrypto on macOS. Including it as <openssl/md5.h> requires ext/md5 on
// the include path, where Homebrew's REAL OpenSSL header
// (/opt/homebrew/include/openssl/md5.h) shadows it — Boost drags
// /opt/homebrew/include in earlier. That header declares MD5 as an extern
// function in libcrypto, which this project does not link, so the build
// compiled clean and failed at link with an undefined _MD5. Reaching the file
// through ext/ instead cannot collide with anything on the system.
#include <md5/openssl/md5.h>

namespace Game::Anvil {

    PlayerUuid OfflinePlayerUuid(const std::string& name) {
        const std::string seed = "OfflinePlayer:" + name;

        PlayerUuid uuid{};
        MD5(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(), uuid.data());

        // Java's UUID.nameUUIDFromBytes stamps the version and variant into
        // the digest. Without these two bytes the value is a plain MD5 and
        // does NOT match what Minecraft computes for the same name.
        uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0F) | 0x30);   // version 3
        uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3F) | 0x80);   // IETF variant
        return uuid;
    }

} // namespace Game::Anvil

#if defined(__APPLE__)
  #pragma clang diagnostic pop
#endif
