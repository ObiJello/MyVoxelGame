// File: src/client/world/ClientLevel.cpp
#include "ClientLevel.hpp"

#include "ClientChunkManager.hpp"
#include "ClientBlockAccess.hpp"
#include "client/entity/ItemEntityManager.hpp"
#include "client/entity/XpOrbManager.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/entity/ClientFallingBlocks.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/mesh/ClientMeshManager.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif
#include "common/physics/RayCast.hpp"
#include "common/core/Log.hpp"

namespace Client {

    // ── ClientLevel ────────────────────────────────────────────────────────

    ClientLevel::ClientLevel(Game::DimensionId dimension)
        : m_dimension(dimension)
        , m_lastNonEmpty(std::chrono::steady_clock::now()) {}

    ClientLevel::~ClientLevel() {
        Shutdown();
    }

    bool ClientLevel::Initialize() {
        Log::Info("[ClientLevel] Creating level for %s",
                  std::string(Game::DimensionName(m_dimension)).c_str());

        m_chunks = std::make_unique<ClientChunkManager>();
        m_chunks->SetDimension(m_dimension);
        m_chunks->Initialize();

        m_meshes = std::make_unique<::Render::ClientMeshManager>();
        m_meshes->Initialize(m_chunks.get());

        m_renderer = std::make_unique<::Render::ChunkRenderer>();
        if (!m_renderer->Initialize(m_chunks.get(), m_meshes.get())) {
            Log::Error("[ClientLevel] Chunk renderer failed to initialise for %s",
                       std::string(Game::DimensionName(m_dimension)).c_str());
            Shutdown();
            return false;
        }

        // The three know each other by pointer, never through the globals,
        // so a level renders and meshes correctly whichever level the
        // globals happen to be bound to.
        m_chunks->SetPeers(m_meshes.get(), m_renderer.get());
        m_meshes->SetRenderer(m_renderer.get());

        m_blocks        = std::make_unique<ClientBlockAccess>(m_chunks.get());
        m_items         = std::make_unique<ItemEntityManager>();
        m_orbs          = std::make_unique<XpOrbManager>();
        m_mobs          = std::make_unique<ClientMobManager>();
        m_fallingBlocks = std::make_unique<ClientFallingBlocks>();
#if ENABLE_IMMERSIVE_PORTALS
        m_portals       = std::make_unique<ClientImmersivePortals>();
#endif
        return true;
    }

    void ClientLevel::Shutdown() {
        // Reverse of Initialize, with the two-way dependencies respected:
        // the mesh manager's shutdown tells the renderer its GPU data is
        // gone, so the renderer must still exist; the chunk manager's
        // shutdown cancels jobs whose results the mesh manager would route.
#if ENABLE_IMMERSIVE_PORTALS
        m_portals.reset();
#endif
        m_fallingBlocks.reset();
        m_mobs.reset();
        m_orbs.reset();
        m_items.reset();
        m_blocks.reset();
        if (m_meshes)   m_meshes->Shutdown();
        if (m_chunks)   m_chunks->Shutdown();
        if (m_renderer) m_renderer->Shutdown();
        m_meshes.reset();
        m_chunks.reset();
        m_renderer.reset();
    }

    bool ClientLevel::IsEmpty() const {
        if (m_chunks && (m_chunks->GetLoadedChunkCount() > 0 || m_chunks->GetRetainedChunkCount() > 0)) return false;
        if (m_mobs && m_mobs->Count() > 0) return false;
        if (m_items && m_items->Count() > 0) return false;
        if (m_orbs && m_orbs->Count() > 0) return false;
#if ENABLE_IMMERSIVE_PORTALS
        if (m_portals && m_portals->Count() > 0) return false;
#endif
        return true;
    }

    void ClientLevel::ClearContents() {
        if (m_chunks)        m_chunks->ClearAllChunks();   // also drops the portals
        if (m_items)         m_items->Clear();
        if (m_orbs)          m_orbs->Clear();
        if (m_mobs)          m_mobs->Clear();
        if (m_fallingBlocks) m_fallingBlocks->Clear();
    }

    // ── ClientLevels ───────────────────────────────────────────────────────

    std::unique_ptr<ClientLevel> ClientLevels::s_levels[Game::kDimensionCount];
    ClientLevel*      ClientLevels::s_active           = nullptr;
    ClientLevel*      ClientLevels::s_bound            = nullptr;
    uint32_t          ClientLevels::s_activeGeneration = 0;
    Game::DimensionId ClientLevels::s_packetDimension  = Game::DimensionId::Overworld;

    namespace {
        // A level the player left has this long to be looked back into
        // (through a portal, say) before its empty shell is freed.
        constexpr auto kEmptyLevelLifetime = std::chrono::seconds(30);

        void BindGlobals(ClientLevel* level) {
            g_clientChunkManager          = level ? level->Chunks()        : nullptr;
            ::Render::g_clientMeshManager   = level ? level->Meshes()        : nullptr;
            ::Render::g_chunkRenderer       = level ? level->Renderer()      : nullptr;
            g_clientBlockAccess           = level ? level->Blocks()        : nullptr;
            g_itemEntityManager           = level ? level->Items()         : nullptr;
            g_xpOrbManager                = level ? level->Orbs()          : nullptr;
            g_clientMobManager            = level ? level->Mobs()          : nullptr;
            g_clientFallingBlocks         = level ? level->FallingBlocks() : nullptr;
            Game::SetGlobalBlockAccess(level ? level->Blocks() : nullptr);
        }
    }

    ClientLevel* ClientLevels::Create(Game::DimensionId dimension) {
        auto& slot = s_levels[Game::DimensionSlot(dimension)];
        if (slot) return slot.get();
        auto level = std::make_unique<ClientLevel>(dimension);
        if (!level->Initialize()) return nullptr;
        slot = std::move(level);
        return slot.get();
    }

    void ClientLevels::Destroy(Game::DimensionId dimension) {
        auto& slot = s_levels[Game::DimensionSlot(dimension)];
        if (!slot) return;
        if (s_bound == slot.get()) s_bound = nullptr;
        if (s_active == slot.get()) s_active = nullptr;
        Log::Info("[ClientLevel] Destroying level for %s",
                  std::string(Game::DimensionName(dimension)).c_str());
        slot->Shutdown();
        slot.reset();
        if (!s_bound) BindGlobals(s_active);
    }

    bool ClientLevels::CreateSession(Game::DimensionId initialDimension) {
        DestroySession();
        s_packetDimension = initialDimension;
        ClientLevel* level = Create(initialDimension);
        if (!level) return false;
        s_active = level;
        ++s_activeGeneration;
        Bind(*level);
        return true;
    }

    void ClientLevels::DestroySession() {
        s_bound  = nullptr;
        s_active = nullptr;
        BindGlobals(nullptr);
        for (auto& slot : s_levels) {
            if (slot) { slot->Shutdown(); slot.reset(); }
        }
        ++s_activeGeneration;
    }

    ClientLevel* ClientLevels::Get(Game::DimensionId dimension) {
        return s_levels[Game::DimensionSlot(dimension)].get();
    }

    ClientLevel* ClientLevels::GetOrCreate(Game::DimensionId dimension) {
        if (ClientLevel* level = Get(dimension)) return level;
        return Create(dimension);
    }

    size_t ClientLevels::Count() {
        size_t n = 0;
        for (const auto& slot : s_levels) if (slot) ++n;
        return n;
    }

    void ClientLevels::ForEach(const std::function<void(ClientLevel&)>& fn) {
        for (auto& slot : s_levels) if (slot) fn(*slot);
    }

    void ClientLevels::SetActive(Game::DimensionId dimension, bool keepPrevious) {
        ClientLevel* previous = s_active;
        if (previous && previous->Dimension() == dimension) return;
        ClientLevel* next = GetOrCreate(dimension);
        if (!next) {
            Log::Error("[ClientLevel] Cannot switch to %s: level failed to initialise",
                       std::string(Game::DimensionName(dimension)).c_str());
            return;
        }
        s_active = next;
        s_packetDimension = dimension;
        ++s_activeGeneration;
        Bind(*next);
        if (previous && !keepPrevious) Destroy(previous->Dimension());
    }

    void ClientLevels::Bind(ClientLevel& level) {
        if (s_bound == &level) return;
        s_bound = &level;
        BindGlobals(&level);
    }

    void ClientLevels::BindActive() {
        if (s_active) Bind(*s_active);
        else { s_bound = nullptr; BindGlobals(nullptr); }
    }

    void ClientLevels::WithLevel(Game::DimensionId dimension, const std::function<void()>& fn) {
        ClientLevel* before = s_bound;
        ClientLevel* level = GetOrCreate(dimension);
        if (!level) return;
        Bind(*level);
        fn();
        if (before) Bind(*before); else BindActive();
    }

    void ClientLevels::BindForPacket() {
        if (s_bound && s_bound->Dimension() == s_packetDimension) return;
        if (ClientLevel* level = GetOrCreate(s_packetDimension)) {
            level->NoteActivity();
            Bind(*level);
        }
    }

    void ClientLevels::Forget(Game::DimensionId dimension) {
        if (s_active && s_active->Dimension() == dimension) return;
        Destroy(dimension);
    }

    void ClientLevels::ClearAll() {
        for (auto& slot : s_levels) {
            if (!slot) continue;
            ClientLevel* before = s_bound;
            Bind(*slot);
            slot->ClearContents();
            if (before) Bind(*before); else BindActive();
        }
    }

    void ClientLevels::GarbageCollect() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& slot : s_levels) {
            if (!slot || slot.get() == s_active) continue;
            if (!slot->IsEmpty()) { slot->NoteActivity(); continue; }
            if (now - slot->LastNonEmpty() < kEmptyLevelLifetime) continue;
            Destroy(slot->Dimension());
        }
    }

    bool IsRemotePlayerInBoundLevel(const RemotePlayer& player) {
        return player.dimension == ClientLevels::BoundDimension();
    }

#if ENABLE_IMMERSIVE_PORTALS
    ClientImmersivePortals& GetClientImmersivePortals() {
        // The bound level's store. Before a session exists (title screen)
        // there is nothing to hold portals, so hand out an empty one.
        static ClientImmersivePortals s_none;
        ClientLevel* level = ClientLevels::Bound();
        if (!level) level = ClientLevels::HasSession() ? &ClientLevels::Active() : nullptr;
        return level ? level->Portals() : s_none;
    }
#endif

} // namespace Client
