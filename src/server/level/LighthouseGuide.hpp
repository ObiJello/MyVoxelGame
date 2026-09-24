// File: src/server/level/LighthouseGuide.hpp
//
// Lighthouses that guide (docs/the-hush.md, "Hush Lighthouses"): within
// kGuideRange blocks of an Aurelith, a Hush lighthouse's beam now and then
// stops and holds, pointing toward the Lantern City, for a few seconds. The
// client draws it (HushLighthouseRenderer's guide stroke); what it needs from
// the server is WHERE the city is, and that is this module's one job.
//
// ── The lookup ──────────────────────────────────────────────────────────
// Aurelith is placed by random_spread (spacing 96 chunks): every spacing cell
// has one potential start chunk. For a lamp, the cells whose potential chunk
// could lie within reach (kGuideRange plus a margin for the start chunk's
// offset from the Heart) are tried; for each, the structure's own generation
// runs for that chunk (Structures::generate, exactly what /locate's
// StructureGeneratesAt asks: the biome check, the level-site check, the
// jigsaw layout). A valid start's START piece is the city's centre column,
// the resonance engine at its middle, so the piece's bounding-box centre is
// the Heart's x/z (within a block, by rotation). The nearest such Heart
// within kGuideRange is the lamp's guide.
//
// A city's generation costs tens of milliseconds, so results are cached per
// potential start chunk for the session (a spacing cell answers every lamp in
// it), and lamps are looked up at most one per server tick, from a queue.
// The answer is fixed by the world seed, so it is looked up once per lamp per
// WORLD: it is written into the lamp's block entity
// (HushLighthouseLampBlockEntity: checked + the Heart), sent to the chunk's
// watchers (World::BlockEntityChanged) and saved with the chunk.
//
// ── When ────────────────────────────────────────────────────────────────
// NoteChunkLoaded, from IntegratedServer::ProcessAsyncChunkResults: every
// lamp in an arriving Hush chunk whose entity has not been checked is queued.
// Tick, from the per-level tick: pops one and answers it. A lamp whose chunk
// unloaded meanwhile is dropped; it is queued again the next time it loads.
// Lamps that arrive without a chunk load (placed by a player, /setblock) are
// found by Tick's slow sweep of the chunks round each player in the Hush
// (every 10 s, four chunks each way).
//
// Server thread only.
#pragma once

#include "common/world/math/WorldMath.hpp"

namespace Game { class Chunk; }

namespace Server {

    class ServerLevel;

    namespace LighthouseGuide {

        // A lamp guides only toward a Heart within this many blocks
        // (horizontally).
        inline constexpr double kGuideRange = 600.0;

        void NoteChunkLoaded(ServerLevel& level, Game::Math::ChunkPos pos, const Game::Chunk& chunk);
        void Tick(ServerLevel& level);

    } // namespace LighthouseGuide

} // namespace Server
