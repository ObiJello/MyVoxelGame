// File: src/client/input/PlayerController.hpp
#pragma once

#include "../entity/Player.hpp"
#include "common/core/Features.hpp"
#include "common/physics/RayCast.hpp"
#include <chrono>
#include <glm/glm.hpp>
#if ENABLE_PORTAL_GUN
#include <vector>
#endif

// Forward declarations
namespace Client {
    class NetworkClient;
}
namespace Network {
    enum class BlockActionType : uint8_t;
    enum class PlayerAction : uint8_t;
}

namespace Game {

    // Forward declaration
    class World;
    class IBlockAccess;

    // Client-side player controller that handles interaction and (future) networking
    class ClientPlayerController {
    public:
        // Configuration
        static constexpr float INTERACTION_RANGE = 5.0f;
        // MC's continuous-mining tick rate (20 TPS). The mining state machine
        // advances once per tick (not per frame) so the break time is framerate-
        // independent. Mirrors MultiPlayerGameMode's per-tick continueDestroyBlock.
        static constexpr float TICK_DT = 1.0f / 20.0f;
        // Post-break delay — MC's MultiPlayerGameMode.destroyDelay = 5
        // would gate held-mining for 250ms after a break, but in practice
        // that gap is perceptible and feels worse than what MC delivers
        // for the player (creative breaks chain cleanly; survival on soft
        // blocks shouldn't stutter). Run with 0 so the next held-mine
        // starts on the very next tick.
        static constexpr int POST_BREAK_DELAY_TICKS = 0;
        // Creative-mode instant break cadence. MC's MultiPlayerGameMode sets
        // destroyDelay = 5 on every creative destroy (both the startDestroy
        // click path and the continueDestroy held path), so held-LMB in
        // creative clears 4 blocks/second while a fresh click is always
        // instant. Unlike POST_BREAK_DELAY_TICKS this one is kept at MC's
        // value — it's what gives creative mining its familiar rhythm.
        static constexpr int CREATIVE_BREAK_DELAY_TICKS = 5;
        // MC's de-facto continuous-place interval (the cycle from item-use
        // animation reset → next BlockItem.useOn in a held-RMB strip). At 20
        // TPS that's 0.2 s between placements.
        static constexpr int PLACE_REFIRE_TICKS = 4;
        // Continuous-mining arm-swing pump. MC's Minecraft.continueAttack
        // calls player.swing() EVERY FRAME during held-mining and lets
        // LivingEntity.swing()'s half-duration gate decide the actual
        // re-trigger rate (~every 3 ticks at default swing duration 6).
        // Mirror that by flagging a trigger every tick; HeldItemRenderer
        // handles the gating.
        static constexpr int MINE_SWING_TICKS = 1;

        ClientPlayerController();

        // Set references (must be called after creation)
        void SetPlayer(ClientPlayer* player);
        void SetWorld(World* world);
        void SetNetworkClient(Client::NetworkClient* networkClient);

        // Block reader for interaction logic. REQUIRED in both modes — the
        // integrated host passes the server World, a remote client passes
        // ClientBlockAccess (the client chunk cache). Before this existed the
        // controller read blocks through `world`, which is null on a remote
        // client: every lookup returned Air, so mining read Air's destroyTime
        // of 0 and finished in one tick no matter what the block actually was.
        void SetBlockAccess(const IBlockAccess* access);

        // Main update tick (call once per frame)
        void Tick(float deltaTime);

        // Input handlers, named and shaped after Minecraft.java's. The caller
        // drains discrete presses from the input event queue
        // (`while (Input::ConsumeClick(...)) StartAttack();`) and separately
        // feeds the held state to ContinueAttack — the same split MC uses in
        // handleKeybinds (Minecraft.java:1979-1999). Deriving "a click
        // happened" from a polled level is what let a click that dismissed a
        // screen break the block under the crosshair.
        void StartAttack();              // MC Minecraft.startAttack

        // Pick the mob under the crosshair and send an attack for it.
        // Returns true when an entity was hit, so the caller skips the block
        // path — MC's startAttack has the same either/or shape.
        // MC Minecraft.pick — which entity, if any, the crosshair is on.
        // Returns 0 for none. An entity BEHIND the block the crosshair also
        // hits does not count: MC clips the entity search at the block hit
        // distance and keeps whichever is nearer, because Minecraft.hitResult
        // is a single result, not one of each.
        //
        // Public because both the attack path and the block-highlight pass
        // have to agree on it — an outline drawn on a block you cannot mine is
        // the visible half of the same bug.
        int32_t PickEntity() const;

        // MC Minecraft.crosshairPickEntity, reduced to what the attack
        // indicator needs: is there a LIVING thing under the crosshair right
        // now. MC only shows the "charged" burst when there is something to
        // hit, which is what makes the indicator read as a targeting cue
        // rather than a bare timer.
        bool HasEntityUnderCrosshair() const { return PickEntity() != 0; }

        bool TryAttackEntity();
        void ContinueAttack(bool down);  // MC Minecraft.continueAttack
        void StartUseItem();             // MC Minecraft.startUseItem
        void StopUseItem();              // RMB release

        // MC Minecraft.missTime — set to a large value while a screen is open
        // and decremented each tick, so an attack can't fire out of a UI frame
        // even if a click somehow leaked through. Reset by ContinueAttack(false).
        void SetMissTime(int ticks) { missTime = ticks; }
        
        // Hotbar selection
        void OnHotbarChanged(int slot);

        // Send a PlayerActionC2S (RELEASE_USE_ITEM / DROP_ITEM /
        // SWAP_ITEM_WITH_OFFHAND / …). Public so PlatformMain's keybinds
        // (Q = drop, F = swap offhand) can fire actions directly.
        void SendPlayerAction(Network::PlayerAction action);

        // Pick-block (P key) — server-authoritative. Predictively fills the
        // selected hotbar slot with a full stack of `picked` and sends
        // InventoryClickC2S {CREATIVE_FILL_SLOT} so the server's inventory
        // matches. Without the server round-trip the slot stays empty on the
        // server side and every subsequent placement / inventory-click on it
        // fails silently while the client's predictive count drifts.
        void OnPickBlock(BlockID picked);
        
        // Player commands
        void OnRespawnRequest();  // TODO: Implement for multiplayer
        
        // Check if currently breaking a block
        bool IsBreaking() const { return digState.isDestroying; }

        // Get breaking progress (0.0 to 1.0)
        float GetBreakProgress() const { return digState.destroyProgress; }

        // Current destroy stage [-1..9] (-1 = hidden, 0..9 = crack overlay frame).
        // Used by BlockBreakOverlay to draw the crumbling texture.
        int GetDestroyStage() const;

        // Block being mined (only meaningful if IsBreaking()).
        glm::ivec3 GetBreakingPos() const { return digState.destroyBlockPos; }
        BlockID    GetBreakingBlockId() const { return digState.destroyingBlockId; }

        // True iff the player has finished at least one mining swing this tick
        // — read by PlatformMain to pump HeldItemRenderer.Tick(swing) at the
        // MC continuous-mine cadence (every MINE_SWING_TICKS).
        bool ConsumeMiningSwingTrigger();

        // Get player reference (for compatibility during refactor)
        ClientPlayer* GetPlayer() { return player; }
        const ClientPlayer* GetPlayer() const { return player; }

    private:
        // References
        ClientPlayer* player;
        World* world;
        const IBlockAccess* blockAccess = nullptr;  // Block reads (host: World, remote: client chunk cache)
        Client::NetworkClient* networkClient;  // Network client for sending packets

        // Mining state — mirrors MultiPlayerGameMode's fields exactly.
        struct DigState {
            bool       isDestroying    = false;
            glm::ivec3 destroyBlockPos {0, -64, 0};
            float      destroyProgress = 0.0f;
            int        destroyTicks    = 0;
            int        destroyDelay    = 0;       // ticks
            BlockID    destroyingBlockId = BlockID::Air;
            // Captured alongside the id, and for the same reason: the local
            // break prediction clears the cell before the server handles the
            // packet, so the server has no way to read either back. Loot
            // tables condition on the state (wheat drops wheat only at age=7).
            uint8_t    destroyingBlockState = 0;
            int        lastSwingTick   = -1000;   // for MINE_SWING_TICKS pump
        };
        DigState digState;
        bool breakButtonHeld = false;

        // Placing state — tick-based now, no wall-clock throttle (matches MC).
        bool placeButtonHeld = false;
        bool armSwingPending = false;       // set when continuous mining hits SWING_TICKS

        // Fixed-step tick accumulator (MC-style 20 TPS).
        float tickAccum = 0.0f;

        // MC Minecraft.missTime / rightClickDelay. missTime blocks StartAttack
        // entirely while positive; rightClickDelay is the 4-tick gap between
        // held-RMB re-fires (MC startUseItem sets it to 4).
        int missTime = 0;
        int rightClickDelay = 0;

        // Network state (placeholders for future implementation)
        std::chrono::steady_clock::time_point lastMoveSend;
        int moveSeq = 0;         // Movement sequence number
        int interactSeq = 0;     // Interaction sequence number
        bool sentPlayerLoaded = false;  // Track if we've sent initial spawn
        bool lastSentFlying = false;    // Fly state last shipped via PlayerAbilitiesC2S

        // Internal methods
        // Read a block from whichever source this session has (see
        // SetBlockAccess). Returns Air for unloaded/invalid positions.
        BlockID ReadBlock(const glm::ivec3& pos) const;
        // State index at `pos`, from the same source as ReadBlock. 0 for
        // unloaded/invalid positions and for accessors that don't track state.
        uint8_t ReadBlockState(const glm::ivec3& pos) const;
        // Live look angles in degrees, derived from lookDir (player->yaw/pitch
        // are stale — see the implementation comment).
        void LookAngles(float& yawDeg, float& pitchDeg) const;
        // Apply a predicted block change to the client's own chunk data so it
        // shows up this frame instead of a round trip later, and register it
        // with ClientChunkManager's prediction handler under `sequence` so the
        // server's ack can confirm or roll it back.
        void PredictBlock(const glm::ivec3& pos, BlockID newBlock, uint32_t sequence,
                          uint8_t stateIndex = 0);
        void SendMovementIfDue();  // TODO: Implement for networking
        void StartDig(const glm::ivec3& pos, int face);
        void AbortDig();
        void FinishDig();
        // Creative-mode destroy: removes the block outright, with no progress
        // accumulation and no destroyTime gate (so bedrock/obsidian go in one
        // click). Mirrors MC's `instabuild` branches in
        // MultiPlayerGameMode.startDestroyBlock / continueDestroyBlock.
        void CreativeDestroy(const glm::ivec3& pos);
        // Returns the interaction sequence the packet was stamped with (0 if
        // nothing was sent) so a matching block prediction can be filed.
        uint32_t SendUseItemOn(const RaycastHit& hit, int hand, bool altInteract = false);  // altInteract=true → left-click "use" semantics (PortalGun blue)

        // Raycast face numbering -> MC Direction ordinals. Shared by the
        // outgoing packet and the local placement prediction, so the two
        // cannot disagree about which face was clicked.
        static uint32_t OurFaceToMcFace(int ourFace);

        // Work out what a right-click on `hit` will place, if anything — the
        // client-side mirror of the placement half of
        // PlayerSession::HandleUseItemOn. Deliberately conservative: it only
        // returns true for plain BlockItem placement into an empty cell with
        // no block/item interaction in the way, because those are the cases
        // whose outcome the client can derive exactly. Everything else falls
        // back to the un-predicted (wait-for-server) path rather than risk
        // predicting a block the server won't place.
        //
        // `outState` receives the block-state index from the same shared
        // Game::ComputePlacementState the server uses, so an oriented block
        // predicts with the correct facing rather than snapping on the ack.
        bool ComputePredictedPlacement(const RaycastHit& hit,
                                       glm::ivec3& outPos, BlockID& outBlock,
                                       uint8_t& outState) const;

        // Run the shared block-use / item-useOn chain locally so its block
        // edits land this frame, exactly as MC does inside
        // MultiPlayerGameMode.performUseItemOn (MultiPlayerGameMode.java:322).
        // Any SetBlock the behaviours perform is captured as a prediction
        // under `sequence`. Returns true if the chain consumed the click, in
        // which case no placement prediction should follow — the same
        // short-circuit the server applies.
        bool PredictUseItemOn(const RaycastHit& hit, uint32_t hand, uint32_t sequence);

        // Air-click item use (bucket fill/empty). Mirrors the tail of MC's
        // MultiPlayerGameMode.useItem prediction block.
        void PredictUseItem(uint32_t hand, uint32_t sequence);
        // Use item in air — sends UseItemC2S (MC ServerboundUseItemPacket).
        // Returns the interaction sequence it was stamped with (0 if not sent).
        uint32_t SendUseItem(int hand);
        // Which hand a right-click "use" should act on — MC's
        // MAIN_HAND → OFF_HAND loop (offhand shield with a tool in main hand).
        uint32_t PickUseHand() const;
        // Start/stop the client-side predicted hold-to-use (mirrors MC's
        // client running startUsingItem locally). Start is a no-op when the
        // held stack has no use duration.
        void StartPredictedUse(uint32_t hand);
        void StopPredictedUse();
        // Per-tick countdown for the predicted use (called from Tick's 20 TPS
        // stepper, alongside UpdateBreakingTick/UpdatePlacingTick).
        void UpdateUsingTick();

#if ENABLE_PORTAL_GUN
        // True portal-gun projectile. Spawned at the eye, flies forward at
        // Portal's BLAST_SPEED (3000 HU/s = 57.15 m/s), max lifetime
        // sv_portal_projectile_delay = 0.5 s. Each frame we sweep a tiny
        // segment (speed × dt) via the existing Raycast and on first solid
        // hit we send UseItemOnC2S so the server places the portal at the
        // impact face. No portal placement happens until impact — so the
        // player can fire across long sight-lines, not just within the
        // melee-range raycast.
        struct PendingPortalProjectile {
            glm::vec3 origin;       // spawn point (eye)
            glm::vec3 direction;    // unit forward
            glm::vec3 currentPos;   // advanced each tick
            float     age = 0.0f;
            bool      isOrange = false;
            int       hand = 0;
        };
        std::vector<PendingPortalProjectile> m_pendingPortalProjectiles;
        void SpawnPortalProjectile(bool isOrange);
        void UpdatePendingPortalProjectiles(float deltaTime);
#endif

        // Helper methods (existing functionality)
        void UpdateBreakingTick();          // run once per 1/20s tick
        void UpdatePlacingTick();           // continuous-RMB placement
        void TryPlaceBlock();
        // Local (predicted) block breaking. `sequence` is the interaction id
        // the STOP_DESTROY packet was sent with, used to reconcile against the
        // server's BlockChangedAckS2C.
        void FinishBreaking(uint32_t sequence);
        bool CanPlaceBlockAt(const glm::ivec3& pos);
        void MarkSurroundingSectionsForRemesh(const glm::ivec3& worldPos);
        BlockID GetBreakingBlockType(const glm::ivec3& pos);
        // Send a START/STOP/ABORT_DESTROY packet to the server. Returns the
        // interaction sequence it was stamped with, which is what a matching
        // block prediction must be registered under (0 if nothing was sent).
        uint32_t SendDigPacket(Network::BlockActionType action,
                               const glm::ivec3& pos, BlockID blockId,
                               uint8_t blockState = 0);
    };

    // Typedef for compatibility during transition
    using PlayerController = ClientPlayerController;

} // namespace Game