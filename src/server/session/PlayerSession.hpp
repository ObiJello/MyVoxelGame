// File: src/server/session/PlayerSession.hpp
#pragma once

#include <set>
#include "common/core/JavaRandom.hpp"               // Game::JavaRandom (loot rolls)
#include "common/core/Log.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/block/BlockInteraction.hpp"  // Game::UseResult
#include "common/network/PacketTypes.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "../world/watch/ChunkTrackingView.hpp"
#include "../world/watch/ChunkLoader.hpp"
#include <glm/glm.hpp>
#include <functional>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <queue>
#include <memory>
#include <chrono>
#include <mutex>
#include <atomic>

namespace Game {
    class World;
}

namespace Server {

    // Forward declarations
    class ServerPlayer;
    class ServerConnection;
    class ChunkTicketManager;
    class ChunkStatusManager;
    class SendScheduler;
    class ItemEntityManager;

    // Coalesced block changes for a chunk section
    struct SectionDiffs {
        Game::Math::ChunkPos chunkPos;
        int sectionIndex;
        // localIndex -> block + state index (they must not get separated:
        // re-orienting a block is a real change watchers need to receive).
        std::unordered_map<uint32_t, Game::BlockState> changes;
        std::chrono::steady_clock::time_point lastUpdate;
        
        uint32_t MakeIndex(uint8_t x, uint8_t y, uint8_t z) const {
            return (y << 8) | (z << 4) | x;
        }
        
        void AddChange(uint8_t x, uint8_t y, uint8_t z, Game::BlockID blockId,
                       Game::BlockStateIndex stateIndex = 0) {
            changes[MakeIndex(x, y, z)] = Game::BlockStates::FromIndex(blockId, stateIndex);
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

    // Player session managing watch sets and streaming
    class PlayerSession {
    public:
        // Configuration
        struct Config {
            // The two distances are INDEPENDENT, as in MC's DistanceManager:
            // viewDistance drives the PlayerTicketTracker (what is loaded
            // and sent) and simulationDistance the SimulationChunkTracker
            // (what ticks). A player can see 32 chunks and simulate 6 — that
            // is the whole point of the Fast preset on a weak machine.
            int simulationDistance = 10;   // Chunks ticked around the player (MC server default)
            int viewDistance = 8;          // Chunks sent to the client
            int maxChunksPerTick = 12;     // Max chunks to send per tick
            int maxBytesPerTick = 1048576; // 1MB per tick max
            int maxDiffBytesPerTick = 524288; // 512KB for block changes per tick
            bool compressPackets = true;   // Enable compression
            int compressionLevel = 3;      // zlib compression level (1-9)
        };

        // Session state
        enum class State {
            CONNECTING,      // Initial handshake
            AUTHENTICATING,  // Login process
            JOINING,         // Sending join game packet
            PLAYING,         // Normal gameplay
            RESPAWNING,      // Respawn in progress
            DISCONNECTING    // Cleanup in progress
        };

        PlayerSession(uint32_t playerId, uint32_t connectionId);
        ~PlayerSession();

        // === LIFECYCLE ===

        // Initialize session after successful login
        void Initialize(const Config& config, int dimensionId, const glm::vec3& spawnPos);
        
        // Attach player entity to this session
        void AttachPlayer(ServerPlayer* player);
        
        // Update connection ID (for integrated server late binding)
        void SetConnectionId(uint32_t connectionId) { m_connectionId = connectionId; }

        // Set send scheduler for packet delivery
        void SetSendScheduler(SendScheduler* scheduler) { m_sendScheduler = scheduler; }
        
        // Detach player entity (on disconnect)
        void DetachPlayer();
        
        // Set connection reference
        void SetConnection(ServerConnection* connection) { m_connection = connection; }
        
        // Process one server tick
        void Tick(int64_t serverTick);
        
        // Cleanup on disconnect
        void Cleanup();

        // === PLAYER STATE (delegated to ServerPlayer) ===

        // Update player position (from client packets) - delegates to ServerPlayer
        void UpdatePosition(const glm::vec3& position, const glm::vec2& rotation);
        
        // Update player's chunk position for view management
        void UpdateChunkPosition(Game::Math::ChunkPos newChunk);
        
        // Change dimension
#if ENABLE_IMMERSIVE_PORTALS
        // The client reports its eye crossed an immersive portal. Validated
        // and applied by IntegratedServer::OnClientPortalTeleport.
        void HandlePortalTeleport(const Network::PortalTeleportC2SPacket& packet);
        // The fill tool: the held block into every open cell of a box.
        void HandleFillBlocks(const Network::FillBlocksC2SPacket& packet);
#endif
        // After the player was moved directly (teleport()), bring the
        // session's chunk anchor in line with the player's new chunk.
        void ResyncChunkPosition();
        // Tell the client which dimension the server has it in, keeping its
        // levels (a rejected portal crossing put it in the wrong one).
        void SendDimensionResync();

        // `keepPrevious`: the level being left stays resident on the client
        // (a seamless portal crossing — it is still visible through the portal
        // behind the player) and its chunks stay tracked by whatever loaders
        // still cover them. Otherwise the client frees that level and the
        // server forgets what it had sent there, without per-chunk unloads.
        void ChangeDimension(int newDimensionId, const glm::vec3& targetPos,
                             bool keepPrevious = false);
        
        // Respawn player
        void Respawn(const glm::vec3& spawnPos);

        // === CLIENT LOAD GATE (MC ServerGamePacketListenerImpl) ===
        //
        // Port of MC's clientLoadedTimeoutTimer / waitingForRespawn pair —
        // the ONLY thing that gates a player's interactions server-side.
        // Deliberately NOT derived from any server-side queue: MC asks the
        // client whether IT is ready (ServerboundPlayerLoadedPacket) and
        // otherwise fails OPEN after 60 ticks, so a lost or unsupported
        // signal costs 3 seconds rather than wedging the player forever.
        //
        // MC ServerGamePacketListenerImpl.java:2069.
        bool HasClientLoaded() const {
            return !m_waitingForRespawn && m_clientLoadedTimeoutTimer <= 0;
        }

        // ── Pause ───────────────────────────────────────────────────────────
        //
        // This player has a pause screen up (PlayerPauseC2SPacket). MC has no
        // per-player equivalent — its integrated server reads the one embedded
        // client — so this is the piece that lets the world freeze correctly
        // with more than one person in it: IntegratedServer stops simulating
        // only when EVERY session reports paused.
        //
        // Starts FALSE, which is the safe default: a client that never sends
        // the packet is treated as playing, so it can never freeze the world
        // for everyone else.
        bool IsPaused() const { return m_paused; }
        void SetPaused(bool paused) { m_paused = paused; }
        // :2073 — called once per tick from ServerPlayer.tick's first line.
        void TickClientLoadTimeout() {
            if (m_clientLoadedTimeoutTimer > 0) --m_clientLoadedTimeoutTimer;
        }
        // :2080 — the client told us its level is ready; skip the rest of the wait.
        void MarkClientLoaded() { m_clientLoadedTimeoutTimer = 0; }
        // :2084 — ServerPlayer.die(). Blocks interaction until PERFORM_RESPAWN.
        void MarkClientUnloadedAfterDeath() { m_waitingForRespawn = true; }
        // :2088 — construction of the play listener AND every respawn.
        void RestartClientLoadTimerAfterRespawn() {
            m_waitingForRespawn = false;
            m_clientLoadedTimeoutTimer = 60;
        }

        // === VIEW CONFIGURATION ===

        // Update view distance (client request or server override)
        void SetViewDistance(int distance);
        
        // Update simulation distance (server config)
        void SetSimulationDistance(int distance);


        // === CHUNK TRACKING (MC ChunkMap.updateChunkTracking) ===

        // Recompute this player's tracking view and emit the difference against
        // the previous one. `onEnter` is called for every chunk newly in view
        // and `onLeave` for every chunk that left; the caller supplies them
        // because entering means "send it if it is loaded, otherwise ask for
        // it", which needs the world and the load queue.
        //
        // No-ops when neither the centre chunk nor the view distance changed —
        // MC's identical early-out, and the reason moving WITHIN a chunk costs
        // nothing at all.
        //
        // MULTI-DIMENSION: the tracked set is the union of the session's
        // chunk LOADERS (ChunkLoader.hpp) — its own view distance plus one
        // circle per portal far side it may look into. `loaders` is the new
        // set, computed by IntegratedServer::ComputeChunkLoaders; the diff
        // against the previous union fires onEnter/onLeave with the chunk's
        // dimension. Nothing happens when the loader set is unchanged.
        void UpdateChunkTracking(
            std::vector<ChunkLoader> loaders,
            const std::function<void(Game::DimensionId, Game::Math::ChunkPos)>& onEnter,
            const std::function<void(Game::DimensionId, Game::Math::ChunkPos)>& onLeave);

        // Is (dimension, chunk) inside any of this session's loaders? This is
        // the authority for "does this player care about chunk X" — there is
        // no reverse index.
        bool IsWatching(Game::DimensionId dimension, Game::Math::ChunkPos chunk) const;
        // The player's OWN dimension. Kept for the callers that only ever
        // meant "the world the player stands in".
        bool IsWatching(Game::Math::ChunkPos chunk) const {
            return IsWatching(Game::DimensionFromRaw(GetDimensionId()), chunk);
        }

        // Has this chunk's data gone out to the client?
        bool HasSentChunk(Game::DimensionId dimension, Game::Math::ChunkPos chunk) const;
        bool HasSentChunk(Game::Math::ChunkPos chunk) const {
            return HasSentChunk(Game::DimensionFromRaw(GetDimensionId()), chunk);
        }

        // The whole sent set for one dimension, for callers that probe it
        // many times per tick and cannot afford a call per probe — the entity
        // tracker asks "has this player got this chunk yet" once per
        // (entity x player). MC's equivalent gate is ChunkMap.isChunkTracked.
        const std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>&
        GetSentChunks(Game::DimensionId dimension) const {
            return m_dimState[Game::DimensionSlot(dimension)].sent;
        }

        // Count of sent chunks across every dimension (F3 readouts).
        size_t GetSentChunkCount() const;

        // Does any loader of this session sit in `dimension`? The player's own
        // dimension always counts. This is what keeps a dimension ticking
        // when it is only being looked into through a portal.
        bool LoadsDimension(Game::DimensionId dimension) const;
        // True the first time it is asked for `dimension` since the client
        // last held that level: the global portals go out then. Also true
        // whenever the chunk just sent is the ONLY one the client holds of
        // the dimension: its level had emptied (every chunk unloaded — the
        // client frees an empty level after a while, and the server is not
        // told), so the surfaces go again. A client that still has them
        // ignores the repeat.
        bool MarkGlobalPortalsSynced(Game::DimensionId dimension) {
            const bool first = m_globalPortalsSynced.insert(dimension).second;
            return first || Dim(dimension).sent.size() <= 1;
        }
        const std::vector<ChunkLoader>& Loaders() const { return m_loaders; }

        // === CHUNK SENDER (Minecraft's PlayerChunkSender) ===

        // MC PlayerChunkSender.markChunkPendingToSend. Queue a LOADED chunk for
        // delivery. Callers must have established the chunk is loaded — an
        // unloaded chunk is simply not queued, and arrives later when
        // generation completes and pushes it (IntegratedServer's
        // onChunkReadyToSend path). There is deliberately no "waiting for this
        // chunk" list on the session.
        void MarkChunkPendingToSend(Game::DimensionId dimension, Game::Math::ChunkPos pos);

        // Drop a chunk: remove from pending, or send unload if already sent
        void DropChunk(Game::DimensionId dimension, Game::Math::ChunkPos pos);

        // A chunk queued for sending, with the world it belongs to.
        struct PendingChunkRef {
            Game::DimensionId    dimension;
            Game::Math::ChunkPos pos;
        };

        // Send next batch of chunks (called once per tick from IntegratedServer).
        // `worldFor` resolves a dimension to its world (null = not loaded).
        // `outNotResident` receives pending chunks that are no longer in the
        // cache (evicted under the LRU cap): they are dropped from the pending
        // set and the caller must request them again, or they never arrive.
        void SendNextChunks(const std::function<Game::World*(Game::DimensionId)>& worldFor,
                            std::vector<PendingChunkRef>* outNotResident = nullptr);

        // Handle client's batch acknowledgment (updates send rate)
        void OnChunkBatchAck(float desiredRate);

        // Chunk sender getters
        size_t GetPendingChunksToSendCount() const;
        float GetDesiredChunksPerTick() const { return m_desiredChunksPerTick; }
        int GetUnackedBatches() const { return m_unackedBatches; }
        void OnChunkRequestFull(Game::DimensionId dimension, Game::Math::ChunkPos pos);
        size_t GetUnchangedSentCount() const { return m_unchangedSent; }
        size_t m_unchangedSent = 0;
        float GetBatchQuota() const { return m_batchQuota; }
        int GetMaxUnackedBatches() const { return m_maxUnackedBatches; }

        // Send unload packet for chunk
        void SendChunkUnload(Game::DimensionId dimension, Game::Math::ChunkPos chunk);

        // === BLOCK UPDATES ===

        // Queue block change diff
        void QueueBlockChange(int worldX, int worldY, int worldZ, Game::BlockID newBlock);
        
        // Queue multi-block change for a section
        void QueueSectionChanges(Game::Math::ChunkPos chunk, int section, 
                                const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes);
        
        // Process and send diffs for this tick
        void ProcessDiffs(SendScheduler* scheduler);

        // === PACKET HANDLING ===

        // Handle incoming packets (delegates gameplay to ServerPlayer)
        void HandlePlayerMove(const Network::PlayerMoveC2SPacket& packet);
        // Fall-distance + exhaustion accounting off the move packet (called
        // by HandlePlayerMove before the position write).
        void UpdateMovementStats(const Network::PlayerMoveC2SPacket& packet);
        void HandleBlockAction(const Network::BlockActionC2SPacket& packet);
        void HandleUseItemOn(const Network::UseItemOnC2SPacket& packet);  // Minecraft-correct naming
        // Use item in air — mirrors ServerGamePacketListenerImpl.handleUseItem
        // (ServerGamePacketListenerImpl.java:1329-1354) + the useItem game-mode
        // logic (ServerPlayerGameMode.java:290-327).
        void HandleUseItem(const Network::UseItemC2SPacket& packet);
        // Release-use / drop / swap-offhand — mirrors handlePlayerAction
        // (ServerGamePacketListenerImpl.java:1191-1248).
        void HandlePlayerAction(const Network::PlayerActionC2SPacket& packet);
        void HandleHeldItemChange(const Network::HeldItemChangeC2SPacket& packet);
        // Pick block / pick entity — MC ServerGamePacketListenerImpl
        // .handlePickItemFromBlock/Entity + tryPickItem, creative only here.
        void HandlePickItem(const Network::PickItemC2SPacket& packet);
        void HandleKeepAlive(const Network::KeepAliveC2SPacket& packet);
        void HandleInventoryClick(const Network::InventoryClickC2SPacket& packet);
        void HandleInventoryClose(const Network::InventoryCloseC2SPacket& packet);
        // Fly-state toggle — mirrors handlePlayerAbilities
        // (ServerGamePacketListenerImpl.java: only the FLYING bit is honored,
        // and only when mayFly; otherwise a corrective abilities resend).
        void HandlePlayerAbilities(const Network::PlayerAbilitiesC2SPacket& packet);

        // Send full 46-slot inventory snapshot to this player's client.
        void SendInventoryFull();

        // The `Item.use` dispatch for one hand — mirrors
        // ServerPlayerGameMode.useItem (ServerPlayerGameMode.java:290-327):
        // runs the per-item `use` callback (or the component-driven
        // Item_DefaultUse), then applies MC's result-stack rules. Called from
        // HandleUseItem (air use) and from HandleUseItemOn's tail step (MC's
        // client falls through useItemOn → useItem locally, Minecraft.java:1656;
        // we run the fallthrough server-side instead). Returns the item's
        // UseResult so HandleUseItemOn's fallthrough can ack correctly.
        Game::UseResult DispatchUseItem(uint32_t hand);
        
        // Helper for resyncing on placement failure
        void ResyncAndAck(const glm::ivec3& clicked, const glm::ivec3& target, uint32_t sequence);
        
        // Send packets to client
        void SendPositionSync(); // Send authoritative position
        // `stateIndex` is the block's index into its own state list. It is NOT
        // optional in practice: a resync that omits it tells the client the
        // block is at its DEFAULT state, so any oriented block the client
        // already had right — a furnace, a log, a leaf litter clump — visibly
        // snaps to north/y-axis the moment anything resyncs it.
        void SendBlockUpdate(const glm::ivec3& pos, Game::BlockID block, Game::BlockStateIndex stateIndex);
        void SendSingleBlockChange(const Network::BlockChangeS2CPacket& packet);
        void SendSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);
        void SendInventoryUpdate(int slot); // TODO: Implement with inventory system
        void AckInteraction(uint32_t sequence, bool success);

        // Container revision (MC AbstractContainerMenu.stateId). Bumped
        // whenever the server mutates the player's inventory or cursor, and
        // stamped onto every InventoryFullS2C so the client can tell a fresh
        // authoritative snapshot from a stale one.
        void BumpContainerState() { ++m_containerStateId; }

        // Diff the authoritative container against m_remote* (our model of
        // what the client believes) and send ONLY the slots that disagree,
        // updating the model as we go. Mirrors MC
        // AbstractContainerMenu.broadcastChanges + synchronizeCarriedToRemote.
        void BroadcastContainerChanges();
        uint32_t ContainerStateId() const { return m_containerStateId; }

        // Force the next BroadcastContainerChanges to resend MENU slot `index`
        // even though the authoritative stack has not changed. Needed when the
        // CLIENT mutated its copy on a prediction the server rejected: the
        // server's slot still matches m_remoteSlots, so the plain diff would
        // find nothing to correct and the bad prediction would stick.
        // MC's equivalent is RemoteSlot.force with a non-matching value.
        // Callers holding a player-inventory index must translate first — see
        // InvalidateRemoteInventorySlot.
        void InvalidateRemoteSlot(int index);
        // Same, for callers that only know a player-inventory index. No-op when
        // the open menu does not show that slot (a crafting table hides armour
        // and the offhand).
        void InvalidateRemoteInventorySlot(int inventoryIndex);

        // Open the container menu a block asked for during use dispatch (see
        // IUsePlayer::OpenMenu). Called right after the dispatch returns, so the
        // screen appears on the same packet round as the interaction ack.
        void FlushPendingMenuOpen();
        // Drop what a CreateFilledResult could not fit in the inventory
        // (MC player.drop inside ItemUtils.createFilledResult).
        void FlushPendingDrops();

        // A block asked to lay food on a campfire during use dispatch
        // (IUsePlayer::PlaceCampfireFood). Drained right after the dispatch
        // returns, alongside FlushPendingMenuOpen and for the same reason.
        void FlushPendingCampfireFood();

        // Push one block entity's current state to every watcher. Block
        // entities carry state the block id and state index cannot (chest
        // contents, campfire food), so a mutation that doesn't change the
        // block is invisible to clients until this goes out.
        void BroadcastBlockEntity(const glm::ivec3& pos, Game::BlockEntity* be);

        // Client block-prediction acknowledgement (MC
        // ServerGamePacketListenerImpl.ackBlockChangesUpTo). Interaction
        // handlers only RECORD the sequence; the packet is emitted by
        // FlushBlockChangeAck once this tick's block updates have gone out.
        // Sending it any earlier lets the client retire a prediction before
        // the correcting block update arrives, which shows up as a flicker.
        void AckBlockChangesUpTo(uint32_t sequence);
        void FlushBlockChangeAck();


        // Track packet acknowledgments
        void OnChunkSendComplete(Game::Math::ChunkPos chunk);
        void OnChunkUnloadComplete(Game::Math::ChunkPos chunk);

        // === STATISTICS ===

        struct Stats {
            // Streaming stats
            size_t chunksInWatch = 0;
            size_t chunksSent = 0;
            size_t chunksInFlight = 0;
            size_t chunksPending = 0;
            
            // Bandwidth stats
            size_t bytesOutThisTick = 0;
            size_t totalBytesOut = 0;
            float averageBytesPerTick = 0;
            
            // Diff stats
            size_t diffsQueued = 0;
            size_t diffsSent = 0;
            size_t diffsDropped = 0;
            
            // Timing stats
            float lastTickTime = 0;
            float averageTickTime = 0;
            
            // Connection stats
            float latency = 0;
            std::chrono::steady_clock::time_point lastKeepAlive;
        };
        
        Stats GetStats() const;
        void ResetStats();

        // === GETTERS ===

        uint32_t GetPlayerId() const { return m_playerId; }
        uint32_t GetConnectionId() const { return m_connectionId; }
        ServerPlayer* GetPlayer() const { return m_player; }
        ServerConnection* GetConnection() const { return m_connection; }
        State GetState() const { return m_state; }
        
        // Position getters (delegate to ServerPlayer if attached)
        glm::vec3 GetPosition() const;
        glm::vec2 GetRotation() const;
        int GetDimensionId() const;

        // The world this session's player is standing in.
        //
        // Every block read or write a session performs must go through here
        // rather than IntegratedServer::GetWorld(), which is the OVERWORLD's:
        // a player in the Nether breaking a block would otherwise edit the
        // Overworld at the same x/y/z — and the very first thing they do in
        // the Nether is light a portal to come home.
        //
        // Null only before the level exists, which cannot happen for a session
        // that has a player attached.
        Game::World* SessionWorld() const;

        // The world a block interaction addresses, and the eye position its
        // reach is measured from. With immersive portals a client may dig or
        // place in the level BEHIND a portal it is standing in front of; the
        // packet carries that level's id (kDimensionUnknown = the player's
        // own). For another level the eye is the player's eye mapped through
        // the nearest portal that leads there, so the ordinary reach test
        // applies unchanged. Null when no such portal is near the player —
        // a client cannot reach into a level it has no surface into.
        //
        // Also latches the world for the block resyncs the handler may send
        // (ResyncAndAck / SendSingleBlockChange) until the returned guard
        // goes out of scope.
        struct InteractionScope {
            PlayerSession* session = nullptr;
            ~InteractionScope() { if (session) session->m_interactionWorld = nullptr; }
        };
        // `portalSearchRadius`: how far from the player's eye a portal they
        // may be reaching through can be. Zero means block reach; the
        // portal gun's shot travels its whole range before it lands, so a
        // gun impact may have come through a portal that far away.
        Game::World* InteractionWorld(int8_t packetDimension, const glm::ivec3& target,
                                      glm::vec3& outEye, InteractionScope& scope,
                                      double portalSearchRadius = 0.0);
        
        // View management getters
        Game::Math::ChunkPos GetChunkPosition() const { return m_currentChunk; }
        Game::Math::ChunkPos GetAnchorChunk() const { return m_anchorChunk; }

        
        int GetViewDistance() const { return m_viewDistance; }
        int GetSimulationDistance() const { return m_simulationDistance; }

        // False between construction and Initialize(). A session is PUBLISHED
        // by PlayerSessionManager::CreateSession and only initialized a few
        // statements later, and the client's ClientConfigC2S regularly lands
        // in that window — it is sent the instant LoginSuccess is received.
        // Initialize() resets both distances from its Config, so anything
        // applied to a session that is not yet initialized is thrown away.
        bool IsInitialized() const { return m_initialized; }

    private:
        // The world's dropped-item store, or null if the server isn't up.
        // Every "this produced an item in the world" path in this class goes
        // through it, so it is worth one accessor rather than repeating the
        // g_integratedServer null-dance at each site.
        ItemEntityManager* ItemEntitiesOrNull() const;

        // Throw `stack` into the world from this player's hand, along their
        // look direction (MC LivingEntity.drop with thrownFromHand). Shared by
        // the Q keybind and the container THROW / click-outside paths. No-op on
        // an empty stack.
        void DropItemFromPlayer(const Game::ItemStack& stack);

        // Spawn experience orbs in the world (MC Block.popExperience /
        // ExperienceOrb.award). No-op for amount <= 0 or before the server's
        // orb manager exists.
        void AwardWorldExperience(const glm::dvec3& pos, int amount);
        // Round a furnace's banked float XP MC's way (floor + random chance
        // at the remainder) and spawn the orbs — createExperience.
        void AwardBankedExperience(const glm::dvec3& pos, float banked);

        // === IDENTIFIERS ===
        uint32_t m_playerId;
        uint32_t m_connectionId;
        
        // === REFERENCES ===
        ServerPlayer* m_player = nullptr;        // Non-owning pointer to player entity
        ServerConnection* m_connection = nullptr; // Non-owning pointer to network connection
        SendScheduler* m_sendScheduler = nullptr; // For sending packets (set by PlayerSessionManager)
        
        // === STATE ===
        std::atomic<State> m_state{State::CONNECTING};
        Config m_config;

        // Loot rolls for this player's block breaks. Per-session rather than
        // global so two players mining at once can't interleave into each
        // other's sequence; seeded from the player id and the clock because MC
        // seeds block loot from the level's RandomSource, not a fixed value.
        Game::JavaRandom m_lootRandom{
            static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
        
        // === VIEW POSITION ===
        // Note: Authoritative position is in ServerPlayer, these are for view management
        Game::Math::ChunkPos m_currentChunk{0, 0};
        // See InteractionWorld. Null outside a block interaction that
        // targets another level.
        Game::World*      m_interactionWorld = nullptr;
        Game::DimensionId m_interactionDimension = Game::DimensionId::Overworld;
        Game::Math::ChunkPos m_anchorChunk{0, 0}; // Center for watch calculations
        Game::Math::ChunkPos m_lastKnownChunk{0, 0};
        
        // === DISTANCES ===
        int m_simulationDistance = 8;
        int m_viewDistance = 8;
        // See IsInitialized().
        bool m_initialized = false;
        
        // === CHUNK TRACKING ===
        // MC ServerPlayer.chunkTrackingView, generalised: the session holds a
        // SET of loaders (its own view plus one per portal far side), and the
        // union of their views is the tracked set. `m_watched` is that union,
        // materialised so the diff between two loader sets — which may overlap
        // in any way — is a set difference and nothing cleverer. Starts EMPTY
        // so the first update emits the whole initial set through the
        // ordinary enter path.
        std::vector<ChunkLoader> m_loaders;
        std::unordered_set<DimChunkKey, DimChunkKeyHash> m_watched;

        // Per-dimension send state. A client holds one level per dimension,
        // so "sent", "pending" and "what stamp does the client have" are
        // questions with a different answer in each world.
        struct DimensionSendState {
            std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> sent;
            // Chunk::modStamp of the last FULL ChunkDataS2C sent for each
            // chunk. Survives DropChunk on purpose: it is what lets a chunk
            // re-entering the view go out as ChunkUnchangedS2C.
            std::unordered_map<Game::Math::ChunkPos, uint64_t, Game::Math::ChunkPosHash> clientStamps;
            // MC PlayerChunkSender.pendingChunks. No "waiting for load" set:
            // a chunk that is not loaded when it enters view is simply not
            // queued, and is pushed here when generation finishes.
            std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> pending;
        };
        std::array<DimensionSendState, Game::kDimensionCount> m_dimState;
        DimensionSendState&       Dim(Game::DimensionId d)       { return m_dimState[Game::DimensionSlot(d)]; }
        const DimensionSendState& Dim(Game::DimensionId d) const { return m_dimState[Game::DimensionSlot(d)]; }
        // Squared chunk distance from `pos` to the nearest loader centre in
        // `dimension` — the send order (nearest first, per the mod).
        int DistanceSqToNearestLoader(Game::DimensionId dimension, Game::Math::ChunkPos pos) const;
        // Drop everything sent/pending for one dimension WITHOUT telling the
        // client (it is freeing the whole level). Loaders there are removed.
        std::set<Game::DimensionId> m_globalPortalsSynced;
        void ForgetDimensionSilently(Game::DimensionId dimension);

        // === CHUNK SENDER STATE (Minecraft's PlayerChunkSender) ===
        float m_desiredChunksPerTick = 9.0f;
        float m_batchQuota = 0.0f;
        int m_unackedBatches = 0;
        int m_maxUnackedBatches = 1;  // Bumps to 10 after first ack

        // === DIFF MANAGEMENT ===
        std::unordered_map<Game::Math::ChunkPos, 
                          std::unordered_map<int, SectionDiffs>, 
                          Game::Math::ChunkPosHash> m_pendingDiffs;
        std::queue<std::pair<Game::Math::ChunkPos, int>> m_diffQueue; // (chunk, section) pairs
        
        // === BUDGETS ===
        size_t m_bytesOutThisTick = 0;
        size_t m_chunksOutThisTick = 0;
        size_t m_diffsOutThisTick = 0;
        
        // === STATISTICS ===
        mutable std::mutex m_statsMutex;
        Stats m_stats;
        std::chrono::steady_clock::time_point m_lastTickTime;
        
        // === TIMING ===
        std::chrono::steady_clock::time_point m_lastKeepAliveRx;
        std::chrono::steady_clock::time_point m_lastKeepAliveTx;
        int64_t m_lastServerTick = 0;
        
        // === INTERACTION TRACKING ===
        uint32_t m_lastInteractionSequence = 0;  // For acknowledging client predictions

        // Highest interaction sequence processed since the last ack flush.
        // 0 = nothing to ack (client sequences start at 1).
        uint32_t m_ackBlockChangesUpTo = 0;

        // Monotonic container revision — see BumpContainerState.
        uint32_t m_containerStateId = 1;

        // Server-side model of the CLIENT's container state (MC
        // AbstractContainerMenu.remoteSlots / remoteCarried). Updated from
        // three places: what we send it, and what the client tells us it
        // predicted. Diffing the truth against this is what lets a correct
        // prediction cost zero packets while still catching a slot the client
        // wrongly wrote — the client reports that write, so the model shows
        // the disagreement.
        // Indexed by MENU slot, so it is resized whenever the open menu changes
        // (SendInventoryFull re-seeds it wholesale).
        std::vector<Game::ItemStack> m_remoteSlots{};
        Game::ItemStack              m_remoteCarried{};
        // The client's copy of the open menu's ContainerData (furnace burn +
        // cook timers). Diffed alongside the slots; see BroadcastContainerChanges.
        std::vector<int>             m_remoteData{};

        // Where the open block menu's container lives, and whether it is
        // block-backed at all. MC's ContainerLevelAccess + stillValid(): a menu
        // whose block has gone must be closed, because its slots and data
        // slots point straight INTO that block entity. See
        // CloseMenuIfBlockGone.
        bool       m_menuIsBlockBacked = false;
        glm::ivec3 m_openMenuPos{0, 0, 0};
        // The other half of an open double chest. Its block entity is half of
        // the menu's CompoundContainer, so it has to stay alive too.
        bool       m_hasMenuPartner = false;
        glm::ivec3 m_openMenuPartnerPos{0, 0, 0};

        // Returns true when the menu was closed because its block vanished.
        bool CloseMenuIfBlockGone();

        // Last-sent stat triple for the SetHealthS2C dirty-check — mirrors
        // ServerPlayer.lastSentHealth / lastSentFood / lastSaturationLevel.
        // Health init -1e8 forces a send on the first PLAYING tick.
        float m_lastSentHealth     = -1.0e8f;
        int   m_lastSentFood       = -1;
        float m_lastSentSaturation = -1.0f;

        // XP dirty-check cache (MC ServerPlayer.lastSentExp) — impossible
        // starting values force the first PLAYING tick to sync.
        int   m_lastSentXpLevel    = -1;
        float m_lastSentXpProgress = -1.0f;
        
        // === FLAGS ===
        bool m_isChangingDimension = false;
        bool m_isRespawning = false;

        // Set by PlayerPauseC2SPacket — see IsPaused above.
        bool m_paused = false;

        // MC ServerGamePacketListenerImpl.waitingForRespawn / clientLoadedTimeoutTimer.
        // See HasClientLoaded above. The initial values are MC's field defaults;
        // Initialize() immediately calls RestartClientLoadTimerAfterRespawn, the
        // same way MC's constructor does (:273).
        bool m_waitingForRespawn = false;
        int  m_clientLoadedTimeoutTimer = 0;
        // Edge detector for ServerPlayer.die() — our ServerPlayer has no
        // connection back-pointer, so the session watches the flag instead.
        bool m_wasPlayerDead = false;
        
        // === INTERNAL METHODS ===
        
        // Diff coalescing
        void CoalesceBlockChange(Game::Math::ChunkPos chunk, int section,
                                uint8_t localX, uint8_t localY, uint8_t localZ,
                                Game::BlockID blockId, Game::BlockStateIndex stateIndex = 0);
        
        // Packet size estimation
        size_t EstimatePacketSize(const Network::ChunkDataS2CPacket& packet) const;
        size_t EstimatePacketSize(const Network::MultiBlockChangeS2CPacket& packet) const;
        
        // Cleanup helpers
        void ClearWatchSets();
        void ClearQueues();
        void ClearDiffs();
    };

} // namespace Server