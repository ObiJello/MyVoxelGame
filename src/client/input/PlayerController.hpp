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

        // Main update tick (call once per frame)
        void Tick(float deltaTime);

        // Input handlers (to be called from main loop based on input)
        void OnLMB(bool pressed);  // Left mouse button (break)
        void OnRMB(bool pressed);  // Right mouse button (place)
        
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
        Client::NetworkClient* networkClient;  // Network client for sending packets

        // Mining state — mirrors MultiPlayerGameMode's fields exactly.
        struct DigState {
            bool       isDestroying    = false;
            glm::ivec3 destroyBlockPos {0, -64, 0};
            float      destroyProgress = 0.0f;
            int        destroyTicks    = 0;
            int        destroyDelay    = 0;       // ticks
            BlockID    destroyingBlockId = BlockID::Air;
            int        lastSwingTick   = -1000;   // for MINE_SWING_TICKS pump
        };
        DigState digState;
        bool breakButtonHeld = false;

        // Placing state — tick-based now, no wall-clock throttle (matches MC).
        bool placeButtonHeld = false;
        int  ticksSincePlace = 1000;       // big-on-startup so first click fires
        bool armSwingPending = false;       // set when continuous mining hits SWING_TICKS

        // Fixed-step tick accumulator (MC-style 20 TPS).
        float tickAccum = 0.0f;

        // Network state (placeholders for future implementation)
        std::chrono::steady_clock::time_point lastMoveSend;
        int moveSeq = 0;         // Movement sequence number
        int interactSeq = 0;     // Interaction sequence number
        bool sentPlayerLoaded = false;  // Track if we've sent initial spawn
        bool lastSentFlying = false;    // Fly state last shipped via PlayerAbilitiesC2S

        // Internal methods
        void SendMovementIfDue();  // TODO: Implement for networking
        void StartDig(const glm::ivec3& pos, int face);
        void AbortDig();
        void FinishDig();
        void SendUseItemOn(const RaycastHit& hit, int hand, bool altInteract = false);  // altInteract=true → left-click "use" semantics (PortalGun blue)
        // Use item in air — sends UseItemC2S (MC ServerboundUseItemPacket).
        void SendUseItem(int hand);
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
        void FinishBreaking();  // Local block breaking implementation
        bool CanPlaceBlockAt(const glm::ivec3& pos);
        void MarkSurroundingSectionsForRemesh(const glm::ivec3& worldPos);
        BlockID GetBreakingBlockType(const glm::ivec3& pos);
        // Send a START/STOP/ABORT_DESTROY packet to the server.
        void SendDigPacket(Network::BlockActionType action,
                           const glm::ivec3& pos, BlockID blockId);
    };

    // Typedef for compatibility during transition
    using PlayerController = ClientPlayerController;

} // namespace Game