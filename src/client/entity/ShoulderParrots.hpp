// File: src/client/entity/ShoulderParrots.hpp
//
// The client's copy of every player's shoulder parrots — MC Player's
// DATA_SHOULDER_PARROT_LEFT / _RIGHT as ClientAvatarEntity
// .getParrotVariantOnShoulder reads them for ParrotOnShoulderLayer. Written
// by ShoulderParrotsS2C (ClientPacketHandler), read by the frame loop when it
// draws players (the local one in third person included). Keyed by player
// id; the server sends every player's pair to a joining client and every
// change to everyone, so a reused id is always overwritten on join.
//
// Client (main) thread only: packets are applied and the frame is drawn
// there.
#pragma once

#include "common/entity/ParrotDanceRange.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Client::ShoulderParrots {

    // A Parrot.Variant id (0..4), or kNone.
    inline constexpr int kNone = -1;

    struct Pair {
        int left  = kNone;
        int right = kNone;
        bool Any() const { return left != kNone || right != kNone; }
    };

    namespace Detail {
        inline std::unordered_map<uint32_t, Pair>& Map() {
            static std::unordered_map<uint32_t, Pair> map;
            return map;
        }

        // The shoulder parrots' dance (a deliberate addition — MC's
        // ParrotOnShoulderLayer never dances). Per player, since both
        // shoulders ride the same body: the jukeboxes these parrots were told
        // about, each kept while its song plays, and whether the one check of
        // the client's playing songs has run since the last reason to look.
        struct Party {
            std::vector<glm::ivec3> jukeboxes;
            bool     searched  = false;
            uint64_t lastFrame = 0;
            void Add(const glm::ivec3& pos) {
                for (const glm::ivec3& p : jukeboxes) if (p == pos) return;
                jukeboxes.push_back(pos);
            }
        };
        inline std::unordered_map<uint32_t, Party>& PartyMap() {
            static std::unordered_map<uint32_t, Party> map;
            return map;
        }

        // The parrot's dance range (a deliberate deviation from MC's 3.46 —
        // ParrotDanceRange.hpp).
        inline constexpr double kJukeboxRange = Game::kParrotDanceRange;
        inline bool InRange(const glm::ivec3& jukebox, const glm::dvec3& feet) {
            const glm::dvec3 d = glm::dvec3(jukebox) + glm::dvec3(0.5) - feet;
            return glm::dot(d, d) < kJukeboxRange * kJukeboxRange;
        }
    } // namespace Detail

    inline void Set(uint32_t playerId, int left, int right) {
        const auto clamp = [](int v) { return (v < 0 || v > 4) ? kNone : v; };
        Pair pair{clamp(left), clamp(right)};
        const bool had = Detail::Map().count(playerId) != 0;
        if (pair.Any()) {
            Detail::Map()[playerId] = pair;
            // A parrot just landed on these shoulders: look once for a song.
            if (!had) Detail::PartyMap()[playerId].searched = false;
        } else {
            Detail::Map().erase(playerId);
            Detail::PartyMap().erase(playerId);
        }
    }

    // A song started at `pos` (level event 1010): every loaded player's
    // shoulders are told, wherever they are; they dance whenever their
    // player comes within range while the song plays.
    inline void OnSongStarted(const glm::ivec3& pos) {
        for (const auto& [id, pair] : Detail::Map()) Detail::PartyMap()[id].Add(pos);
    }

    // That song stopped (1011, or its jukebox went away): forget it.
    inline void OnSongStopped(const glm::ivec3& pos) {
        for (auto& [id, party] : Detail::PartyMap()) {
            std::erase(party.jukeboxes, pos);
        }
    }

    // Every song stopped (the level went away).
    inline void OnAllSongsStopped() {
        for (auto& [id, party] : Detail::PartyMap()) party.jukeboxes.clear();
    }

    // Per drawn frame, for a player whose shoulders carry parrots: whether
    // they dance (the PARTY pose). `feet` is the player's position; `frame`
    // any counter that advances once per frame — a player not drawn in the
    // previous frame (just came into view), or whose parrots just landed,
    // takes every jukebox the client is playing once (`forEachPlaying`);
    // `isPlaying(pos)` answers whether a jukebox at `pos` still plays. Per
    // frame: one lookup and a distance check per known jukebox.
    inline bool IsDancing(uint32_t playerId, const glm::dvec3& feet, uint64_t frame,
                          const std::function<bool(const glm::ivec3&)>& isPlaying,
                          const std::function<void(const std::function<void(const glm::ivec3&)>&)>& forEachPlaying) {
        Detail::Party& party = Detail::PartyMap()[playerId];
        if (party.lastFrame + 1 < frame) party.searched = false;
        party.lastFrame = frame;
        if (!party.searched) {
            party.searched = true;
            forEachPlaying([&](const glm::ivec3& pos) { party.Add(pos); });
        }
        std::erase_if(party.jukeboxes, [&](const glm::ivec3& p) { return !isPlaying(p); });
        for (const glm::ivec3& p : party.jukeboxes) {
            if (Detail::InRange(p, feet)) return true;
        }
        return false;
    }

    inline Pair Get(uint32_t playerId) {
        const auto& map = Detail::Map();
        const auto it = map.find(playerId);
        return it != map.end() ? it->second : Pair{};
    }

    inline void Clear() {
        Detail::Map().clear();
        Detail::PartyMap().clear();
    }

} // namespace Client::ShoulderParrots
