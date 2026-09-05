// File: src/server/portal/NetherPortalGeneration.hpp
//
// Immersive nether portals: from a lit obsidian frame to a linked pair of
// see-through surfaces in two dimensions. The Immersive Portals mod's
// NetherPortalGeneration / DiligentNetherPortalForm, on this engine.
//
// WHAT HAPPENS WHEN A FRAME IS LIT (Game::Portals' frame-lit handler)
//   1. The fire is removed; the frame's interior stays AIR. There is no
//      purple block: the see-through surface IS the portal, and the frame
//      cells are watched instead of a placeholder block (the mod fills the
//      interior with an invisible placeholder for light and integrity; this
//      engine has no light engine, and integrity is checked below).
//   2. The far position is the frame's centre scaled 8:1, clamped to the far
//      dimension's build range. The chunks around it are requested and held
//      with tickets; generation continues when they are resident.
//   3. The far side is searched for an existing obsidian frame of the SAME
//      shape (any closed loop, matched cell for cell) or, for rectangles, the
//      same shape at an integer scale — the mod's "adaptive" matching,
//      minus rotations. Failing that, a frame of the same shape is built on
//      the nearest solid ground (floating if there is none).
//   4. Four portal records are created — the two-way, two-faced cluster —
//      and synced to every client that holds either chunk.
//
// INTEGRITY: every ~233 ticks (staggered by id) each nether portal's frame
// is re-derived from its record and checked: frame all obsidian, interior
// all air. Breaking either removes the whole cluster on both sides, as
// does mining a frame block (World::SetBlock tells this class directly).
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/portal/ImmersiveFrame.hpp"

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

        // A fire was lit inside a closed obsidian loop in `dimension`.
        // Server thread. Starts the far-side search; returns false if this
        // frame already hosts a portal or the dimension cannot have one.
        bool OnFrameLit(Game::DimensionId dimension, const Game::Immersive::FrameShape& shape);

        // Advance pending generations and run the integrity sweep.
        void Tick(int64_t serverTick);

        // An obsidian block was removed. Breaks any nether portal whose
        // frame it belonged to.
        void OnObsidianRemoved(Game::DimensionId dimension, const glm::ivec3& pos);

        size_t PendingCount() const { return m_pending.size(); }

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
