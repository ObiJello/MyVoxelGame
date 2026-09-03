// File: src/server/IntegratedServer.hpp
#pragma once

#include "server/world/storage/anvil/SessionLock.hpp"

#include "common/network/PacketTypes.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/level/World.hpp"
#include "common/network/AsioInclude.hpp"
#include "commands/CommandDispatcher.hpp"
#include "server/world/watch/ChunkLoader.hpp"
#include "ServerTickRateManager.hpp"
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <array>

#include "common/world/level/DimensionId.hpp"

namespace Game {
    class ILevelWrite;
    class ClientPlayer;
    class MyTerrainGenerator;
    class Entity;
}

// SummonMobs takes an EntityTypeId by value, so the enum must be complete
// here rather than forward-declared.
#include "common/entity/EntityType.hpp"

namespace Game::Immersive { struct Portal; }

namespace Server {

    class PlayerEntityView;
#if ENABLE_IMMERSIVE_PORTALS
    class ImmersivePortalRegistry;
    class NetherPortalGeneration;
    class EntityPortalTravel;
#endif

    // MC's summon carries an NBT compound for per-entity overrides
    // (`/summon tnt ~ ~ ~ {Fuse:40}`). There is no NBT parser here, so the
    // handful of overrides worth having are a struct instead — see
    // SummonCommand for the `key=value` surface that fills it.
    //
    // Namespace scope rather than nested in IntegratedServer: a nested type's
    // default member initializers are not usable in a defaulted argument
    // inside the same class definition, which is exactly how SummonMobs takes
    // it.
    // /shape's parsed request — see ShapeCommand. Namespace scope for the
    // same reason as SummonOptions below.
    enum class ShapeForm : uint8_t { Box, Wall, Sphere, Dome, Cylinder, Pyramid };

    struct ShapeJobRequest {
        ShapeForm     form  = ShapeForm::Box;
        Game::BlockID block = Game::BlockID::Air;
        // Form-specific sizes: Box a x b x c; Wall a wide, b high; Sphere/Dome
        // radius a; Cylinder radius a height b; Pyramid base a.
        int a = 1, b = 1, c = 1;
        bool hollow = false, frame = false, checker = false;
        int  spaced = 1;
        glm::ivec3 facing{0, 0, 1};   // cardinal the player faced
        glm::ivec3 base{0};           // base centre (bottom, middle)

        // How far the shape extends along the facing axis (for the default
        // "in front of you" placement).
        int ForwardExtent() const {
            switch (form) {
                case ShapeForm::Box:      return facing.x != 0 ? a : c;
                case ShapeForm::Wall:     return 1;
                case ShapeForm::Sphere:
                case ShapeForm::Dome:     return 2 * a + 1;
                case ShapeForm::Cylinder: return 2 * a + 1;
                case ShapeForm::Pyramid:  return a;
            }
            return a;
        }
        int64_t BoundingVolume() const {
            switch (form) {
                case ShapeForm::Box:      return int64_t(a) * b * c;
                case ShapeForm::Wall:     return int64_t(a) * b;
                case ShapeForm::Sphere:   return int64_t(2 * a + 1) * (2 * a + 1) * (2 * a + 1);
                case ShapeForm::Dome:     return int64_t(2 * a + 1) * (a + 1) * (2 * a + 1);
                case ShapeForm::Cylinder: return int64_t(2 * a + 1) * b * (2 * a + 1);
                case ShapeForm::Pyramid:  return int64_t(a) * ((a + 1) / 2) * a;
            }
            return 0;
        }
    };

    struct SummonOptions {
        // MC PrimedTnt's `fuse` tag. -1 keeps the type's own default (80).
        int tntFuse = -1;
        // Added to the fuse once per successive entity, so a stack of TNT
        // detonates in sequence instead of as one blast. 0 = simultaneous.
        // No vanilla equivalent — MC would need one command per fuse value.
        int tntFuseStep = 0;
    };

    // Forward declarations
    class ChunkTicketManager;
    class ChunkStatusManager;
    class SendScheduler;
    class PlayerSessionManager;
    class ServerPlayer;
    class PlayerSession;
    class SectionChangeAccumulator;
    class ChunkDeltaBroadcaster;
    class ItemEntityManager;
    class ExperienceOrbManager;
    class MobManager;
    class ServerLevelBridge;
    class ServerEntityTracker;
    class ServerLevel;
    struct ItemPickupEvent;
    struct XpOrbPickupEvent;

    // Server thread tracking
    extern std::thread::id g_serverThreadId;

    // Server configuration
    struct IntegratedServerConfig {
        int tickRate = 20;                      // Server ticks per second (20 TPS like Minecraft)
        int maxChunksPerTick = 32;             // Max chunks to process per tick (increased from 5)
        float chunkProcessBudgetMs = 2.0f;     // Time budget for chunk processing per tick
        int defaultViewDistance = 8;           // Default view distance in chunks (Minecraft-like)
        int serverViewDistance = 32;           // Server's max view distance cap (clients clamped to this)
        bool enableAsyncChunkLoading = true;   // Use ServerWorkerPool for chunk loading
        bool enableChunkCaching = true;        // Keep recently used chunks in memory
        std::string minecraftWorldPath;        // Optional Minecraft world to load (empty by default)

        // Root of this world's ObeyCraft save folder. Non-empty means the
        // world PERSISTS: chunks load from here before generating and are
        // written back, and level.dat is created if missing.
        //
        // Deliberately separate from minecraftWorldPath. That one is the
        // player's real Minecraft world and is never written; this one is
        // ours. Conflating the two is how the old writer ended up aimed at
        // somebody's survival world.
        std::string savePath;
        std::string worldDisplayName = "World";   // level.dat LevelName
        // The world's generation seed, for level.dat. PlatformMain pushes the
        // seed onto the World only AFTER this server is constructed, so the
        // config has to carry it or the first level.dat records seed 0 — and
        // Minecraft would then generate everything beyond our saved chunks
        // from the wrong seed.
        int64_t worldSeed = 0;
        bool useLocalSaveDirectory = true;     // Automatically use local save directory if available (temporary feature)
        // True when this world came from an imported Minecraft save rather than
        // being generated from a seed. level.dat isn't parsed, so such worlds
        // never get SetGenerationSeed and GetGenerationSeed() reports the
        // generator default — /seed says so rather than printing a wrong number.
        bool useMinecraftSave = false;
        // Load the Anvil world but never write to it. Set for worlds imported
        // from the player's real Minecraft installation: we do not implement
        // enough of the save format to be trusted with somebody's actual
        // survival world, and a partial rewrite would corrupt it. Blocks can
        // still be broken during the session; nothing is persisted.
        bool readOnlyWorld = false;
        int defaultGameMode = 0;               // World game mode applied to joining players (GameMode raw: 0 survival, 1 creative)
        int64_t initialDayTime = 6000;         // World time restored from world metadata (6000 = noon)
        bool doDaylightCycle = false;          // doDaylightCycle gamerule restored from world metadata
        // Immersive nether portals (see-through, no purple blocks) vs the
        // vanilla block portals. /gamerule immersive_portals, --vanilla-portals.
        bool immersivePortals = true;
        // World options (see Game::Portals::WorldWrapSize / DimensionStack).
        int  worldWrapSize = 0;
        bool dimensionStack = false;
        // 0 peaceful, 1 easy, 2 normal, 3 hard (MC Difficulty ids). Applied
        // to every level's World; /difficulty changes it for the session and
        // the save.
        int  difficulty = 2;

        // MC IntegratedServer.java:69 — `setSingleplayerProfile(minecraft
        // .getGameProfile())`. The name of the player who launched this
        // process. Empty means "this server has no singleplayer owner", which
        // is MC's null getSingleplayerProfile and is what makes
        // DedicatedServer.isSingleplayerOwner return false outright.
        //
        // The owner is exempt from keep-alive and the read timeout, exactly as
        // in vanilla — which is why MC never times a host out of their own
        // world however long a tick runs.
        std::string singleplayerProfileName;
        // Whether this server HAS a singleplayer owner at all. False is MC's
        // DedicatedServer, whose isSingleplayerOwner returns false outright
        // (DedicatedServer.java:722). Kept separate from the name because an
        // empty name is legitimate — it means "the server's default name".
        bool hasSingleplayerOwner = false;
    };


    // Integrated server class (mirrors MinecraftServer + IntegratedServer from Minecraft)
    class IntegratedServer {
    public:
        explicit IntegratedServer(const IntegratedServerConfig& config = IntegratedServerConfig{});
        ~IntegratedServer();

        // Non-copyable, non-movable
        IntegratedServer(const IntegratedServer&) = delete;
        IntegratedServer& operator=(const IntegratedServer&) = delete;

        // ========================================================================
        // SERVER LIFECYCLE
        // ========================================================================

        // Initialize server (creates world internally)
        bool Initialize();

        // Start server thread (20 TPS loop)
        bool Start();

        // Stop server gracefully
        void Stop();

        // Check if server is running
        bool IsRunning() const { return m_running.load(); }

        // Shutdown and cleanup
        void Shutdown();

        // ========================================================================
        // WORLD MANAGEMENT
        // ========================================================================

        // The OVERWORLD's world instance.
        //
        // Kept under its original name and meaning because the overwhelming
        // majority of its callers — commands, the debug panel, the world-time
        // sync, spawn finding — are asking about the overworld specifically or
        // predate dimensions entirely. Anything that must follow a player
        // wants LevelOf() instead; passing this where a dimension was meant is
        // how a Nether edit lands in the Overworld.
        Game::World* GetWorld() const;

        // ========================================================================
        // DIMENSIONS
        // ========================================================================

        // The level for a dimension, creating it if this is the first visit.
        // Null only when creation failed (a generator that could not start).
        //
        // Server thread only: it may build a chunk provider, and
        // ServerChunkCache captures the constructing thread's id.
        ServerLevel* GetOrCreateLevel(Game::DimensionId dimension);

        // The level for a dimension, or null if it has never been visited.
        // Safe from any context that already holds the server thread.
        ServerLevel* GetLevel(Game::DimensionId dimension) const;

        // The overworld, which always exists once Initialize has run.
        ServerLevel& Overworld() const;

#if ENABLE_IMMERSIVE_PORTALS
        // Every immersive portal in every dimension — see
        // server/portal/ImmersivePortalRegistry.hpp. Null until Initialize
        // has built the overworld (the registry loads from its save root).
        ImmersivePortalRegistry* ImmersivePortals() const { return m_immersivePortals.get(); }
        NetherPortalGeneration*  NetherPortals()    const { return m_netherPortalGeneration.get(); }
        EntityPortalTravel*      EntityTravel()     const { return m_entityTravel.get(); }
        // The immersive/vanilla switch (config + the common-side global).
        void SetImmersivePortals(bool on);
        bool ImmersivePortalsEnabled() const { return m_config.immersivePortals; }
#endif
        // A block of obsidian was removed somewhere (World::SetBlock). Breaks
        // any immersive nether portal whose frame it belonged to.
        void OnObsidianRemoved(Game::DimensionId dimension, const glm::ivec3& pos);

        // A chunk's data (full or "unchanged") just went out to a session.
        // Anything anchored in that chunk that the client must hold alongside
        // it — today the immersive portals — is sent here, right behind it.
        // Called by PlayerSession::SendNextChunks; server thread.
        void OnChunkSentToClient(PlayerSession& session, Game::DimensionId dimension,
                                 Game::Math::ChunkPos chunk);

#if ENABLE_IMMERSIVE_PORTALS
        // A client's eye crossed an immersive portal (PortalTeleportC2S).
        // Validate loosely — the reported position must be near where the
        // server has the player, and near the portal — then move the
        // server's player through the portal's transform, into the far
        // level if it leads there, without a respawn. A rejection resyncs
        // the client to the server's position and dimension. Server thread.
        void OnClientPortalTeleport(PlayerSession& session,
                                    const Network::PortalTeleportC2SPacket& packet);
        // Move a player through `portal` from `eyeBefore` (already judged
        // legitimate), into the far level if it leads there, and face
        // `yaw`/`pitch` on arrival. `clientPredicted`: the client is
        // already there and needs no position packet; otherwise (the
        // server saw the crossing itself, EntityPortalTravel::TickPlayers)
        // the client is teleported. Server thread.
        bool TeleportPlayerThroughPortal(PlayerSession& session,
                                         const Game::Immersive::Portal& portal,
                                         const glm::dvec3& eyeBefore,
                                         float yaw, float pitch, bool clientPredicted);
#endif

        // The set of chunk loaders a session wants right now: its own view
        // distance, plus the far side of every immersive portal near it (and,
        // one level deep, portals near those far sides). See ChunkLoader.hpp
        // and the mod's ChunkVisibility. Server thread.
        std::vector<ChunkLoader> ComputeChunkLoaders(const PlayerSession& session) const;

        // The level a session's player is standing in. Falls back to the
        // overworld for a session with no player attached yet.
        ServerLevel& LevelOf(const PlayerSession& session);

        // Every level that currently exists, in creation order. Used by the
        // tick loop and by shutdown; never allocates.
        template <typename Fn>
        void ForEachLevel(Fn&& fn) {
            for (auto& level : m_levels) {
                if (level) fn(*level);
            }
        }

        // MC MinecraftServer.tickRateManager(). Owns the tick budget, the
        // freeze/step state and the sprint machinery that /tick drives.
        ServerTickRateManager& tickRateManager() { return m_tickRateManager; }
        
        // The OVERWORLD's change accumulator.
        //
        // Reached from Game::World::SetBlock, which has no session in scope
        // and so cannot ask "whose dimension is this". It resolves the right
        // accumulator from the World's own DimensionId instead — see
        // GetChangeAccumulatorFor.
        SectionChangeAccumulator* GetChangeAccumulator() const;

        // The accumulator belonging to whichever level owns this World. Null
        // for a World the server does not own (the client's predicted level).
        SectionChangeAccumulator* GetChangeAccumulatorFor(const Game::World& world) const;

        // Broadcast the current world time to every connected player.
        // MC forceTimeSynchronization: called every 20 ticks and immediately
        // after /time or /gamerule doDaylightCycle changes.
        void ForceTimeSync();

        // MC MinecraftServer.autoSave: queue every dirty chunk and rewrite
        // level.dat. Non-blocking — the storage thread drains the queue.
        void AutoSave();
        // level.dat carries values that are only settled after startup (the
        // seed, the searched-for spawn, the current time), so it is rewritten
        // rather than written once.
        void WriteLevelDat();

        // ── Pause ───────────────────────────────────────────────────────────
        //
        // MC IntegratedServer.tickServer:102-133. The world simulation stops;
        // networking does NOT, so the server stays joinable, keeps streaming
        // chunks and still answers commands. That is the same split `/tick
        // freeze` already makes here, which is why SimulationRuns() folds both
        // into one question.
        //
        // Vanilla derives the flag from the single embedded client and gives up
        // entirely once the world is published to LAN
        // (Minecraft.java:1284 `&& !singleplayerServer.isPublished()`), because
        // a remote player has no way to report their screen. Each client here
        // sends PlayerPauseC2SPacket, so the rule generalises properly: freeze
        // only when EVERY connected player is paused.
        bool IsPaused() const { return m_paused; }

        // "Should game elements run this tick?" — the freeze from /tick and the
        // pause from the menu, answered together. Every world-simulation gate
        // asks this rather than the tick manager directly, so a new one cannot
        // accidentally keep running while the game is paused.
        bool SimulationRuns() const {
            return m_tickRateManager.runsNormally() && !m_paused;
        }

        // playerdata/<uuid>.dat. Saved on disconnect and on autosave — NOT in
        // Shutdown(), where Stop() has already destroyed every ServerPlayer.
        void SavePlayerData(const ServerPlayer& player);
        // True when a saved file was actually applied. The caller needs this:
        // a returning player's game mode came from their save, and stamping
        // the world default over it also clears their flight state.
        bool LoadPlayerData(ServerPlayer& player);
        void SaveAllPlayers();

        // ========================================================================
        // PLAYER MANAGEMENT
        // ========================================================================

        // Set player (for integrated server)
        void SetPlayer(Game::ClientPlayer* player);
        
        // Get player position/chunk from active session (single source of truth)
        Game::Math::ChunkPos GetPlayerChunkPosition() const;
        glm::vec3 GetPlayerPosition() const;

        // Get player session for network/view management
        // NOTE: Delegates to SessionManager now instead of using m_playerSession
        std::shared_ptr<PlayerSession> GetPlayerSession() const;

        // Get session manager for accessing player sessions
        PlayerSessionManager* GetSessionManager() const { return m_sessionManager.get(); }

        // Dropped-item entities. Every "this produced an item in the world"
        // path goes through here — block loot, container spill, player throws,
        // and Game::DropItemStackNear. Null before the server has started.
        ItemEntityManager* GetItemEntities() const;

        // Experience orbs. Every "this awarded XP in the world" path goes
        // through here (ServerLevelBridge::AwardExperience). Null before the
        // server has started.
        ExperienceOrbManager* GetXpOrbs() const;

        // Get command dispatcher for server-side command execution
        CommandDispatcher& GetCommandDispatcher() { return m_commandDispatcher; }

        // Get status manager for chunk generation tracking
        ChunkStatusManager* GetStatusManager() const;

        // Process watch set changes: request loading for new chunks, unload for removed
        void ProcessWatchSetChanges();

        // Drains the terrain generator's main-thread queue (MC's
        // runDistanceManagerUpdates). MUST run on the server thread: the tasks
        // touch ChunkMap/DistanceManager, which the terrain library treats as
        // main-thread-only, so a worker cannot pump this itself.
        //
        // Called from the server loop's idle window, not just once per tick.
        // Measured 2026-08: a ServerWorker blocked in ServerChunkCache::getChunk
        // waits on THIS queue, so pumping it only at 20 TPS added ~25 ms of pure
        // latency to every chunk (measured wait 32.79 ms of a 68.68 ms load).
        //
        // `deadline` is mandatory and is the ONLY bound on how long this runs.
        // One unit of pipeline work is bounded; the pipeline is not. Every
        // caller must pass a deadline it can afford to reach — see
        // MyTerrainGenerator::PumpOneTask for what happened when nothing did.
        void PumpChunkPipeline(std::chrono::steady_clock::time_point deadline);
        void ServiceGenerationQueues(ServerLevel& level, Game::MyTerrainGenerator& gen);

        // The OVERWORLD's terrain generator, or null before its provider is
        // initialised. Server thread only.
        //
        // Overworld-specifically: every caller of this wants "the generator"
        // in the singular sense that predates dimensions. Anything that must
        // pump or query a particular level's pipeline goes through
        // ServerLevel::TerrainGenerator instead — see PumpChunkPipeline, which
        // has to reach all three.
        Game::MyTerrainGenerator* GetTerrainGenerator() const;

        // Unload chunks that no player is watching (periodic cleanup)
        void UnloadUnwatchedChunks();
        
        // Send block change packets to the players watching the affected chunk
        // IN `dimension`.
        //
        // The dimension is mandatory because neither packet carries one: an
        // unscoped broadcast makes every client apply the edit to its own copy
        // of that x/z chunk, whichever world it is standing in.
        void SendBlockChangeS2CPacket(Game::DimensionId dimension,
                                      const Network::BlockChangeS2CPacket& packet);
        void SendSectionBlocksUpdateS2CPacket(Game::DimensionId dimension,
                                              const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);

        // ========================================================================
        // PACKET PROCESSING (Called by NetworkServer)
        // ========================================================================
        
        // Process incoming block action packet
        void ProcessBlockAction(const Network::BlockActionC2SPacket& packet);
        
        // Process incoming chat message
        void ProcessChatMessage(const Network::ChatMessageC2SPacket& packet);
        
        // Called when a player successfully logs in and needs initial chunks
        void OnPlayerJoined(std::shared_ptr<class ServerConnection> connection);

        // Called when a player disconnects (TCP close)
        void OnPlayerDisconnected(std::shared_ptr<class ServerConnection> connection);

        // A client's requested distances, as they travel from the I/O thread
        // to the server thread. View = chunks streamed to it; simulation =
        // chunks the server ticks around it. Independent, as in MC's
        // DistanceManager (PlayerTicketTracker vs SimulationChunkTracker).
        struct ClientDistances {
            int viewDistance;
            int simulationDistance;
        };

        // Called when server receives client settings (render distance, etc.)
        void OnClientSettingsReceived(uint32_t connectionId, int requestedViewDistance,
                                      int requestedSimulationDistance);

        // Clamp + apply a client's requested distances to its session and
        // echo the effective view distance back. Shared by the normal path
        // and the deferred one below.
        void ApplyClientViewDistance(PlayerSession& session, uint32_t connectionId,
                                     ClientDistances requested);

        // Apply every stashed client view distance whose session is now ready.
        // Server thread only. Called from the join path (so the first chunk
        // batch already goes out at the right radius) and once per tick (so a
        // stash that lost the race with the join is not lost forever).
        void ApplyPendingClientViewDistances();

        // Client settings waiting for a session that is ready to keep them,
        // keyed by connection id.
        //
        // ClientConfigC2S is sent once, the instant the client sees
        // LoginSuccess, and it arrives on the NETWORK I/O thread while the
        // SERVER thread is still building the session. Two things go wrong if
        // it is applied where it lands: the session may not exist yet, and —
        // the case that actually bit — it may exist but not be Initialize()d,
        // in which case Initialize resets the view distance to 2 right after
        // and the player is stuck with a 5x5 square of chunks for the session.
        //
        // So the I/O thread only ever STASHES, under m_pendingViewDistanceMutex,
        // and the server thread is the only one that applies. That is also
        // what MC does: handleClientInformation defers to the main thread.
        // Entries are erased on disconnect so a reused connection id cannot
        // inherit a stale one.
        std::unordered_map<uint32_t, ClientDistances> m_pendingClientViewDistance;
        mutable std::mutex                m_pendingViewDistanceMutex;

        // Send the effective view distance to the client
        void SendSetChunkCacheRadius(uint32_t connectionId, int viewDistance);

        // ========================================================================
        // CONFIGURATION
        // ========================================================================

        void SetConfig(const IntegratedServerConfig& config) { m_config = config; }
        const IntegratedServerConfig& GetConfig() const { return m_config; }

        // The world's difficulty (MC Difficulty id, 0 peaceful .. 3 hard),
        // pushed to every level's World and written to the save.
        int  GetDifficulty() const { return m_config.difficulty; }
        void SetDifficulty(int difficulty);

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ServerStats {
            std::atomic<uint64_t> ticksProcessed{0};
            std::atomic<uint64_t> chunksLoaded{0};
            std::atomic<uint64_t> chunksSent{0};
            std::atomic<uint64_t> blockChangesProcessed{0};
            std::atomic<uint64_t> packetsReceived{0};
            std::atomic<uint64_t> packetsSent{0};
            std::atomic<float> averageTickTime{0.0f};
            std::atomic<float> averageTPS{20.0f};
            std::atomic<size_t> noiseChunksReleased{0};
            std::atomic<size_t> libraryHoldersUnloaded{0};

            void Reset() {
                ticksProcessed = chunksLoaded = chunksSent = blockChangesProcessed = 0;
                packetsReceived = packetsSent = 0;
                averageTickTime = 0.0f;
                averageTPS = 20.0f;
            }
        };

        const ServerStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }
        void LogStats() const;

        // Chunk streaming metrics
        size_t GetPendingChunkLoadCount() const;

    private:
        // Configuration
        IntegratedServerConfig m_config;

        // ── Dimensions ──────────────────────────────────────────────────────
        //
        // One ServerLevel per dimension, indexed by Game::DimensionSlot. The
        // overworld is built during Initialize; the Nether and the End are
        // built on first visit, because each costs a terrain generator and
        // most sessions never see either.
        //
        // Everything that used to be a world-scoped member of this class —
        // the world itself, the ticket and status managers, the change
        // accumulator and broadcaster, item entities, XP orbs, the mob level,
        // manager and tracker, and the pending/failed chunk-load sets — now
        // lives inside a level. They ALL had to move together: every one of
        // them is keyed by ChunkPos or by entity id, and neither carries a
        // dimension, so leaving any single one shared would apply one
        // dimension's edits to another's identically-numbered chunk.
        std::array<std::unique_ptr<ServerLevel>, Game::kDimensionCount> m_levels;

#if ENABLE_IMMERSIVE_PORTALS
        // One for the whole server, not per level: ids are global because a
        // client holds portals from several dimensions at once, and a
        // portal's partners live in the other dimension.
        std::unique_ptr<ImmersivePortalRegistry> m_immersivePortals;
        std::unique_ptr<NetherPortalGeneration>  m_netherPortalGeneration;
        // Mobs, items and orbs crossing surfaces; mobs chasing players
        // through them. Reaches the private entity broadcasts.
        std::unique_ptr<EntityPortalTravel>      m_entityTravel;
        friend class EntityPortalTravel;
        // Game::Portals' frame-lit handler — the fire block's route into
        // NetherPortalGeneration.
        static bool OnImmersiveFrameLit(Game::ILevelWrite& level, const glm::ivec3& firePos);
#endif
        // No-op when the feature is off or nothing changed.
        void SaveImmersivePortals();

        // ── "No ambient occlusion" boxes (the occlusion wand) ────────────
        // Inclusive block boxes per dimension. Kept whole on the server,
        // saved to <save>/data/ao_regions.json, sent whole to every client
        // holding the dimension on change and on first hold.
        struct AoRegion {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            glm::ivec3 min{0};
            glm::ivec3 max{0};
        };
        std::vector<AoRegion> m_aoRegions;
    public:
        // The wand's two entry points (AoWandBehavior.cpp).
        void AddAoRegion(Game::DimensionId dimension, const glm::ivec3& a, const glm::ivec3& b);
        // Remove every box containing `pos`; returns how many went.
        size_t RemoveAoRegionsAt(Game::DimensionId dimension, const glm::ivec3& pos);
    private:
        void SendAoRegions(ServerConnection& connection, Game::DimensionId dimension) const;
        void BroadcastAoRegions(Game::DimensionId dimension) const;
        void LoadAoRegions();
        void SaveAoRegions() const;
        // The world-sized portals a dimension's world options ask for (wrap
        // borders, stack seams), created once per dimension when its level
        // comes up. Idempotent per tag.
        void EnsureGlobalPortals(Game::DimensionId dimension);

        // By value, not a unique_ptr: it has no dependencies to construct and
        // the server loop reads it every iteration, so an indirection would be
        // on the hot path for nothing.
        ServerTickRateManager m_tickRateManager{*this};

        // ── Global, deliberately not per-dimension ──────────────────────────
        // A player exists once wherever they are standing, a connection is not
        // a property of a world, and the send budget belongs to the socket.
        std::unique_ptr<SendScheduler> m_sendScheduler;
        std::unique_ptr<PlayerSessionManager> m_sessionManager;
        CommandDispatcher m_commandDispatcher;

        // Player reference (for integrated server)
        Game::ClientPlayer* m_player = nullptr;

        // Thread management
        std::unique_ptr<std::thread> m_serverThread;
        // Connection id currently holding the singleplayer-owner exemption,
        // 0 = free. A one-shot latch rather than a bare name test: the name
        // arrives over the wire in LoginStart and --name is a free-form flag,
        // so first-claimant-wins stands in for MC's name_taken rejection and
        // bounds any spoof to a single connection.
        // MC MinecraftServer.nextTickTimeNanos — the absolute end of the
        // current tick. Phases that can defer work bound themselves against it
        // instead of taking a fixed slice, so a tick already over budget stops
        // adding to the overrun. Zero until the first tick.
        std::chrono::steady_clock::time_point m_tickDeadline{};

        std::atomic<uint32_t> m_singleplayerOwnerConnId{0};

        std::atomic<bool> m_running{false};
        std::atomic<bool> m_shouldStop{false};
        // Server-thread only, but atomic because the debug overlay and the
        // client's own "is the world frozen" checks read it from elsewhere.
        // TRUE at construction, exactly like MC's `private boolean paused =
        // true` (IntegratedServer.java:58). An empty server is a paused one, so
        // starting false would make the very first tick look like a pause
        // TRANSITION and fire a full world save before the world exists.
        int64_t m_currentServerTick = 0;   // set each ServerTick, read by the unload sweep
        std::atomic<bool> m_paused{true};

        // New player architecture
        std::unique_ptr<ServerPlayer> m_serverPlayer;     // Host player (ID 1)

        // World spawn (block-centered feet position) — the OVERWORLD's, which
        // is the only one MC computes from the generator. Legacy default until
        // the server thread computes the real spawn at startup via
        // IChunkGenerator::FindSpawnPosition. Written once on the server thread
        // before any player joins; joins run on the same thread, so no further
        // synchronization is needed.
        glm::vec3 m_worldSpawn{0.5f, 67.0f, 0.5f};

        // Held for the whole session on a world we own, released only after
        // the final flush. Null for an imported read-only world, which we
        // never write and therefore need not lock.
        std::unique_ptr<Game::Anvil::SessionLock> m_sessionLock;
        std::unordered_map<uint32_t, std::unique_ptr<ServerPlayer>> m_remotePlayers; // Remote players by ID
        // NOTE: PlayerSession is now managed by PlayerSessionManager, not stored here

        // Chunk management. The in-flight and failed sets are PER LEVEL
        // (ServerLevel::pendingChunkLoads / failedChunkLoads) — Overworld
        // (0,0) and Nether (0,0) are the same ChunkPos, and a shared set would
        // make the second request look like a duplicate of the first, so the
        // chunk would never load and nothing would ever ask again.

        // Statistics
        ServerStats m_stats;

        // Timing
        std::chrono::steady_clock::time_point m_lastTickTime;
        std::chrono::steady_clock::time_point m_lastTickStartTime;  // For accurate TPS calculation
        std::chrono::duration<float> m_tickDuration{1.0f / 20.0f}; // 50ms per tick

        // ========================================================================
        // SERVER THREAD MAIN LOOP
        // ========================================================================

        // Main server loop (runs at 20 TPS)
        void ServerLoop();

        // Single server tick
        void ServerTick();
        
        // Initialize the new session management system
        void InitializeSessionSystem();
        
        // Cleanup the session management system
        void CleanupSessionSystem();

        // ========================================================================
        // DISCIPLINED QUEUE DRAINING (ALWAYS FIRST IN TICK)
        // ========================================================================

        // Process all Client→Server packets
        void ProcessClientToServerPackets();


        // ========================================================================
        // CHUNK MANAGEMENT
        // ========================================================================

        // Process async chunk load results from ServerWorkerPool
        void ProcessAsyncChunkResults();

        // ========================================================================
        // ITEM ENTITY BROADCAST
        // ========================================================================

        // Send this tick's item-entity position refreshes for one level. Scoped
        // per chunk — a client is only told about items in chunks it is
        // actually watching, which matters because items outnumber players by
        // orders of magnitude.
        void BroadcastItemEntityUpdates(ServerLevel& level, int64_t serverTick);

        // ========================================================================
        // MOB ENTITIES
        // ========================================================================

        // One tick of one level's mob system: sync the player views, tick the
        // mobs, run the natural spawner, then drain the tracker's packets.
        void TickMobs(ServerLevel& level, int64_t serverTick);

        // MC NaturalSpawner's per-tick pass over one level's spawnable chunks.
        void RunNaturalSpawner(ServerLevel& level, int64_t serverTick);

        // ========================================================================
        // PORTALS (nether / end)
        // ========================================================================

        // One tick of portal contact and timing for every entity that can
        // travel: mobs, projectiles, and the PlayerEntityView backing each
        // connected player.
        //
        // MC drives this from Entity.baseTick, which every entity runs. Here it
        // is one server-side pass because a PlayerEntityView is deliberately
        // never Tick()ed — the client owns player movement — so the player,
        // the one thing that MUST be able to use a portal, would otherwise be
        // the only entity the hook never reached.
        //
        // Runs inside the `/tick freeze` gate: standing in a portal on a frozen
        // server should not carry you to the Nether.
        //
        // Per level: an entity's portal contact is tested against the blocks of
        // the world it is standing in, and both the player views and the mob
        // list are per level too.
        void TickPortals(ServerLevel& level);

        // A portal fired for `entity`. Resolves the destination and performs
        // the travel. `portal` is the block kind and `entryPos` the cell the
        // entity was standing in when the timer completed — MC's
        // PortalProcessor.entryPosition, which is what the exit alignment is
        // measured against.
        void HandlePortalTraversal(ServerLevel& from, Game::Entity& entity,
                                   Game::BlockID portal, const glm::ivec3& entryPos);

        // Handle a client's attack on a mob (MC ServerboundInteractPacket).
        // Public entry point for the play packet listener.
    public:
        // `sprinting` comes from the client because movement is
        // client-authoritative here; MC reads it from its own copy of the
        // player. It only ever REMOVES a crit and adds knockback, so a client
        // lying about it can make its own hits weaker, not stronger.
        // `dragonPart` is the client-picked EnderDragon part index (-1 for
        // everything else); re-validated against the server's own part
        // layout before it can route head damage.
        void HandleInteract(uint32_t connectionId, int32_t entityId, bool attack,
                            bool sprinting, int dragonPart = -1);

        // MC PlayerList.broadcastSystemMessage(component, false): a server
        // message with no sender, delivered to every connected client. Used for
        // the join/leave notices, which vanilla renders in yellow.
        void BroadcastSystemMessage(const std::string& text, uint32_t color);

        // MC's post-hit visuals: entity event 4 for the swing, plus the crit
        // particle burst. Sent to every watcher of the target's chunk IN
        // `dimension`, not just the attacker.
        void BroadcastAttackEffects(Game::DimensionId dimension,
                                    const Game::LivingEntity& target, bool crit);

        // MC Player.doSweepAttack — the sword arc that clips everything living
        // standing next to what was hit. Split out of HandleInteract for the
        // same reason MC splits it: the conditions that decide whether it
        // happens are already a paragraph on their own.
        void DoSweepAttack(Server::PlayerEntityView& attacker,
                           Game::LivingEntity& target, float strengthScale);

        // MC Entity.onClimbable, reduced to the block the feet are in — this
        // port has no block tags at runtime, so BlockTags.CLIMBABLE is a switch.
        bool IsOnClimbable(const ServerPlayer& player) const;

        // The entity view backing a connected player. Exposed because the
        // player-position broadcast reads its hurtTime for the flash, and the
        // view is where LivingEntity::Hurt actually set it.
        Server::PlayerEntityView* GetPlayerEntityView(uint32_t connectionId);

        // MC's crit is a particle burst plus a sound; this port has neither
        // yet, so the event exists to carry the signal and the client draws
        // what it can. Numbered clear of MC's own 2/3/10/18/60.
        static constexpr uint8_t kEntityEventCrit = 200;

        // Spawn `count` mobs of `type` at `pos`, for /summon. Returns how many
        // were actually created. Scattered slightly so a stack of them does not
        // spawn inside one another and immediately push apart.
        int SummonMobs(Game::EntityTypeId type, const glm::dvec3& pos, int count,
                       const SummonOptions& options = {});

        // /shape: accept a build job (false while one is still streaming) and
        // the per-tick pump that places its blocks. See ShapeCommand.
        bool SubmitShapeJob(const ShapeJobRequest& job);
        void TickShapeJob();

        // MC EntityType.spawn(level, stack, user, pos, SPAWN_ITEM_USE,
        // tryMoveDown, movedUp) — the spawn-egg path. Reached from common code
        // through Game::SpawnMobFromItem; see WorldMobSpawn.hpp for why the
        // indirection exists and what the two flags mean.
        //
        // Distinct from SummonMobs because the placement rules differ: /summon
        // puts a mob exactly where it is told, an egg drops it onto the surface
        // it was clicked against.
        bool SpawnMobFromItemUse(Game::EntityTypeId type, const glm::ivec3& spawnPos,
                                 bool tryMoveDown, bool movedUp, Game::DimensionId dimension,
                                 int portalCooldownTicks = 0);

        // MC EndCrystalItem.useOn — place a crystal entity on obsidian or
        // bedrock, and let the End's dragon fight test for the respawn
        // ritual. Returns whether one was placed (the caller consumes the
        // item). Server-side for the same reason as the spawn eggs: item
        // code in `common` cannot spawn entities or reach the fight.
        bool PlaceEndCrystalFromUse(PlayerSession& session, const glm::ivec3& clicked);

        // Request chunk loading (either sync or async via ServerWorkerPool).
        //
        // `dimension` names the level to load into. It travels all the way to
        // the worker and back on the result, because a ChunkPos alone cannot
        // say which world it belongs to — and the in-flight guard is per level
        // for exactly that reason (see ServerLevel::pendingChunkLoads).
        //
        // PUBLIC because EndDragonFight streams its own arena: chunk loading
        // is watch-set driven here, and MC's DRAGON ticket has no other
        // equivalent (forced tickets only keep loaded chunks ticking).
        // Duplicate requests are absorbed by pendingChunkLoads, so external
        // callers cannot flood the pipeline.
        void RequestChunkLoad(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                              int priority = 0);

        // MC WitherSkullBlock.checkSpawn — the soul-sand ritual. Called by
        // PlayerSession right after a wither skeleton skull block (floor or
        // wall) is placed at `pos`; scans for the completed "^^^ / ### / ~#~"
        // pattern in any orientation and, on a match, clears it and spawns the
        // charging Wither. No-op in peaceful or when no pattern is complete.
        void CheckWitherSpawn(const glm::ivec3& pos);

        // Read-only, for the debug panel. Null before the session system is
        // initialised. Callers must treat this as a same-thread snapshot
        // source only — the mobs themselves tick on the server thread.
        MobManager* GetMobs() const;
    private:

        // Tell everyone an item is gone (picked up, merged away, despawned, or
        // its chunk unloaded). Scoped to the DIMENSION but unscoped within it:
        // a client in the right world that has already dropped the chunk simply
        // has no such entity and ignores the id, which is much cheaper than
        // tracking who was told about what — but item, orb and mob ids are
        // allocated per level from the same base, so a flat broadcast would
        // retire the wrong entity for a player standing somewhere else.
        void BroadcastItemEntityRemovals(Game::DimensionId dimension,
                                         const std::vector<int32_t>& ids);

        // Tell clients a player collected items, so they can play the
        // fly-into-the-player animation. This ALSO retires the entity
        // client-side for a full pickup — see BroadcastItemEntityRemovals.
        void BroadcastItemEntityPickups(Game::DimensionId dimension,
            const std::vector<ItemPickupEvent>& pickups);

        // Full spawn packet for one entity, to the watchers of its chunk in
        // `level` — which is also the level whose ItemEntityManager holds it.
        void BroadcastItemEntitySpawn(ServerLevel& level, int32_t id);

        // The experience-orb mirrors of the item broadcast trio. Pickups ride
        // TakeItemEntityS2C (MC uses the same packet for both entity kinds)
        // and removals reuse BroadcastItemEntityRemovals — RemoveEntities is
        // dispatched by id range client-side.
        void BroadcastXpOrbUpdates(ServerLevel& level, int64_t serverTick);
        void BroadcastXpOrbSpawn(ServerLevel& level, int32_t id);
        void BroadcastXpOrbPickups(Game::DimensionId dimension,
                                   const std::vector<XpOrbPickupEvent>& pickups);

    public:
        // Send one already-serialized packet to every player IN `dimension`
        // watching the chunk a position falls in. The scoping is the point:
        // item entities are far more numerous than players, so the naive
        // broadcast-to-everyone used for player positions would not scale here
        // — and the dimension half of it stops a Nether item's move packet
        // reaching someone standing at the same x/z in the Overworld.
        //
        // Public because Game::World::SetBlock uses it for the block-entity
        // create/remove packets: it has a dimension (its own) but no session,
        // and those packets are positional, so they need exactly this scoping.
        void SendToChunkWatchers(Game::DimensionId dimension, const glm::dvec3& pos,
                                 Network::PacketId packetId,
                                 const std::vector<uint8_t>& data);
        void SendToChunkWatchersAt(Game::DimensionId dimension, Game::Math::ChunkPos chunk,
                                   Network::PacketId packetId,
                                   const std::vector<uint8_t>& data);

    private:

        // ========================================================================
        // BLOCK CHANGE PROCESSING (Private implementation details)
        // ========================================================================

        // Validate block action
        bool ValidateBlockAction(const Network::BlockActionC2SPacket& packet) const;

        // Apply block change and notify client
        void ApplyBlockChange(int worldX, int worldY, int worldZ, Game::BlockID blockId);

        // ========================================================================
        // PLAYER UPDATE PROCESSING (Private implementation details)
        // ========================================================================

        // Update view distance watchers
        void UpdateViewDistanceWatchers();

        // ========================================================================
        // PACKET SENDING
        // ========================================================================

        // Send BlockChangeS2CPacket to client
        void SendPacketToClient(Network::BlockChangeS2CPacket&& packet);

        // Send MultiBlockChangeS2CPacket to client
        void SendPacketToClient(Network::MultiBlockChangeS2CPacket&& packet);

        // ========================================================================
        // UTILITY METHODS
        // ========================================================================

        // Calculate distance from player to chunk
        float CalculateChunkDistance(Game::Math::ChunkPos chunkPos) const;

        // Check if chunk is within send radius
        bool IsChunkInSendRadius(Game::Math::ChunkPos chunkPos) const;

        // Get chunks that should be loaded around player
        std::vector<Game::Math::ChunkPos> GetRequiredChunks() const;

        // Update statistics
        void UpdateStatistics(float tickExecutionTime, float timeBetweenTicks);

        // Log server state for debugging
        void LogServerState() const;
        
        // Network server
        std::unique_ptr<net::io_context> m_ioContext;
        std::unique_ptr<class NetworkServer> m_networkServer;
        
        // Dedicated I/O thread and work guard (Minecraft-style Netty pattern)
        using WorkGuard = net::executor_work_guard<net::io_context::executor_type>;
        std::unique_ptr<WorkGuard> m_ioWorkGuard;
        std::unique_ptr<std::thread> m_networkThread;
        
    public:
        // Get network server for direct access
        NetworkServer* GetNetworkServer() const { return m_networkServer.get(); }
    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // Global integrated server instance
    extern std::unique_ptr<IntegratedServer> g_integratedServer;

    // Convenience functions
    void InitializeIntegratedServer(const IntegratedServerConfig& config = IntegratedServerConfig{});
    // Returns false if the server could not start — most commonly because
    // port 25565 is already bound by another instance of the game. This is NOT
    // a soft failure: even in singleplayer the local client connects to the
    // integrated server over TCP (PlatformMain uses 127.0.0.1 + GetPort()), so
    // a failed start means no ticks, no chunk generation, and a client left
    // floating in an empty world. Callers must check it.
    [[nodiscard]] bool StartIntegratedServer();
    void StopIntegratedServer();
    void ShutdownIntegratedServer();

    // Server state queries
    bool IsIntegratedServerRunning();
    const IntegratedServer::ServerStats& GetIntegratedServerStats();

} // namespace Server