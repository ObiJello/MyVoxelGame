// File: src/server/level/NetherPortalIndex.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/level/
// portal/PortalForcer.java.

#include "NetherPortalIndex.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <algorithm>

namespace Server {

    void NetherPortalIndex::Add(const glm::ivec3& pos) {
        m_positions.insert(pos);
    }

    void NetherPortalIndex::Remove(const glm::ivec3& pos) {
        m_positions.erase(pos);
    }

    void NetherPortalIndex::NoteChunkLoaded(Game::Math::ChunkPos chunkPos,
                                            const Game::Chunk& chunk) {
        if (!m_scannedChunks.insert(chunkPos).second) {
            return;   // already walked this chunk once this session
        }

        // Index range, not a population count. This used to loop to
        // GetSectionCount(), which counted how many sections EXISTED — so a
        // sparse chunk whose content sat at index 12 answered 1 and the scan
        // stopped after section 0. Every portal above the first filled section
        // was missed, which in the Nether (bedrock floor at index 4, portals
        // built well above it) meant essentially all of them.
        for (int si = 0; si < Game::Math::SECTIONS_PER_CHUNK; ++si) {
            const Game::ChunkSection* section = chunk.GetSection(si);
            if (!section || section->IsAllAir()) continue;

            // Palette-membership test first. A chunk section is 4096 voxels
            // and a world is overwhelmingly free of nether portals, so paying
            // the full walk on every section would make chunk loading measurably
            // slower for a query that almost always answers "none". The palette
            // is a few dozen entries and answers the same question.
            //
            // A section on the GLOBAL palette carries no such list, so it falls
            // through to the walk — rare, and correct.
            const auto& states = section->States();
            if (!states.IsGlobalPalette()) {
                bool mayContain = false;
                for (uint32_t raw : states.Palette()) {
                    if (Game::BlockState::FromRawId(raw).Is(Game::BlockID::NetherPortal)) {
                        mayContain = true;
                        break;
                    }
                }
                if (!mayContain) continue;
            }

            const int baseY = Game::Math::WorldCoordinates::SectionCoordsToWorldY(
                static_cast<int>(si), 0);
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        if (section->GetBlockID(x, y, z) != Game::BlockID::NetherPortal) {
                            continue;
                        }
                        m_positions.insert(glm::ivec3(chunkPos.x * 16 + x,
                                                      baseY + y,
                                                      chunkPos.z * 16 + z));
                    }
                }
            }
        }
    }

    // PortalForcer.java:43
    std::optional<glm::ivec3> NetherPortalIndex::FindClosest(const Game::World& world,
                                                             const glm::ivec3& around,
                                                             int radius) {
        std::optional<glm::ivec3> best;
        double bestDistSqr = 0.0;

        // Collected rather than erased in place: erasing from the set while
        // walking it invalidates the iterator, and a stale entry is common
        // enough (any portal broken since it was indexed) that a second pass
        // is worth having.
        std::vector<glm::ivec3> stale;

        for (const glm::ivec3& pos : m_positions) {
            // MC getInSquare — a Chebyshev box on X/Z with NO vertical bound.
            // Using a Euclidean radius here instead would quietly fail to find
            // a portal at the corner of the search area, which is exactly
            // where a scaled coordinate tends to land.
            if (std::abs(pos.x - around.x) > radius) continue;
            if (std::abs(pos.z - around.z) > radius) continue;

            // Re-validate. The index never forgets (see the header), so it is
            // the only thing standing between a portal that was mined out and
            // a player being teleported into the rock that replaced it. Only
            // possible for a LOADED chunk — an unloaded one reads as air, and
            // dropping the entry then would defeat the whole point of keeping
            // it, so an unloaded position is trusted rather than culled.
            const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            if (world.IsChunkLoaded(cp.x, cp.z) &&
                world.GetBlock(pos.x, pos.y, pos.z) != Game::BlockID::NetherPortal) {
                stale.push_back(pos);
                continue;
            }

            const double dx = static_cast<double>(pos.x - around.x);
            const double dy = static_cast<double>(pos.y - around.y);
            const double dz = static_cast<double>(pos.z - around.z);
            const double distSqr = dx * dx + dy * dy + dz * dz;

            // MC: min by distSqr, ties broken by the LOWEST Y. The tie-break
            // is what makes a three-tall portal link at its bottom cell, which
            // in turn is what the exit-rectangle maths expects.
            if (!best || distSqr < bestDistSqr ||
                (distSqr == bestDistSqr && pos.y < best->y)) {
                best = pos;
                bestDistSqr = distSqr;
            }
        }

        for (const glm::ivec3& pos : stale) m_positions.erase(pos);

        return best;
    }

} // namespace Server
