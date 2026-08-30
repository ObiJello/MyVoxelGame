// File: src/server/level/StrongholdLocate.hpp
//
// "Where is the nearest stronghold" — the question an Eye of Ender asks.
//
// Ports the narrow slice of MC's structure-locate machinery that strongholds
// need. `ServerLevel.findNearestMapStructure(StructureTags.EYE_OF_ENDER_LOCATED,
// pos, 100, false)` in vanilla resolves a tag containing exactly one structure
// (`minecraft:stronghold`) whose placement is CONCENTRIC_RINGS — and a
// concentric-rings structure does not need a search at all, because its
// positions are precomputed from the world seed when the generator starts.
//
// So this is a nearest-of-128-known-points query, not a chunk scan, and it
// answers instantly at any distance. That is also why an eye thrown on a
// freshly created world points somewhere sensible before a single stronghold
// chunk has been generated.
#pragma once

#include <optional>
#include <glm/glm.hpp>

namespace Server {

    class ServerLevel;

    // Nearest stronghold to `around` (a world-space position), or nothing
    // when this level generates no strongholds — which is every dimension but
    // the overworld, and an overworld with structures switched off.
    //
    // The returned position is the CENTRE of the stronghold's starting chunk
    // at y = 0. MC's own answer is no more precise than this: it hands
    // `BlockPos` with y = 0 straight to `EyeOfEnder.signalTo`, whose 12-block
    // clamp makes the vertical component irrelevant.
    std::optional<glm::ivec3> FindNearestStronghold(const ServerLevel& level,
                                                    const glm::ivec3& around);

} // namespace Server
