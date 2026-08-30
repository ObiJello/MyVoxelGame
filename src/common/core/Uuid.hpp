// File: src/common/core/Uuid.hpp
//
// A 128-bit entity identity, in the byte order Minecraft stores it.
//
// Vanilla writes a UUID as a TAG_Int_Array of four big-endian ints
// (UUIDUtil.CODEC = Codec.INT_STREAM fixed at 4), which is byte-for-byte the
// same as this 16-byte big-endian array. Keeping ONE representation means the
// NBT conversion is a reinterpretation rather than a format decision made
// twice.
//
// Distinct from the ENTITY ID (Entity::GetId, int32_t): that is a per-level,
// per-session handle reset on every launch and reused freely, so it can never
// name an entity across a save. This can.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Game {

    using Uuid = std::array<uint8_t, 16>;

    // RFC 4122 version 4 (random). Not JavaRandom: that is a determinism tool
    // for worldgen with 48 bits of state, which is not an acceptable source of
    // identity.
    Uuid RandomUuid();

    inline bool UuidIsNil(const Uuid& u) {
        for (uint8_t b : u) if (b != 0) return false;
        return true;
    }

    // int[i] = bytes[4i..4i+3], big-endian — vanilla's split.
    inline void UuidToIntArray(const Uuid& u, int32_t out[4]) {
        for (int i = 0; i < 4; ++i) {
            out[i] = static_cast<int32_t>(
                (uint32_t(u[i * 4 + 0]) << 24) | (uint32_t(u[i * 4 + 1]) << 16) |
                (uint32_t(u[i * 4 + 2]) << 8)  |  uint32_t(u[i * 4 + 3]));
        }
    }

    inline Uuid UuidFromIntArray(const int32_t in[4]) {
        Uuid u{};
        for (int i = 0; i < 4; ++i) {
            const uint32_t w = static_cast<uint32_t>(in[i]);
            u[i * 4 + 0] = static_cast<uint8_t>(w >> 24);
            u[i * 4 + 1] = static_cast<uint8_t>(w >> 16);
            u[i * 4 + 2] = static_cast<uint8_t>(w >> 8);
            u[i * 4 + 3] = static_cast<uint8_t>(w);
        }
        return u;
    }

    // Canonical 8-4-4-4-12, for logs and file names.
    inline std::string UuidToString(const Uuid& u) {
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(36);
        for (size_t i = 0; i < u.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
            out.push_back(hex[u[i] >> 4]);
            out.push_back(hex[u[i] & 0x0F]);
        }
        return out;
    }

    struct UuidHash {
        size_t operator()(const Uuid& u) const noexcept {
            // FNV-1a over 16 bytes. The values are already random, so this
            // only needs to fold them, not diffuse them.
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : u) { h ^= b; h *= 1099511628211ull; }
            return static_cast<size_t>(h);
        }
    };

} // namespace Game
