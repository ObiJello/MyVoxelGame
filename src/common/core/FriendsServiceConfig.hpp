// File: src/common/core/FriendsServiceConfig.hpp
//
// Shared defaults for the ObeyCraft friends service (accounts, friends
// graph, presence, invites). Included by BOTH the launcher (HTTP client)
// and the game (NDJSON client) so the two always agree on where the
// service lives.
//
// The backend is tools/friends_server/friends_service.py, self-hosted on
// the same machine that hosts the public game server. One port serves
// both protocols (first-byte sniffed).
//
// Overrides: launcher.json key "friends_service" ("host" or "host:port"),
// which the launcher also forwards to the game via --friends-service.
// The HOSTING machine itself should typically override to 127.0.0.1 —
// consumer routers often can't hairpin their own public IP.
#pragma once

#include <cstdint>

namespace Friends {

    // NOTE: residential IPs rotate. If friends suddenly can't sign in or see
    // each other, check the current public IP (`curl https://api.ipify.org`)
    // and update this — or better, point it at a dynamic-DNS hostname so it
    // survives ISP changes. Verified 2026-07-31.
    inline constexpr const char* kDefaultServiceHost = "108.35.220.113";
    inline constexpr uint16_t    kDefaultServicePort = 25570;

    // Account-name rules — must match NAME_RE in friends_service.py.
    // [A-Za-z0-9_]{3,16}: also guarantees names survive the launcher's
    // unquoted `open --args` launch path.
    inline constexpr int kNameMinLen = 3;
    inline constexpr int kNameMaxLen = 16;

    inline constexpr bool IsValidNameChar(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    }

    inline bool IsValidName(const char* s) {
        int n = 0;
        for (; s[n]; ++n) {
            if (n >= kNameMaxLen || !IsValidNameChar(s[n])) return false;
        }
        return n >= kNameMinLen;
    }

} // namespace Friends
