// File: src/server/level/NetherPortalIndex.hpp
//
// Where the nether portals are, per dimension. Stands in for MC's
// `PoiManager` + `PoiTypes.NETHER_PORTAL`.
//
// WHY A BESPOKE INDEX RATHER THAN A POI SYSTEM
// --------------------------------------------
// MC registers every nether_portal block state as a point of interest, and
// `PortalForcer.findClosestPortalPosition` asks the POI manager for the
// nearest one inside a square — 16 blocks when arriving in the Nether, 128
// when arriving in the Overworld. The POI manager is a large subsystem
// (per-section records, occupancy, ticket-backed loading, region persistence)
// and portals are the only thing in this engine that would use it, so this is
// the one query it answers, and nothing else.
//
// THE ENTRY THAT DOES NOT EXPIRE
// ------------------------------
// Entries are deliberately NOT dropped when a chunk unloads. MC can afford to
// forget, because its POIs are written into the region file and re-read when
// the chunk comes back; this engine has no such storage. If the index forgot,
// then walking back to your own portal — whose chunk unloaded the moment you
// left it — would find nothing and build a SECOND portal a few blocks away,
// every single crossing. Keeping a handful of block positions alive for the
// session is the cheaper half of that trade by a wide margin.
//
// The cost of never forgetting is a stale entry for a portal that was broken
// while its chunk was unloaded, which cannot actually happen (breaking it
// requires being there) — and `FindClosest` re-validates against the world
// anyway before returning a position.
#pragma once

#include "common/world/math/WorldMath.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

namespace Game {
    class World;
    class Chunk;
}

namespace Server {

    class NetherPortalIndex {
    public:
        // A nether_portal block appeared at `pos`. Called from the block-change
        // hook and by PortalForcer when it builds one.
        void Add(const glm::ivec3& pos);

        // A nether_portal block at `pos` is gone. Cheap no-op when it was
        // never indexed.
        void Remove(const glm::ivec3& pos);

        // Scan a freshly loaded or generated chunk for portal blocks.
        //
        // Runs once per chunk per session — the `m_scannedChunks` set is what
        // keeps a chunk that unloads and reloads from being walked twice, and
        // more importantly keeps the entries it found from being duplicated.
        void NoteChunkLoaded(Game::Math::ChunkPos chunkPos, const Game::Chunk& chunk);

        // MC PortalForcer.findClosestPortalPosition (PortalForcer.java:43).
        //
        // `radius` is a CHEBYSHEV distance in blocks on X/Z with no Y bound —
        // MC's `getInSquare`. It depends on the DESTINATION, not the origin:
        // 16 when arriving in the Nether, 128 when arriving in the Overworld,
        // which is what makes an overworld portal link to a nether portal up
        // to 128 blocks away but not the reverse.
        //
        // The winner is the smallest squared distance, ties broken by the
        // LOWEST Y — vanilla's `thenComparingInt(Vec3i::getY)`, which is what
        // makes a tall portal link at its base.
        //
        // Positions are re-validated against `world` before being returned, so
        // an entry for a portal that has since been broken is dropped rather
        // than teleporting somebody into solid rock.
        std::optional<glm::ivec3> FindClosest(const Game::World& world,
                                              const glm::ivec3& around, int radius);

        size_t Size() const { return m_positions.size(); }

    private:
        struct IVec3Hash {
            size_t operator()(const glm::ivec3& v) const noexcept {
                // Same mix as Math::ChunkPosHash, widened. Portal positions
                // cluster hard (a 21x21 portal is 441 adjacent cells), so a
                // naive xor of the components would pile them into one bucket.
                uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(v.x));
                h = h * 0x9E3779B97F4A7C15ull
                  + static_cast<uint64_t>(static_cast<uint32_t>(v.y));
                h = h * 0x9E3779B97F4A7C15ull
                  + static_cast<uint64_t>(static_cast<uint32_t>(v.z));
                h ^= h >> 29;
                return static_cast<size_t>(h);
            }
        };

        std::unordered_set<glm::ivec3, IVec3Hash> m_positions;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_scannedChunks;
    };

} // namespace Server
