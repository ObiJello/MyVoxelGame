// File: src/client/renderer/mesh/MeshCensus.hpp
//
// Opt-in census of WHERE terrain vertices come from — launch with
// OBEY_MESH_CENSUS=1. The mesh workers count every quad they emit by block,
// layer and merged/unmerged; the main thread logs the running totals every
// 15 s as a table sorted by vertices ("[MeshCensus] ..."), with the share of
// all vertices built so far. The 2026-09-04 GPU traces put the frame in the
// vertex stage at ~0.4 ns per vertex, so this is the map of what is worth
// making cheaper: a plains section can carry more vertices in grass tufts
// than in its terrain.
//
// Zero cost when off: one cached bool per quad. Counts are what the mesher
// BUILT, which is the superset of what gets drawn; the drawn set follows the
// same proportions unless the camera looks at one biome only.
#pragma once
#include "common/world/block/Blocks.hpp"
#include <cstdint>

namespace Render::MeshCensus {

    bool Enabled();

    // Worker threads, per emitted quad (4 vertices). layer: 0 opaque,
    // 1 cutout, 2 translucent (RenderLayer's order). A greedy-merged
    // rectangle is ONE emitted quad standing for `faces` block faces; an
    // unmerged quad is one face. The table reports faces (what the world
    // has) against quads (what the GPU gets): removed% = 1 - quads/faces.
    void Count(Game::BlockID block, int layer, bool merged, uint32_t faces = 1);

    // Worker threads, at the end of each section build: hand the thread's
    // counts to the global table.
    void FlushThread();

    // Main thread, once per frame: logs the table when 15 s have passed.
    void DumpIfDue();

} // namespace Render::MeshCensus
