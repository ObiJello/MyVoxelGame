// File: src/client/entity/Player.hpp
#pragma once

#include "common/entity/Inventory.hpp"
#include "common/entity/PlayerColors.hpp"
#include "common/physics/RayCast.hpp"
#include "common/physics/Physics.hpp"
#include "../renderer/core/Camera.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <chrono>

namespace Game {

    // Forward declarations
    class World;
    struct IBlockAccess;


    // Player statistics tracking
    struct PlayerStats {
        int blocksPlaced = 0;
        int blocksBroken = 0;
        int lastPlacedBlockId = -1;
        int lastBrokenBlockId = -1;
        float totalDistanceTraveled = 0.0f;
        float totalPlayTime = 0.0f;
    };

    // Client-side player entity that holds authoritative visual state
    class ClientPlayer {
    public:
        ClientPlayer();

        // === Core State ===
        
        // Physics state (authoritative for client)
        PlayerPhysics physics;
        
        // Transform tracking for client-server sync
        glm::dvec3 serverPos{0.0, 67.0, 0.0};     // Last server-confirmed position
        glm::dvec3 predictedPos{0.0, 67.0, 0.0};  // Local predicted position
        float yaw = 0.0f;                         // Camera yaw (degrees)
        float pitch = 0.0f;                       // Camera pitch (degrees)
        
        // Visual smoothing (for interpolation)
        glm::dvec3 visualPos{0.0, 67.0, 0.0};     // Smoothed position for rendering
        float visualYaw = 0.0f;                   // Smoothed yaw
        float visualPitch = 0.0f;                 // Smoothed pitch

        // Stick-figure render color — set from launcher's --color CLI arg at startup,
        // sent to the server at handshake so other clients render this player in the
        // chosen color too. Default = the historical neon green.
        Game::PlayerColorId color = Game::PlayerColorId::Default;
        
        // === Player Attributes ===

        // Status — synced from the server via SetHealthS2C
        // (ClientPacketHandler::handleSetHealth); read by the HUD.
        int   health     = 20;
        int   food       = 20;
        float saturation = 5.0f;
        int   air = 300;           // TODO: Sync from server (ticks of air remaining)
        float stepHeight = 0.6f;   // How high the player can step up

        // Game mode + abilities — synced from the server via
        // PlayerAbilitiesS2C (handlePlayerAbilities). gameMode holds the
        // Server::GameMode raw value (0 survival, 1 creative, 2 adventure,
        // 3 spectator). physics.mayFly / physics.isFlying carry the flight
        // half so the physics step can read them without reaching back here.
        uint8_t gameMode     = 0;
        // False until the first PlayerAbilitiesS2C lands. MC can't observe
        // this window — Minecraft.gameMode is null until handleLogin, and the
        // Gui needs a level to render, so the mode is always known by the time
        // anything reads it. Our render loop starts as soon as the socket
        // connects, so callers that would otherwise act on the survival
        // default (notably the HUD's hearts/hunger block) must check this
        // first, or a creative player sees a flash of survival UI on join.
        bool    gameModeKnown = false;
        bool    invulnerable = false;
        bool    instabuild   = false;
        float   flyingSpeed  = 0.05f;  // MC Abilities.flyingSpeed (per-tick)

        bool IsCreative()  const { return gameMode == 1; }
        bool IsSpectator() const { return gameMode == 3; }

        // === Predicted item-use state ===
        // Client-side mirror of the server's hold-to-use lifecycle
        // (LivingEntity.useItem / useItemRemaining) — MC's client runs
        // startUsingItem locally when MultiPlayerGameMode.useItem succeeds,
        // and we do the same when the RMB press starts a use (held stack has
        // GetUseDuration() > 0). Drives the viewmodel eat/drink/block pose;
        // the server remains authoritative for the actual consume (its
        // InventorySetSlotS2C corrects any mispredict).
        bool             usingItem         = false;
        int              useItemRemaining  = 0;   // ticks left (counts down at 20 TPS)
        int              useItemDuration   = 0;   // total ticks (for pose progress)
        uint32_t         usingHand         = 0;   // 0 = main, 1 = off
        ItemUseAnimation useAnim           = ItemUseAnimation::NONE;
        
        // === Inventory ===
        Inventory inventory;
        
        // === Raycast Cache ===
        std::optional<RaycastHit> lastBlockHit;  // Cached result from per-frame raycast
        // Last camera-space forward vector — refreshed every frame by
        // UpdateRaycast(camera). Use this instead of yaw/pitch fields
        // when you need the live look direction; the yaw/pitch members
        // are stale because mouse-look writes camera.yaw/pitch directly
        // and only syncs back at teleport-style events.
        glm::vec3 lookDir{0.0f, 0.0f, 1.0f};
        // TODO: Add entity hit cache when entity system is implemented
        // std::optional<EntityHit> lastEntityHit;
        
        // === Input State ===
        glm::vec3 movementInput{0.0f};
        bool jumpPressed = false;
        bool jumpHeld = false;         // True while space is held (for water bobbing)
        bool sprintPressed = false;
        bool sneakPressed = false;

        // Double-tap-space flight toggle state (MC jumpTriggerTime = 7 ticks).
        static constexpr float FLY_DOUBLE_TAP_WINDOW = 0.35f;
        bool  jumpKeyWasDown = false;  // Raw held state last frame (true edge detect)
        float flyToggleTimer = 0.0f;   // Seconds left in the double-tap window

        // Jump happened since the last PlayerMoveC2S send (jump exhaustion).
        // Set in UpdatePhysics, consumed by the move-send in PlatformMain.
        bool jumpedSinceMoveSend = false;

        // Largest fall-landing distance since the last PlayerMoveC2S send
        // (client physics tracks exact ground contact — see
        // PlayerPhysics::fallDistance). Consumed by the move-send; the
        // server turns it into fall damage.
        float landedFallSinceMoveSend = 0.0f;
        
        // === Statistics ===
        PlayerStats stats;
        
        // === Public Methods ===
        
        // Initialize player state
        void Initialize();
        
        // Update physics simulation (accepts any IBlockAccess: World*, ClientBlockAccess*, etc.)
        void UpdatePhysics(float deltaTime, IBlockAccess* blockAccess);
        
        // Update raycast from camera position
        void UpdateRaycast(const Render::Camera& camera);
        
        // Update visual smoothing (lerp visual toward predicted)
        void UpdateVisual(float deltaTime);
        
        // Apply server position correction
        void ApplyServerCorrection(const glm::dvec3& pos, float newYaw, float newPitch);
        
        // Get current eye position (for camera)
        glm::vec3 GetEyePosition() const;
        
        // Get current eye height based on pose
        float GetEyeHeight() const;
        
        // Movement input setters
        void SetMovementInput(const glm::vec3& movement) { movementInput = movement; }
        void SetJumpPressed(bool pressed);
        void SetJumpHeld(bool held) { jumpHeld = held; }
        void SetSprintPressed(bool pressed) { sprintPressed = pressed; }
        void SetSneakPressed(bool pressed) { sneakPressed = pressed; }
        
        // Noclip control
        void ToggleNoclip();
        void SetNoclip(bool enabled);
        
        // Inventory management
        void SelectSlot(int slot);
        void SelectNextSlot();
        void SelectPreviousSlot();
        BlockID GetSelectedBlock() const { return inventory.GetSelectedBlock(); }
        int GetSelectedSlot() const { return inventory.GetSelectedSlot(); }
        
        // Statistics tracking
        void UpdateStatistics(float deltaTime);
        const PlayerStats& GetStats() const { return stats; }
        
    private:
        // Track last position for distance calculations
        glm::vec3 lastPosition{0.0f};
        
        // Smoothing parameters
        static constexpr float POSITION_SMOOTHING_FACTOR = 10.0f;  // How fast visual lerps to predicted
        static constexpr float ROTATION_SMOOTHING_FACTOR = 15.0f;  // How fast rotation lerps
    };

} // namespace Game