// File: src/client/network/ClientPacketHandler.hpp
#pragma once

#include "common/core/Features.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/IPacketListener.hpp"
#include "client/ClientTickRateManager.hpp"
#include "client/world/ClientWeather.hpp"
#include <memory>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace Game {
    class ClientPlayer;
}

namespace Client {

    // Forward declarations
    class ClientChunkManager;
    class NetworkClient;
    class ClientConnection;

    // Handles incoming packets on the main thread
    // All methods here run on the render thread and can safely modify game state
    class ClientPacketHandler : public Network::IPacketListener {
    public:
        ClientPacketHandler();
        ~ClientPacketHandler();

        // Set client player reference for inventory sync
        void SetPlayer(Game::ClientPlayer* player) { m_player = player; }

        // MC's ClientPacketListener holds `this.connection` for exactly this
        // reason: several handlers need connection-owned state (world age,
        // spawn position, the local player id) or need to send a reply, like
        // the teleport ack. Set by NetworkClient when the connection is created.
        void SetConnection(ClientConnection* connection) { m_connection = connection; }
        
        // IPacketListener interface
        const char* getName() const override { return "ClientPacketHandler"; }
        
        // Visitor pattern packet handlers (called from packet apply())
        void onChunkDataS2C(const Network::ChunkDataS2CPacket& packet) override { handleChunkData(packet); }
        void onUnloadChunkS2C(const Network::UnloadChunkS2CPacket& packet) override { handleChunkUnload(packet); }
        void onChunkUnchangedS2C(const Network::ChunkUnchangedS2CPacket& packet) override { handleChunkUnchanged(packet); }
        void onLightUpdateS2C(const Network::LightUpdateS2CPacket& packet) override;
        void onChunksBiomesS2C(const Network::ChunksBiomesS2CPacket& packet) override;
        void onBlockChangeS2C(const Network::BlockChangeS2CPacket& packet) override { handleBlockChange(packet); }
        void onClientboundSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) override { handleSectionBlocksUpdate(packet); }
        void onMultiBlockChangeS2C(const Network::MultiBlockChangeS2CPacket& packet) override { handleMultiBlockChange(packet); }
        void onPlayerUpdateS2C(const Network::PlayerUpdateS2CPacket& packet) override { handlePlayerUpdate(packet); }
        void onRemoveEntitiesS2C(const Network::RemoveEntitiesS2CPacket& packet) override { handleRemoveEntities(packet); }
        void onItemEntitySpawnS2C(const Network::ItemEntitySpawnS2CPacket& packet) override { handleItemEntitySpawn(packet); }

        // ── Mob entities ───────────────────────────────────────────────────
        void onAddEntityS2C(const Network::AddEntityS2CPacket& packet) override { handleAddEntity(packet); }
        void onMoveEntityS2C(const Network::MoveEntityS2CPacket& packet) override { handleMoveEntity(packet); }
        void onEntityPositionSyncS2C(const Network::EntityPositionSyncS2CPacket& packet) override { handleEntityPositionSync(packet); }
        void onEntityPositionSyncBatchS2C(const Network::EntityPositionSyncBatchS2CPacket& packet) override { handleEntityPositionSyncBatch(packet); }
        void onSetEntityMotionS2C(const Network::SetEntityMotionS2CPacket& packet) override { handleSetEntityMotion(packet); }
        void onSetEntityDataS2C(const Network::SetEntityDataS2CPacket& packet) override { handleSetEntityData(packet); }
        void onEntityEventS2C(const Network::EntityEventS2CPacket& packet) override { handleEntityEvent(packet); }
        void onHurtAnimationS2C(const Network::HurtAnimationS2CPacket& packet) override { handleHurtAnimation(packet); }
        // ── Beds ───────────────────────────────────────────────────────────
        void onPlayerSleepS2C(const Network::PlayerSleepS2CPacket& packet) override { handlePlayerSleep(packet); }
        void onPlayerMountS2C(const Network::PlayerMountS2CPacket& packet) override;
        // ── Vehicles (VehiclePackets.hpp → Client::Vehicles) ────────────────
        void onSetPassengersS2C(const Network::SetPassengersS2CPacket& packet) override;
        void onMoveVehicleS2C(const Network::MoveVehicleS2CPacket& packet) override;
        void onVehicleDataS2C(const Network::VehicleDataS2CPacket& packet) override;
        // MC handleMountScreenOpen — a horse's / llama's / camel's /
        // nautilus's inventory screen (MountInventoryScreen).
        void onMountScreenOpenS2C(const Network::MountScreenOpenS2CPacket& packet) override;
        void onPlayerSwingS2C(const Network::PlayerSwingS2CPacket& packet) override;
        void onShoulderParrotsS2C(const Network::ShoulderParrotsS2CPacket& packet) override;
        // ── Signs ──────────────────────────────────────────────────────────
        void onOpenSignEditorS2C(const Network::OpenSignEditorS2CPacket& packet) override { handleOpenSignEditor(packet); }
        // MC handleOpenBook (BookPackets.hpp)
        void onOpenBookS2C(const Network::OpenBookS2CPacket& packet) override;
        // MC handleMerchantOffers (MerchantPackets.hpp)
        void onMerchantOffersS2C(const Network::MerchantOffersS2CPacket& packet) override;
        // Spectator mode — MC ClientPacketListener.handleSetCamera.
        void onSetCameraS2C(const Network::SetCameraS2CPacket& packet) override;
        // ── /control ───────────────────────────────────────────────────────
        void onControlS2C(const Network::ControlS2CPacket& packet) override;
        void onControlInputS2C(const Network::ControlInputPacket& packet) override;
        void onControlViewS2C(const Network::ControlViewPacket& packet) override;
        void onMorphHeldS2C(const Network::MorphHeldS2CPacket& packet) override;
        // MC handleUpdateMobEffect / handleRemoveMobEffect — the local
        // player's own effect list (MobEffectPackets.hpp).
        void onUpdateMobEffectS2C(const Network::UpdateMobEffectS2CPacket& packet) override;
        // MC handleSoundEvent / handleSoundEntityEvent (SoundPackets.hpp).
        void onSoundS2C(const Network::SoundS2CPacket& packet) override;
        void onLevelEventS2C(const Network::LevelEventS2CPacket& packet) override;
        void onJukeboxSongS2C(const Network::JukeboxSongS2CPacket& packet) override;
        // MC handleParticleEvent (ServerLevel.sendParticles).
        void onLevelParticlesS2C(const Network::LevelParticlesS2CPacket& packet) override;
        void onSelfParticleStateS2C(const Network::SelfParticleStateS2CPacket& packet) override;
        void onSoundEntityS2C(const Network::SoundEntityS2CPacket& packet) override;
        void onRemoveMobEffectS2C(const Network::RemoveMobEffectS2CPacket& packet) override;
        void onMorphPickupS2C(const Network::MorphPickupS2CPacket& packet) override;

        // ── End dragon fight ───────────────────────────────────────────────
        void onBossEventS2C(const Network::BossEventS2CPacket& packet) override { handleBossEvent(packet); }
        void onEndCrystalBeamS2C(const Network::EndCrystalBeamS2CPacket& packet) override { handleEndCrystalBeam(packet); }
        void onArmorStandDataS2C(const Network::ArmorStandDataS2CPacket& packet) override { handleArmorStandData(packet); }
        void onItemFrameDataS2C(const Network::ItemFrameDataS2CPacket& packet) override { handleItemFrameData(packet); }
        void onFishingHookDataS2C(const Network::FishingHookDataS2CPacket& packet) override;
        // MC handleMapItemData — one map's colour patch / decorations.
        void onMapItemDataS2C(const Network::MapItemDataS2CPacket& packet) override;
        // MC handleEntityLinkPacket — a leashed mob's holder.
        void onSetEntityLinkS2C(const Network::SetEntityLinkS2CPacket& packet) override { handleSetEntityLink(packet); }
        void onBodyArmorS2C(const Network::BodyArmorS2CPacket& packet) override { handleBodyArmor(packet); }
        void onFireworkRocketDataS2C(const Network::FireworkRocketDataS2CPacket& packet) override;
        void onUpdateAttributesS2C(const Network::UpdateAttributesS2CPacket& packet) override;

        // ── /tick state ────────────────────────────────────────────────────
        // Handled inline: both are two-field mirrors into the client's
        // TickRateManager with no other consequence, so routing them through a
        // handleX in the .cpp would add a hop and nothing else.
        void onTickingStateS2C(const Network::TickingStateS2CPacket& packet) override {
            g_clientTickRate.SetTickRate(packet.tickRate);
            g_clientTickRate.SetFrozen(packet.isFrozen);
        }
        void onTickingStepS2C(const Network::TickingStepS2CPacket& packet) override {
            g_clientTickRate.SetFrozenTicksToRun(packet.tickSteps);
        }

        void onChangeDimensionS2C(const Network::ChangeDimensionS2CPacket& packet) override {
            handleChangeDimension(packet);
        }

        // MC handleGameEvent: the weather branches (ClientWeather.hpp) and
        // PUFFER_FISH_STING (the sound at the local player).
        void onGameEventS2C(const Network::GameEventS2CPacket& packet) override {
            if (packet.event == Network::GameEventS2CPacket::kPufferFishSting) {
                handlePufferFishSting();
                return;
            }
            ClientWeather::OnGameEvent(packet.event, packet.param);
        }

        // Stream scope — the level the following packets are applied to.
        // Handled inline: it is one store, read by ClientConnection before
        // the next apply (see ClientLevels::BindForPacket).
        void onDimensionScopeS2C(const Network::DimensionScopeS2CPacket& packet) override;

        void onExplodeS2C(const Network::ExplodeS2CPacket& packet) override {
            handleExplode(packet);
        }

        void onItemEntityMoveS2C(const Network::ItemEntityMoveS2CPacket& packet) override { handleItemEntityMove(packet); }
        void onTakeItemEntityS2C(const Network::TakeItemEntityS2CPacket& packet) override { handleTakeItemEntity(packet); }
        void onXpOrbSpawnS2C(const Network::XpOrbSpawnS2CPacket& packet) override { handleXpOrbSpawn(packet); }
        void onXpOrbMoveS2C(const Network::XpOrbMoveS2CPacket& packet) override { handleXpOrbMove(packet); }
        void onSetExperienceS2C(const Network::SetExperienceS2CPacket& packet) override { handleSetExperience(packet); }
        void onDisconnect(const std::string& reason) override { handleDisconnect(reason); }
        void onKeepAlive(uint64_t id) override { handleKeepAlive(id); }
        void onChunkBatchStart() override { handleChunkBatchStart(); }
        void onChunkBatchFinished(int batchSize, uint32_t serverSendMicros) override {
            handleChunkBatchFinished(batchSize, serverSendMicros);
        }
        void onHotbarSyncS2C(const Network::HotbarSyncS2CPacket& packet) override { handleHotbarSync(packet); }
        void onInventoryFullS2C(const Network::InventoryFullS2CPacket& packet) override { handleInventoryFull(packet); }
        void onInventorySetSlotS2C(const Network::InventorySetSlotS2CPacket& packet) override { handleInventorySetSlot(packet); }
        void onInventorySetCarriedS2C(const Network::InventorySetCarriedS2CPacket& packet) override { handleInventorySetCarried(packet); }
        void onSetHeldSlotS2C(const Network::SetHeldSlotS2CPacket& packet) override { handleSetHeldSlot(packet); }
        void onOpenScreenS2C(const Network::OpenScreenS2CPacket& packet) override { handleOpenScreen(packet); }
        void onContainerSetDataS2C(const Network::ContainerSetDataS2CPacket& packet) override { handleContainerSetData(packet); }
        void onSetHealthS2C(const Network::SetHealthS2CPacket& packet) override { handleSetHealth(packet); }
        void onBlockChangedAckS2C(const Network::BlockChangedAckS2CPacket& packet) override { handleBlockChangedAck(packet); }
        void onPlayerAbilitiesS2C(const Network::PlayerAbilitiesS2CPacket& packet) override { handlePlayerAbilities(packet); }

        // ── The last packets off the raw-payload path ──────────────────────
        // Bodies live on ClientConnection (they read its world age, spawn
        // position and player id), which is the same split MC uses: the
        // listener dispatches, `this.connection` holds the state.
        void onChatMessageS2C(const Network::ChatMessageS2CPacket& packet) override;
        void onTimeUpdateS2C(const Network::TimeUpdateS2CPacket& packet) override;
        void onWorldSpawnS2C(const Network::WorldSpawnS2CPacket& packet) override;
        void onPlayerInfoS2C(const Network::PlayerInfoS2CPacket& packet) override;
        void onClientboundPlayerPosition(const Network::ClientboundPlayerPositionPacket& packet) override;
        void onBlockEntityDataS2C(const Network::BlockEntityDataS2CPacket& packet) override;
        void onBlockEntityRemoveS2C(const Network::BlockEntityRemoveS2CPacket& packet) override;
        // Block events (MC ClientboundBlockEventPacket). Intentionally still a
        // no-op — no animated block entities ship yet — but the dispatch is in
        // place for when ChestLidController lands.
        void onBlockEntityActionS2C(const Network::BlockEntityActionS2CPacket& packet) override;
#if ENABLE_IMMERSIVE_PORTALS
        void onImmersivePortalSyncS2C(const Network::ImmersivePortalSyncS2CPacket& packet) override { handleImmersivePortalSync(packet); }
        void onImmersivePortalRemoveS2C(const Network::ImmersivePortalRemoveS2CPacket& packet) override { handleImmersivePortalRemove(packet); }
#endif
        void onAoRegionsS2C(const Network::AoRegionsS2CPacket& packet) override { handleAoRegions(packet); }
#if ENABLE_PORTAL_GUN
        void onPortalSetS2C(const Network::PortalSetS2CPacket& packet) override    { handlePortalSet(packet); }
        void onPortalRemoveS2C(const Network::PortalRemoveS2CPacket& packet) override { handlePortalRemove(packet); }
        void onPortalTeleportFlashS2C(const Network::PortalTeleportFlashS2CPacket& packet) override { handlePortalTeleportFlash(packet); }
        void onPortalFizzleS2C(const Network::PortalFizzleS2CPacket& packet) override { handlePortalFizzle(packet); }
#endif
        void onSetChunkCacheRadiusS2C(int viewDistance) override { handleSetChunkCacheRadius(viewDistance); }
        void onCommandsS2C(const Network::CommandsS2CPacket& packet) override { handleCommands(packet); }
        void onWorldgenIdsS2C(const Network::WorldgenIdsS2CPacket& packet) override;

        // ========================================================================
        // PACKET HANDLERS (called via IPacket::apply on main thread)
        // ========================================================================
        
        // Chunk management
        void handleChunkData(const Network::ChunkDataS2CPacket& packet);
        void handleChunkUnchanged(const Network::ChunkUnchangedS2CPacket& packet);
        void handleChunkUnload(const Network::UnloadChunkS2CPacket& packet);
        
        // Block updates
        void handleBlockChange(const Network::BlockChangeS2CPacket& packet);
        void handleSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);
        void handleMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet);
        
        // Player updates
        void handlePlayerUpdate(const Network::PlayerUpdateS2CPacket& packet);

        // Entity removal (players AND dropped items — split by id range)
        void handleRemoveEntities(const Network::RemoveEntitiesS2CPacket& packet);

        // Dropped items
        void handleItemEntitySpawn(const Network::ItemEntitySpawnS2CPacket& packet);
        void handleAddEntity(const Network::AddEntityS2CPacket& packet);
        void handleMoveEntity(const Network::MoveEntityS2CPacket& packet);
        void handleEntityPositionSync(const Network::EntityPositionSyncS2CPacket& packet);
        void handleEntityPositionSyncBatch(const Network::EntityPositionSyncBatchS2CPacket& packet);
        void handleSetEntityMotion(const Network::SetEntityMotionS2CPacket& packet);
        void handleSetEntityData(const Network::SetEntityDataS2CPacket& packet);
        void handleEntityEvent(const Network::EntityEventS2CPacket& packet);
        void handleHurtAnimation(const Network::HurtAnimationS2CPacket& packet);
        void handlePlayerSleep(const Network::PlayerSleepS2CPacket& packet);
        void handleOpenSignEditor(const Network::OpenSignEditorS2CPacket& packet);
        void handleBossEvent(const Network::BossEventS2CPacket& packet);
        void handleEndCrystalBeam(const Network::EndCrystalBeamS2CPacket& packet);
        void handleArmorStandData(const Network::ArmorStandDataS2CPacket& packet);
        void handleItemFrameData(const Network::ItemFrameDataS2CPacket& packet);
        void handleSetEntityLink(const Network::SetEntityLinkS2CPacket& packet);
        void handleBodyArmor(const Network::BodyArmorS2CPacket& packet);
        void handleItemEntityMove(const Network::ItemEntityMoveS2CPacket& packet);
        void handleTakeItemEntity(const Network::TakeItemEntityS2CPacket& packet);
        void handleXpOrbSpawn(const Network::XpOrbSpawnS2CPacket& packet);
        void handleXpOrbMove(const Network::XpOrbMoveS2CPacket& packet);

        // World state
        void handleTimeUpdate(uint64_t worldAge, uint64_t timeOfDay);
        void handleWeatherChange(uint8_t weatherType, float intensity);
        
        // Connection management
        void handleLoginSuccess(uint32_t playerId, const std::string& playerName);
        void handleDisconnect(const std::string& reason);
        void handleKeepAlive(uint64_t id);
        
        // Player abilities + game mode
        void handlePlayerAbilities(const Network::PlayerAbilitiesS2CPacket& packet);
        void handleWorldSpawn(int32_t x, int32_t y, int32_t z);
        
        // Chat
        void handleChatMessage(const std::string& message, uint8_t position);

        // Chunk batch (adaptive rate control)
        void handleChunkBatchStart();
        void handleChunkBatchFinished(int batchSize, uint32_t serverSendMicros);

        // Inventory sync
        void handleHotbarSync(const Network::HotbarSyncS2CPacket& packet);
        void handleInventoryFull(const Network::InventoryFullS2CPacket& packet);
        void handleInventorySetSlot(const Network::InventorySetSlotS2CPacket& packet);
        void handleInventorySetCarried(const Network::InventorySetCarriedS2CPacket& packet);
        void handleSetHeldSlot(const Network::SetHeldSlotS2CPacket& packet);
        void handleOpenScreen(const Network::OpenScreenS2CPacket& packet);
        void handleContainerSetData(const Network::ContainerSetDataS2CPacket& packet);
        void handleSetHealth(const Network::SetHealthS2CPacket& packet);
        void handleSetExperience(const Network::SetExperienceS2CPacket& packet);
        void handleBlockChangedAck(const Network::BlockChangedAckS2CPacket& packet);

        // The server moved us to another dimension. Everything cached about
        // the world we were in has to go before the first chunk of the new one
        // lands, or a Nether chunk merges into an Overworld one at the same
        // x/z and an Overworld mob turns up standing in lava.
        void handleChangeDimension(const Network::ChangeDimensionS2CPacket& packet);
        void handleExplode(const Network::ExplodeS2CPacket& packet);
        // MC handleGameEvent PUFFER_FISH_STING: level.playSound(player, the
        // player's position, PUFFER_FISH_STING, NEUTRAL, 1, 1).
        void handlePufferFishSting();

#if ENABLE_IMMERSIVE_PORTALS
        // Immersive portals — forwards into the client mirror, see
        // client/portal/ClientImmersivePortals.hpp.
        void handleImmersivePortalSync(const Network::ImmersivePortalSyncS2CPacket& packet);
        void handleImmersivePortalRemove(const Network::ImmersivePortalRemoveS2CPacket& packet);
#endif
        // The occlusion wand's boxes: swap the mesher's list for that
        // dimension and remesh what they touch.
        void handleAoRegions(const Network::AoRegionsS2CPacket& packet);

#if ENABLE_PORTAL_GUN
        // Portal gun (server-broadcast pair state). Forwards into
        // ClientPortalManager — see client/portal/ClientPortalManager.hpp.
        void handlePortalSet(const Network::PortalSetS2CPacket& packet);
        void handlePortalRemove(const Network::PortalRemoveS2CPacket& packet);
        void handlePortalTeleportFlash(const Network::PortalTeleportFlashS2CPacket& packet);
        void handlePortalFizzle(const Network::PortalFizzleS2CPacket& packet);
#endif

        // View distance
        void handleSetChunkCacheRadius(int viewDistance);
        void handleCommands(const Network::CommandsS2CPacket& packet);

        // ========================================================================
        // STATISTICS
        // ========================================================================
        
        struct HandlerStats {
            uint64_t chunksReceived = 0;
            uint64_t chunksUnloaded = 0;
            uint64_t blockChanges = 0;
            uint64_t playerUpdates = 0;
            uint64_t packetsProcessed = 0;
            
            void reset() {
                chunksReceived = chunksUnloaded = blockChanges = 0;
                playerUpdates = packetsProcessed = 0;
            }
        };
        
        const HandlerStats& getStats() const { return m_stats; }
        void resetStats() { m_stats.reset(); }

        // Chunk batch rate getters for pipeline debug panel
        float GetDesiredChunksPerTick() const { return m_batchCalculator.getDesiredChunksPerTick(); }
        // Set by ClientConnection::DrainIncomingPackets around each packet it
        // applies: when the packet ARRIVED (its I/O-thread timestamp).
        void SetPacketReceivedAt(std::chrono::steady_clock::time_point received) { m_packetReceivedAt = received; }
        // Chunk packets received and not yet applied (the network thread counts
        // one in as it queues it, the drain one out as it applies it, decoded
        // or not): the estimator's backlog guard.
        void NoteChunkPacketReceived() { m_chunkBacklog.fetch_add(1, std::memory_order_relaxed); }
        void NoteChunkPacketApplied() { m_chunkBacklog.fetch_sub(1, std::memory_order_relaxed); }

        // Which server this connection talks to: only the safety cap differs
        // (512 chunks a tick from our own integrated server, 256 from a remote
        // one). The estimate itself is the same measurement either way.
        // OBEY_CHUNK_BUDGET_MS / OBEY_CHUNK_RATE_MAX override for A/B runs.
        void SetLocalServer(bool local);
        float GetAvgNanosPerChunk() const { return static_cast<float>(m_batchCalculator.bottleneckNanosPerChunk()); }

    private:
        HandlerStats m_stats;

        // Cached references (set during initialization). The chunk manager is
        // deliberately NOT cached: it is the bound level's, rebound per packet
        // (see ClientLevel.hpp), so every handler reads g_clientChunkManager.
        Game::ClientPlayer* m_player = nullptr;
        ClientConnection* m_connection = nullptr;   // MC ClientPacketListener.connection
        // Note: NetworkClient is accessed via g_networkClient global

        // Chunk batch rate calculator: Minecraft's ChunkBatchSizeCalculator
        // role (tell the server how many chunks a tick to send), measured per
        // stage instead of by wall time.
        //
        // Vanilla divides a batch's WALL time, start packet to finish packet,
        // by its size. That window also holds time that is no chunk's cost:
        // the server spreading the batch across its send phase, the batch
        // waiting for the next frame's drain, the client's own startup frames.
        // Here it read 45-425 us a chunk against a real ~3 us main-thread apply
        // and held a saved view to 250-380 chunks a tick (2026-09-26).
        //
        // A chunk passes three stages, and each is timed directly:
        //   link   - the batch's arrival span minus the server's own send span
        //            (a trailing field on ChunkBatchFinishedS2C): what the
        //            connection added. ~0 on loopback, the transfer time over
        //            a slow internet link.
        //   decode - measured on the ChunkDecodePool threads, which run in
        //            parallel (ChunkDecodeThreadCount()).
        //   apply  - measured on the main thread, which may spend budgetNanos
        //            of each tick on chunks (vanilla's 7 ms).
        // A tick can take as many chunks as the slowest stage can handle; the
        // client asks for that. If received chunks still pile up unapplied
        // (the machine got busy), the request shrinks with the backlog.
        struct ChunkBatchSizeCalculator {
            static constexpr double kTickNanos = 50.0e6;
            // The share of each stage the estimate may plan to use.
            static constexpr double kLinkShare = 0.9;
            static constexpr double kDecodeShare = 0.75;

            // Per-chunk costs, averaged over recent batches (a light average:
            // the measurements are direct, so they need little smoothing).
            double linkNanosPerChunk = 0.0;
            double decodeNanosPerChunk = 100000.0;   // before the first batch reports
            double applyNanosPerChunk = 20000.0;
            int samples = 0;

            // The batch in progress.
            std::chrono::steady_clock::time_point batchStartReceived;
            double batchDecodeNanos = 0.0;
            double batchApplyNanos = 0.0;

            double budgetNanos = 7.0e6;   // main-thread time per tick for chunks
            float rateMax = 256.0f;       // safety cap (vanilla 64)
            int decodeThreads = 1;
            int backlog = 0;              // chunk packets waiting when the batch finished

            void onBatchStart(std::chrono::steady_clock::time_point received) {
                batchStartReceived = received;
                batchDecodeNanos = 0.0;
                batchApplyNanos = 0.0;
            }
            void onChunkApplied(double decodeNanos, double applyNanos) {
                batchDecodeNanos += decodeNanos;
                batchApplyNanos += applyNanos;
            }

            void onBatchFinished(int batchSize, std::chrono::steady_clock::time_point received,
                                 uint32_t serverSendMicros, int waiting) {
                backlog = waiting;
                if (batchSize <= 0) return;
                const double n = static_cast<double>(batchSize);
                const double arrival = std::max(0.0,
                    std::chrono::duration<double, std::nano>(received - batchStartReceived).count());
                const double link = std::max(0.0, arrival - static_cast<double>(serverSendMicros) * 1000.0) / n;
                const double decode = batchDecodeNanos / n;
                const double apply = batchApplyNanos / n;
                if (samples == 0) {
                    linkNanosPerChunk = link;
                    decodeNanosPerChunk = decode;
                    applyNanosPerChunk = apply;
                } else {
                    const double w = static_cast<double>(std::min(samples, 3));
                    linkNanosPerChunk = (linkNanosPerChunk * w + link) / (w + 1.0);
                    decodeNanosPerChunk = (decodeNanosPerChunk * w + decode) / (w + 1.0);
                    applyNanosPerChunk = (applyNanosPerChunk * w + apply) / (w + 1.0);
                }
                ++samples;
            }

            // Chunks a tick each stage can take. Costs under 1 us are noise
            // (a loopback link, timer granularity) and do not limit.
            double linkCapacity() const {
                return linkNanosPerChunk > 1000.0 ? kTickNanos * kLinkShare / linkNanosPerChunk : 1.0e9;
            }
            double decodeCapacity() const {
                return kTickNanos * kDecodeShare * decodeThreads / std::max(decodeNanosPerChunk, 1000.0);
            }
            double applyCapacity() const {
                return budgetNanos / std::max(applyNanosPerChunk, 1000.0);
            }

            float getDesiredChunksPerTick() const {
                double desired = std::min({linkCapacity(), decodeCapacity(), applyCapacity()});
                // Backlog guard: more than two ticks' worth of chunks still
                // waiting means the estimate is ahead of this machine right now.
                if (backlog > 2.0 * desired) desired *= 2.0 * desired / backlog;
                return static_cast<float>(desired);
            }
            // The slowest stage's time per chunk (debug panel).
            double bottleneckNanosPerChunk() const {
                return kTickNanos / std::max(1.0, static_cast<double>(getDesiredChunksPerTick()));
            }
        };
        ChunkBatchSizeCalculator m_batchCalculator;
        std::chrono::steady_clock::time_point m_packetReceivedAt{};
        std::atomic<int> m_chunkBacklog{0};
    };

} // namespace Client