// File: src/server/portal/NetherPortalGeneration.hpp
//
// Immersive frame portals: from a lit frame to a linked pair of see-through
// surfaces in two dimensions. The Immersive Portals mod's
// NetherPortalGeneration / DiligentNetherPortalForm, on this engine.
//
// ONE GENERATOR, TWO FAMILIES. The class keeps its name, but it serves
// every PortalFamily (PortalFamily.hpp): an obsidian frame lit by fire
// (Overworld <-> Nether, 8:1) and a reinforced-deepslate frame lit by an
// Echo Shard (Overworld <-> The Hush, 1:1). The FrameShape it is handed
// carries its family, and everything below — the far dimension, the frame
// block it looks for and builds, the search radii, the record kind and tag
// — is read off that. The only family-specific rule is the Hush's surface
// placement in Complete().
//
// WHAT HAPPENS WHEN A FRAME IS LIT (Game::Portals' frame-lit handler)
//   1. The fire is removed; the frame's interior stays AIR. There is no
//      portal block: the see-through surface IS the portal, and the frame
//      cells are watched instead of a placeholder block (the mod fills the
//      interior with an invisible placeholder for light and integrity; this
//      engine has no light engine, and integrity is checked below).
//   2. The far position is the frame's centre through the family's scale,
//      clamped to the far dimension's build range. The chunks around it
//      are requested and held with tickets; generation continues when they
//      are resident.
//   3. The far side is searched for an existing frame of the SAME shape
//      (any closed loop of the family's frame block, matched cell for cell)
//      or, for rectangles, the same shape at an integer scale — the mod's
//      "adaptive" matching, minus rotations. Failing that, a frame of the
//      same shape is built on the nearest solid ground (floating if there
//      is none).
//   4. Four portal records are created — the two-way, two-faced cluster —
//      and synced to every client that holds either chunk.
//
// INTEGRITY: every ~233 ticks (staggered by id) each frame portal's frame
// is re-derived from its record and checked: frame all frame blocks,
// interior all air. Breaking either removes the whole cluster on both
// sides, as does mining a frame block (World::SetBlock tells this class
// directly).
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/portal/ImmersiveFrame.hpp"
#include "common/world/portal/PortalFamily.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Game { class World; }

namespace Server {

    class IntegratedServer;
    class ServerLevel;

    class NetherPortalGeneration {
    public:
        explicit NetherPortalGeneration(IntegratedServer& server);

        // A closed loop of `shape.Family()`'s frame block was lit in
        // `dimension` (fire in obsidian, an echo shard on reinforced
        // deepslate). Server thread. Starts the far-side search; returns
        // false if this frame already hosts a portal or the family cannot
        // be lit in that dimension.
        bool OnFrameLit(Game::DimensionId dimension, const Game::Immersive::FrameShape& shape);

        // Advance pending generations and run the integrity sweep.
        void Tick(int64_t serverTick);

        // A block that may be a frame block was removed (`removed` is what
        // stood there). Breaks any portal of that block's family whose frame
        // it belonged to; a no-op for a block no family builds frames of.
        void OnFrameBlockRemoved(Game::DimensionId dimension, const glm::ivec3& pos,
                                 Game::BlockID removed);

        // A chunk became resident (server thread, from the chunk result
        // queue). For every family that is immersive right now (the nether
        // while /gamerule immersive_portals is on; the Hush and the Aether
        // always — Portals::FamilyIsImmersive): vanilla portal blocks left
        // inside a frame that carries an immersive record are cleared (the
        // rule was off when the world was last saved, or the frame's chunk
        // was not loaded when the rule came back on), and a vanilla portal
        // with NO record — the same situation after its records were lost,
        // or a hush/aether frame an older build lit as blocks while the rule
        // was off — is adopted: its blocks go and it is lit as an immersive
        // portal, which links to its existing counterpart frame. Per family,
        // for the families that can be lit in this dimension.
        void OnChunkLoaded(ServerLevel& level, Game::Math::ChunkPos chunkPos);

        size_t PendingCount() const { return m_pending.size(); }
        // /gamerule immersive_portals false: forget the nether frames still
        // waiting for their far side to load. Per family, because the rule
        // is the nether's alone — a Hush or Aether frame lit a moment ago
        // keeps generating.
        void ClearPending(Game::PortalFamilyId family) {
            std::erase_if(m_pending, [family](const Pending& p) { return p.fromShape.family == family; });
        }

    private:
        struct Pending {
            Game::DimensionId from;
            Game::DimensionId to;
            Game::Immersive::FrameShape fromShape;
            glm::ivec3 toPos{0};
            std::vector<Game::Math::ChunkPos> wanted;
            int64_t startedTick = 0;
        };

        void Complete(Pending& pending, int64_t serverTick);
        bool ChunksResident(ServerLevel& level, const std::vector<Game::Math::ChunkPos>& chunks) const;

        // An existing frame the template can link to, nearest to `around`,
        // with the scale that maps template → found (1 for an exact match).
        struct Match { Game::Immersive::FrameShape shape; double scale; };
        std::optional<Match> FindExistingFrame(ServerLevel& level, const Game::Immersive::FrameShape& templ,
                                               const glm::ivec3& around, int radius) const;
        bool FrameHostsPortal(Game::DimensionId dimension, const Game::Immersive::FrameShape& shape) const;
        // A generation in flight will create or link this frame: its own
        // origin, or a far side aimed within linking range of it.
        bool PendingCovers(Game::DimensionId dimension, const Game::Immersive::FrameShape& shape) const;
        // Vanilla portal blocks (either family's) inside the frame's area
        // become air (resident cells only). Returns how many went.
        size_t ClearPortalBlocks(Game::World& world, const Game::Immersive::FrameShape& shape) const;

        std::optional<glm::ivec3> FindPlacement(ServerLevel& level, const Game::Immersive::FrameShape& templ,
                                                const glm::ivec3& around, int radius) const;
        void BuildFrame(Game::World& world, const Game::Immersive::FrameShape& shape) const;
        // The air on either side of the surface (a floor frame: the drop
        // beneath it) — carved for built AND linked frames.
        void CarveClearance(Game::World& world, const Game::Immersive::FrameShape& shape) const;
        void CreateCluster(Game::DimensionId from, const Game::Immersive::FrameShape& fromShape,
                           Game::DimensionId to, const Game::Immersive::FrameShape& toShape, double scale);
        void CheckIntegrity(int64_t serverTick);

        IntegratedServer& m_server;
        std::vector<Pending> m_pending;
    };

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
