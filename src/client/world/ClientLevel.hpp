// File: src/client/world/ClientLevel.hpp
//
// One dimension's worth of CLIENT state — MC's ClientLevel, with the
// Immersive Portals mod's twist that several of them are alive at once.
//
// WHY THIS TYPE EXISTS
// --------------------
// Until immersive portals the client held exactly one world, and every
// world-scoped system was a process global: one chunk manager, one mesh
// manager, one chunk renderer, one block view, one of each entity manager.
// Each is keyed by ChunkPos or entity id, neither of which carries a
// dimension — so a second dimension sharing them would merge a Nether chunk
// into the Overworld chunk at the same x/z, exactly the failure the server
// side hit and fixed with ServerLevel. This is the client's ServerLevel.
//
// HOW THE GLOBALS STAY
// --------------------
// Hundreds of call sites read `g_clientChunkManager`, `g_clientMeshManager`,
// `g_chunkRenderer`, `g_clientBlockAccess` and the entity managers. Rather
// than thread a level through all of them, the globals became plain
// pointers that ClientLevels REBINDS: to the active level for gameplay and
// rendering, to the level a packet belongs to while that packet is applied
// (ClientConnection::DrainIncomingPackets), and to the level seen through a
// portal while that view renders (phase 4). This is the mod's
// `withSwitchedWorld`. Anything that caches one of those pointers across a
// rebind is wrong; the frame loop refreshes the few that do.
//
// LIFETIME
// --------
// The initial level (the dimension the session starts in) is created by
// CreateSession. Others come into being the first time a packet for that
// dimension arrives (the server sends a portal's far side before anything
// walks through) and go away either when the server says so
// (ChangeDimensionS2C without kFlagKeepPrevious, or when the client holds
// nothing for them any more — GarbageCollect) or at session end.
//
// THREADING: main thread only. Nothing here is touched by a worker; mesh
// workers carry the dimension inside their job and result instead.
#pragma once

#include "common/core/Features.hpp"
#include "common/world/level/DimensionId.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Game { class IBlockAccess; }

namespace Render {
    class ClientMeshManager;
    class ChunkRenderer;
}

namespace Client {

    class ClientChunkManager;
    class ClientBlockAccess;
    class ItemEntityManager;
    class XpOrbManager;
    class ClientMobManager;
    class ClientFallingBlocks;
#if ENABLE_IMMERSIVE_PORTALS
    class ClientImmersivePortals;
#endif
    struct RemotePlayer;

    class ClientLevel {
    public:
        explicit ClientLevel(Game::DimensionId dimension);
        ~ClientLevel();

        ClientLevel(const ClientLevel&) = delete;
        ClientLevel& operator=(const ClientLevel&) = delete;

        // Builds every subsystem in dependency order. False (with the partial
        // state torn down) if the chunk renderer could not load its shaders.
        bool Initialize();
        void Shutdown();

        Game::DimensionId Dimension() const { return m_dimension; }

        ClientChunkManager*        Chunks()        const { return m_chunks.get(); }
        ::Render::ClientMeshManager* Meshes()        const { return m_meshes.get(); }
        ::Render::ChunkRenderer*     Renderer()      const { return m_renderer.get(); }
        ClientBlockAccess*         Blocks()        const { return m_blocks.get(); }
        ItemEntityManager*         Items()         const { return m_items.get(); }
        XpOrbManager*              Orbs()          const { return m_orbs.get(); }
        ClientMobManager*          Mobs()          const { return m_mobs.get(); }
        ClientFallingBlocks*       FallingBlocks() const { return m_fallingBlocks.get(); }
#if ENABLE_IMMERSIVE_PORTALS
        ClientImmersivePortals&    Portals()       const { return *m_portals; }
#endif

        // Nothing loaded, nothing retained, nothing tracked — a level that
        // can be dropped without losing anything the server would not resend.
        bool IsEmpty() const;

        // Drop every chunk, entity and portal but keep the subsystems: the
        // level is ready to receive the same dimension again from scratch.
        void ClearContents();

        // Last time this level held something (for GarbageCollect).
        std::chrono::steady_clock::time_point LastNonEmpty() const { return m_lastNonEmpty; }
        void NoteActivity() { m_lastNonEmpty = std::chrono::steady_clock::now(); }

    private:
        Game::DimensionId m_dimension;
        std::chrono::steady_clock::time_point m_lastNonEmpty;

        std::unique_ptr<ClientChunkManager>        m_chunks;
        std::unique_ptr<::Render::ClientMeshManager> m_meshes;
        std::unique_ptr<::Render::ChunkRenderer>     m_renderer;
        std::unique_ptr<ClientBlockAccess>         m_blocks;
        std::unique_ptr<ItemEntityManager>         m_items;
        std::unique_ptr<XpOrbManager>              m_orbs;
        std::unique_ptr<ClientMobManager>          m_mobs;
        std::unique_ptr<ClientFallingBlocks>       m_fallingBlocks;
#if ENABLE_IMMERSIVE_PORTALS
        std::unique_ptr<ClientImmersivePortals>    m_portals;
#endif
    };

    // The set of levels this client holds, and which one the globals point
    // at. All static: there is one client per process, like the globals it
    // rebinds.
    class ClientLevels {
    public:
        // ── Session ─────────────────────────────────────────────────────
        // Creates the first level, makes it active and binds the globals.
        static bool CreateSession(Game::DimensionId initialDimension);
        // Shuts down and destroys every level; globals become null.
        static void DestroySession();
        static bool HasSession() { return s_active != nullptr; }

        // ── Lookup ──────────────────────────────────────────────────────
        static ClientLevel* Get(Game::DimensionId dimension);
        // Creates (and initialises) the level on first use. Null only if
        // Initialize failed, which is logged.
        static ClientLevel* GetOrCreate(Game::DimensionId dimension);
        static size_t Count();
        static void ForEach(const std::function<void(ClientLevel&)>& fn);

        // ── The active level: where the player is ───────────────────────
        static ClientLevel&       Active()          { return *s_active; }
        static Game::DimensionId  ActiveDimension() { return s_active ? s_active->Dimension() : Game::DimensionId::Overworld; }
        // Switches the player's level (creating it if needed), rebinds the
        // globals and, unless `keepPrevious`, destroys the level being left.
        // Bumps ActiveGeneration so frame-loop caches refresh.
        static void SetActive(Game::DimensionId dimension, bool keepPrevious);
        static uint32_t ActiveGeneration() { return s_activeGeneration; }

        // ── Binding: which level the globals currently point at ─────────
        static ClientLevel*      Bound()          { return s_bound; }
        static Game::DimensionId BoundDimension() { return s_bound ? s_bound->Dimension() : ActiveDimension(); }
        static void Bind(ClientLevel& level);
        static void BindActive();
        // Run `fn` with the globals bound to `dimension` (creating the level
        // if needed), then rebind whatever was bound before. The mod's
        // withSwitchedWorld. `fn` is skipped if the level cannot be created.
        static void WithLevel(Game::DimensionId dimension, const std::function<void()>& fn);

        // ── Packet scope (DimensionScopeS2C) ────────────────────────────
        // The dimension the next world-scoped packet applies to. Set by the
        // scope packet's handler and by ChangeDimensionS2C.
        static void SetPacketDimension(Game::DimensionId d) { s_packetDimension = d; }
        static Game::DimensionId PacketDimension() { return s_packetDimension; }
        // Bind the globals to the packet dimension's level, creating it if
        // this is the first packet for that dimension.
        static void BindForPacket();

        // ── Housekeeping ────────────────────────────────────────────────
        // Destroy a non-active level outright (ChangeDimensionS2C without
        // keepPrevious). No-op on the active level.
        static void Forget(Game::DimensionId dimension);
        // Clear the contents of every level (disconnect).
        static void ClearAll();
        // Destroy non-active levels that have held nothing for a while — the
        // far side of a portal the player walked away from. Cheap; once a tick.
        static void GarbageCollect();

    private:
        static ClientLevel* Create(Game::DimensionId dimension);
        static void Destroy(Game::DimensionId dimension);

        static std::unique_ptr<ClientLevel> s_levels[Game::kDimensionCount];
        static ClientLevel*      s_active;
        static ClientLevel*      s_bound;
        static uint32_t          s_activeGeneration;
        static Game::DimensionId s_packetDimension;
    };

    // Does this remote player stand in the level the globals are bound to?
    // The renderers ask this so a player in the Nether is not drawn at their
    // Nether coordinates inside the Overworld.
    bool IsRemotePlayerInBoundLevel(const RemotePlayer& player);

} // namespace Client
